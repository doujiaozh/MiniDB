#include "db_impl.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "platform.h"

namespace mdb {

Options::Options()
    : memtable_size_bytes(4 * 1024 * 1024),
      block_size_bytes(4096),
      compaction_trigger_files(4),
      bloom_bits_per_key(10),
      sync_on_write(false) {}

namespace {

const char* kManifestName = "MANIFEST";
const char* kManifestTempName = "MANIFEST.tmp";
const char* kManifestHeader = "MINIDB-MANIFEST-1";

std::string FormatFileNumber(uint64_t number) {
  char buffer[6];
  for (int i = 5; i >= 0; --i) {
    buffer[i] = static_cast<char>('0' + number % 10);
    number /= 10;
  }
  return std::string(buffer, 6);
}

void AppendUnsigned(std::string* out, uint64_t value) {
  char buffer[24];
  int length = 0;
  do {
    buffer[length++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);
  while (length > 0) out->push_back(buffer[--length]);
}

bool ParseUnsigned(const std::string& text, uint64_t* value) {
  if (text.empty()) return false;
  uint64_t result = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c < '0' || c > '9') return false;
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (result > (0xffffffffffffffffull - digit) / 10) return false;
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

bool ParseFileName(const std::string& name, uint64_t* number, std::string* extension) {
  size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0 || dot > 12) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < dot; ++i) {
    char c = name[i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = value;
  *extension = name.substr(dot + 1);
  return true;
}

bool EndsWith(const std::string& text, const char* suffix) {
  size_t length = std::strlen(suffix);
  if (text.size() < length) return false;
  return text.compare(text.size() - length, length, suffix) == 0;
}

}

DBImpl::DBImpl()
    : options_(),
      wal_number_(0),
      next_file_number_(1),
      last_seq_(0),
      shutdown_(false),
      compact_requested_(false),
      flush_count_(0),
      compaction_count_(0),
      get_count_(0),
      put_count_(0),
      delete_count_(0),
      opened_(false) {
  mem_ = std::make_shared<SkipList>();
  version_ = std::make_shared<Version>();
}

DBImpl::~DBImpl() {
  Close();
}

std::string DBImpl::SstPath(uint64_t number) const {
  return PathJoin(directory_, FormatFileNumber(number) + ".sst");
}

std::string DBImpl::WalPath(uint64_t number) const {
  return PathJoin(directory_, FormatFileNumber(number) + ".wal");
}

std::string DBImpl::ManifestPath() const {
  return PathJoin(directory_, kManifestName);
}

std::string DBImpl::ManifestTempPath() const {
  return PathJoin(directory_, kManifestTempName);
}

void DBImpl::SetError(const std::string& message) {
  std::lock_guard<std::mutex> lock(error_mu_);
  last_error_ = message;
}

const char* DBImpl::LastError() {
  std::lock_guard<std::mutex> lock(error_mu_);
  return last_error_.c_str();
}

bool DBImpl::WriteManifest(const std::vector<uint64_t>& files, uint64_t max_seq,
                           std::string* error) {
  std::string content;
  content.reserve(64 + files.size() * 16);
  content.append(kManifestHeader);
  content.push_back('\n');
  content.append("seq ");
  AppendUnsigned(&content, max_seq);
  content.push_back('\n');
  for (size_t i = 0; i < files.size(); ++i) {
    content.append("file ");
    AppendUnsigned(&content, files[i]);
    content.push_back('\n');
  }

  std::string temp_path = ManifestTempPath();
  FileWriter writer;
  if (!writer.Open(temp_path, false)) {
    if (error != nullptr) *error = "cannot open manifest temp file";
    return false;
  }
  if (!writer.Append(content.data(), content.size()) || !writer.Sync()) {
    writer.Close();
    RemoveFile(temp_path);
    if (error != nullptr) *error = "cannot write manifest";
    return false;
  }
  writer.Close();
  if (!RenameFile(temp_path, ManifestPath())) {
    RemoveFile(temp_path);
    if (error != nullptr) *error = "cannot rename manifest";
    return false;
  }
  return true;
}

bool DBImpl::LoadManifest(std::vector<uint64_t>* files, uint64_t* max_seq, bool* found,
                          std::string* error) {
  files->clear();
  *max_seq = 0;
  *found = false;

  FileReader reader;
  if (!reader.Open(ManifestPath())) return true;
  uint64_t size = reader.size();
  if (size == 0 || size > 16ull * 1024ull * 1024ull) {
    if (error != nullptr) *error = "manifest has invalid size";
    return false;
  }
  std::string data;
  data.resize(static_cast<size_t>(size));
  if (!reader.ReadAtExact(0, &data[0], data.size())) {
    if (error != nullptr) *error = "cannot read manifest";
    return false;
  }

  bool header_seen = false;
  size_t position = 0;
  while (position < data.size()) {
    size_t end = data.find('\n', position);
    if (end == std::string::npos) end = data.size();
    std::string line = data.substr(position, end - position);
    position = end + 1;
    while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) {
      line.erase(line.size() - 1);
    }
    if (line.empty()) continue;
    if (!header_seen) {
      if (line != kManifestHeader) {
        if (error != nullptr) *error = "manifest header mismatch";
        return false;
      }
      header_seen = true;
      continue;
    }
    if (line.compare(0, 4, "seq ") == 0) {
      uint64_t value = 0;
      if (!ParseUnsigned(line.substr(4), &value)) {
        if (error != nullptr) *error = "manifest sequence is invalid";
        return false;
      }
      *max_seq = value;
    } else if (line.compare(0, 5, "file ") == 0) {
      uint64_t value = 0;
      if (!ParseUnsigned(line.substr(5), &value)) {
        if (error != nullptr) *error = "manifest file number is invalid";
        return false;
      }
      files->push_back(value);
    } else {
      if (error != nullptr) *error = "manifest contains unknown entry";
      return false;
    }
  }
  if (!header_seen) {
    if (error != nullptr) *error = "manifest is empty";
    return false;
  }
  *found = true;
  return true;
}

bool DBImpl::Recover(std::string* error) {
  if (!EnsureDir(directory_)) {
    if (error != nullptr) *error = "cannot create database directory";
    return false;
  }

  std::vector<std::string> names;
  ListFiles(directory_, &names);
  std::vector<uint64_t> sst_numbers;
  std::vector<uint64_t> wal_numbers;
  uint64_t max_number = 0;

  for (size_t i = 0; i < names.size(); ++i) {
    const std::string& name = names[i];
    if (name == kManifestName) continue;
    if (name == kManifestTempName) {
      RemoveFile(PathJoin(directory_, name));
      continue;
    }
    uint64_t number = 0;
    std::string extension;
    if (!ParseFileName(name, &number, &extension)) continue;
    if (extension == "sst") {
      sst_numbers.push_back(number);
    } else if (extension == "wal") {
      wal_numbers.push_back(number);
    } else if (EndsWith(extension, ".tmp")) {
      RemoveFile(PathJoin(directory_, name));
      continue;
    } else {
      continue;
    }
    if (number > max_number) max_number = number;
  }

  std::sort(sst_numbers.begin(), sst_numbers.end());
  std::sort(wal_numbers.begin(), wal_numbers.end());

  std::vector<uint64_t> live_files;
  uint64_t persisted_seq = 0;
  bool manifest_found = false;
  if (!LoadManifest(&live_files, &persisted_seq, &manifest_found, error)) return false;
  if (!manifest_found) live_files = sst_numbers;
  std::sort(live_files.begin(), live_files.end());

  std::shared_ptr<Version> version(new Version());
  for (size_t i = 0; i < live_files.size(); ++i) {
    uint64_t number = live_files[i];
    std::shared_ptr<SSTableReader> reader(new SSTableReader());
    if (!reader->Open(SstPath(number), number)) {
      if (error != nullptr) *error = "cannot open sstable " + FormatFileNumber(number);
      return false;
    }
    version->files.push_back(reader);
    if (reader->max_seq() > persisted_seq) persisted_seq = reader->max_seq();
  }
  version->max_seq = persisted_seq;
  version_ = version;

  for (size_t i = 0; i < sst_numbers.size(); ++i) {
    if (!std::binary_search(live_files.begin(), live_files.end(), sst_numbers[i])) {
      RemoveFile(SstPath(sst_numbers[i]));
    }
  }

  mem_ = std::make_shared<SkipList>();
  uint64_t replay_max = 0;
  std::vector<std::string> wal_paths;
  for (size_t i = 0; i < wal_numbers.size(); ++i) {
    wal_paths.push_back(WalPath(wal_numbers[i]));
  }

  for (size_t i = 0; i < wal_paths.size(); ++i) {
    std::vector<WalRecord> records;
    uint64_t valid_bytes = 0;
    if (!LoadWalFile(wal_paths[i], &records, &valid_bytes)) {
      if (error != nullptr) *error = "cannot read wal " + wal_paths[i];
      return false;
    }
    uint64_t file_size = 0;
    if (GetFileSize(wal_paths[i], &file_size) && file_size > valid_bytes) {
      TruncateFile(wal_paths[i], valid_bytes);
    }
    for (size_t j = 0; j < records.size(); ++j) {
      if (records[j].seq <= persisted_seq) continue;
      mem_->Insert(records[j].key, records[j].seq, records[j].type, records[j].value);
      if (records[j].seq > replay_max) replay_max = records[j].seq;
    }
  }

  uint64_t base_seq = persisted_seq > replay_max ? persisted_seq : replay_max;
  last_seq_.store(base_seq, std::memory_order_relaxed);
  next_file_number_ = max_number + 1;

  if (mem_->Count() > 0) {
    if (!Flush(error)) return false;
    for (size_t i = 0; i < wal_paths.size(); ++i) RemoveFile(wal_paths[i]);
  }

  if (!wal_ || !wal_->IsOpen()) {
    if (!wal_numbers.empty()) {
      uint64_t newest = wal_numbers[wal_numbers.size() - 1];
      std::unique_ptr<WalWriter> writer(new WalWriter());
      if (!writer->Open(WalPath(newest), true)) {
        if (error != nullptr) *error = "cannot open wal for append";
        return false;
      }
      wal_ = std::move(writer);
      wal_number_ = newest;
      for (size_t i = 0; i + 1 < wal_numbers.size(); ++i) RemoveFile(WalPath(wal_numbers[i]));
    } else {
      uint64_t number = next_file_number_++;
      std::unique_ptr<WalWriter> writer(new WalWriter());
      if (!writer->Open(WalPath(number), false)) {
        if (error != nullptr) *error = "cannot create wal";
        return false;
      }
      wal_ = std::move(writer);
      wal_number_ = number;
    }
  }
  return true;
}

bool DBImpl::Open(const std::string& directory, const Options& options, std::string* error) {
  directory_ = directory;
  options_ = options;
  if (options_.memtable_size_bytes < 4096) options_.memtable_size_bytes = 4096;
  if (options_.block_size_bytes < 512) options_.block_size_bytes = 512;
  if (options_.compaction_trigger_files < 2) options_.compaction_trigger_files = 2;
  if (options_.bloom_bits_per_key < 1) options_.bloom_bits_per_key = 1;

  if (!Recover(error)) return false;

  shutdown_.store(false, std::memory_order_relaxed);
  compact_requested_.store(false, std::memory_order_relaxed);
  bg_thread_ = std::thread(&DBImpl::BackgroundLoop, this);
  opened_ = true;
  return true;
}

bool DBImpl::Flush(std::string* error) {
  std::lock_guard<std::mutex> maintenance_lock(maintenance_mu_);
  std::shared_ptr<SkipList> frozen;
  std::string old_wal_path;

  {
    std::lock_guard<std::mutex> write_lock(writer_mu_);
    bool rotate = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (imm_) {
        frozen = imm_;
      } else if (mem_ && mem_->Count() > 0) {
        frozen = mem_;
        imm_ = frozen;
        mem_ = std::make_shared<SkipList>();
        rotate = true;
      } else {
        return true;
      }
    }
    if (rotate) {
      uint64_t new_number = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);
        new_number = next_file_number_++;
      }
      std::unique_ptr<WalWriter> new_wal(new WalWriter());
      if (!new_wal->Open(WalPath(new_number), false)) {
        if (error != nullptr) *error = "cannot rotate wal";
        return false;
      }
      if (wal_) {
        old_wal_path = wal_->path();
        wal_->Close();
      }
      wal_ = std::move(new_wal);
      wal_number_ = new_number;
    }
  }

  uint64_t sst_number = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sst_number = next_file_number_++;
  }
  std::string sst_path = SstPath(sst_number);
  SSTableBuilder builder(sst_path, options_.block_size_bytes, options_.bloom_bits_per_key);
  {
    SkipList::Iterator iterator = frozen->NewIterator();
    while (iterator.Valid()) {
      if (!builder.Add(iterator.key(), iterator.value(), iterator.seq(), iterator.type())) {
        builder.Abandon();
        if (error != nullptr) *error = "cannot add entry to sstable";
        return false;
      }
      iterator.Next();
    }
  }

  uint64_t file_size = 0;
  uint64_t entry_count = 0;
  if (!builder.Finish(frozen->MaxSeq(), &file_size, &entry_count)) {
    builder.Abandon();
    if (error != nullptr) *error = "cannot finish sstable";
    return false;
  }

  std::shared_ptr<SSTableReader> reader(new SSTableReader());
  if (!reader->Open(sst_path, sst_number)) {
    RemoveFile(sst_path);
    if (error != nullptr) *error = "cannot open created sstable";
    return false;
  }

  std::vector<uint64_t> files;
  uint64_t new_max_seq = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < version_->files.size(); ++i) {
      files.push_back(version_->files[i]->file_number());
    }
    new_max_seq = version_->max_seq > frozen->MaxSeq() ? version_->max_seq : frozen->MaxSeq();
  }
  files.push_back(sst_number);

  if (!WriteManifest(files, new_max_seq, error)) {
    RemoveFile(sst_path);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    std::shared_ptr<Version> next(new Version());
    next->files = version_->files;
    next->files.push_back(reader);
    next->max_seq = new_max_seq;
    version_ = next;
    if (imm_ == frozen) imm_.reset();
  }

  if (!old_wal_path.empty()) RemoveFile(old_wal_path);
  flush_count_.fetch_add(1, std::memory_order_relaxed);
  compact_requested_.store(true, std::memory_order_relaxed);
  bg_cv_.notify_all();
  return true;
}

int DBImpl::WriteBatch(ValueType type, const std::string& key, const std::string& value) {
  uint64_t seq = last_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
  bool need_flush = false;

  {
    std::lock_guard<std::mutex> write_lock(writer_mu_);
    if (!wal_ || !wal_->IsOpen()) {
      SetError("wal is not open");
      return kError;
    }
    if (!wal_->Append(seq, type, key, value)) {
      SetError("wal append failed");
      return kError;
    }
    if (options_.sync_on_write && !wal_->Sync()) {
      SetError("wal sync failed");
      return kError;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      mem_->Insert(key, seq, type, value);
      if (mem_->ApproximateMemoryUsage() >= options_.memtable_size_bytes) need_flush = true;
    }
  }

  if (need_flush) {
    std::string error;
    if (!Flush(&error)) {
      SetError(error);
      return kError;
    }
  }
  return kOk;
}

int DBImpl::Put(const std::string& key, const std::string& value) {
  int status = WriteBatch(kTypeValue, key, value);
  if (status == kOk) put_count_.fetch_add(1, std::memory_order_relaxed);
  return status;
}

int DBImpl::Delete(const std::string& key) {
  int status = WriteBatch(kTypeDeletion, key, std::string());
  if (status == kOk) delete_count_.fetch_add(1, std::memory_order_relaxed);
  return status;
}

int DBImpl::Get(const std::string& key, std::string* value) {
  std::shared_ptr<SkipList> mem;
  std::shared_ptr<SkipList> imm;
  std::shared_ptr<Version> version;
  {
    std::lock_guard<std::mutex> lock(mu_);
    mem = mem_;
    imm = imm_;
    version = version_;
  }
  get_count_.fetch_add(1, std::memory_order_relaxed);

  LookupResult result;
  if (mem && mem->Get(key, &result)) {
    if (result.type == kTypeValue) {
      if (value != nullptr) *value = result.value;
      return kOk;
    }
    return kNotFound;
  }
  if (imm && imm->Get(key, &result)) {
    if (result.type == kTypeValue) {
      if (value != nullptr) *value = result.value;
      return kOk;
    }
    return kNotFound;
  }
  if (version) {
    for (size_t i = version->files.size(); i > 0; --i) {
      if (version->files[i - 1]->Get(key, &result)) {
        if (result.type == kTypeValue) {
          if (value != nullptr) *value = result.value;
          return kOk;
        }
        return kNotFound;
      }
    }
  }
  return kNotFound;
}

bool DBImpl::MaybeCompact(std::string* error) {
  std::shared_ptr<Version> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot = version_;
  }
  int trigger = options_.compaction_trigger_files;
  if (!snapshot || static_cast<int>(snapshot->files.size()) < trigger) return true;
  return Compact(error);
}

bool DBImpl::Compact(std::string* error) {
  std::lock_guard<std::mutex> maintenance_lock(maintenance_mu_);
  std::shared_ptr<Version> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot = version_;
  }
  if (!snapshot || snapshot->files.empty()) return true;

  uint64_t new_number = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    new_number = next_file_number_++;
  }

  std::string path = SstPath(new_number);
  SSTableBuilder builder(path, options_.block_size_bytes, options_.bloom_bits_per_key);

  struct Cursor {
    std::shared_ptr<SSTableReader> reader;
    std::unique_ptr<SSTableReader::Iterator> iterator;
    bool valid;

    Cursor() : valid(false) {}
  };

  std::vector<Cursor> cursors(snapshot->files.size());
  for (size_t i = 0; i < snapshot->files.size(); ++i) {
    cursors[i].reader = snapshot->files[i];
    cursors[i].iterator = snapshot->files[i]->NewIterator();
    cursors[i].valid = cursors[i].iterator->SeekToFirst();
  }

  std::string current_key;
  bool have_current = false;
  for (;;) {
    int best = -1;
    for (size_t i = 0; i < cursors.size(); ++i) {
      if (!cursors[i].valid) continue;
      if (best < 0) {
        best = static_cast<int>(i);
        continue;
      }
      int compare = CompareStrings(cursors[i].iterator->key(),
                                   cursors[static_cast<size_t>(best)].iterator->key());
      if (compare < 0) {
        best = static_cast<int>(i);
      } else if (compare == 0 &&
                 cursors[i].iterator->seq() >
                     cursors[static_cast<size_t>(best)].iterator->seq()) {
        best = static_cast<int>(i);
      }
    }
    if (best < 0) break;

    Cursor& cursor = cursors[static_cast<size_t>(best)];
    bool emit = false;
    if (!have_current || CompareStrings(cursor.iterator->key(), current_key) != 0) {
      current_key = cursor.iterator->key();
      have_current = true;
      emit = cursor.iterator->type() == kTypeValue;
    }
    if (emit) {
      if (!builder.Add(current_key, cursor.iterator->value(), cursor.iterator->seq(),
                       cursor.iterator->type())) {
        builder.Abandon();
        RemoveFile(path);
        if (error != nullptr) *error = "compaction add failed";
        return false;
      }
    }
    cursor.iterator->Next();
    if (!cursor.iterator->Valid()) cursor.valid = false;
  }

  uint64_t file_size = 0;
  uint64_t entry_count = 0;
  if (!builder.Finish(snapshot->max_seq, &file_size, &entry_count)) {
    builder.Abandon();
    RemoveFile(path);
    if (error != nullptr) *error = "compaction finish failed";
    return false;
  }

  std::shared_ptr<SSTableReader> reader(new SSTableReader());
  if (!reader->Open(path, new_number)) {
    RemoveFile(path);
    if (error != nullptr) *error = "cannot open compacted sstable";
    return false;
  }

  std::vector<uint64_t> files;
  files.push_back(new_number);
  if (!WriteManifest(files, snapshot->max_seq, error)) {
    RemoveFile(path);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    std::shared_ptr<Version> next(new Version());
    next->files.push_back(reader);
    next->max_seq = snapshot->max_seq;
    version_ = next;
  }

  for (size_t i = 0; i < snapshot->files.size(); ++i) {
    RemoveFile(snapshot->files[i]->path());
  }
  compaction_count_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void DBImpl::BackgroundLoop() {
  for (;;) {
    std::unique_lock<std::mutex> lock(bg_mu_);
    bg_cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
      return shutdown_.load(std::memory_order_relaxed) ||
             compact_requested_.load(std::memory_order_relaxed);
    });
    if (shutdown_.load(std::memory_order_relaxed)) break;
    bool requested = compact_requested_.exchange(false, std::memory_order_relaxed);
    lock.unlock();
    if (!requested) continue;
    std::string error;
    if (!MaybeCompact(&error) && !error.empty()) SetError(error);
  }
}

void DBImpl::GetStats(minidb_stats* stats) {
  if (stats == nullptr) return;
  std::memset(stats, 0, sizeof(*stats));
  std::lock_guard<std::mutex> lock(mu_);
  stats->memtable_entries = mem_ ? mem_->Count() : 0;
  stats->immutable_entries = imm_ ? imm_->Count() : 0;
  stats->sstable_count = version_ ? static_cast<unsigned long long>(version_->files.size()) : 0;
  uint64_t entries = 0;
  if (version_) {
    for (size_t i = 0; i < version_->files.size(); ++i) {
      entries += version_->files[i]->entry_count();
    }
  }
  stats->sstable_entries = entries;
  stats->wal_bytes = (wal_ && wal_->IsOpen()) ? wal_->size() : 0;
  stats->last_sequence = last_seq_.load(std::memory_order_relaxed);
  stats->flush_count = flush_count_.load(std::memory_order_relaxed);
  stats->compaction_count = compaction_count_.load(std::memory_order_relaxed);
  stats->get_count = get_count_.load(std::memory_order_relaxed);
  stats->put_count = put_count_.load(std::memory_order_relaxed);
}

void DBImpl::Close() {
  if (!opened_) {
    std::lock_guard<std::mutex> lock(mu_);
    if (wal_) {
      wal_->Close();
      wal_.reset();
    }
    return;
  }
  opened_ = false;
  shutdown_.store(true, std::memory_order_relaxed);
  bg_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();

  std::string error;
  bool flushed = Flush(&error);

  std::lock_guard<std::mutex> lock(mu_);
  if (wal_) {
    if (flushed) {
      std::string path = wal_->path();
      wal_->Sync();
      wal_->Close();
      RemoveFile(path);
    } else {
      wal_->Sync();
      wal_->Close();
    }
    wal_.reset();
  }
}

}

struct db_handle {
  mdb::DBImpl* impl;
};

extern "C" {

void db_options_init(minidb_options* options) {
  if (options == nullptr) return;
  options->memtable_size_bytes = 4u * 1024u * 1024u;
  options->sync_on_write = 0;
  options->compaction_trigger_files = 4;
  options->block_size_bytes = 4096;
  options->bloom_bits_per_key = 10;
}

minidb* db_open(const char* path, const minidb_options* options) {
  if (path == nullptr) return nullptr;
  minidb_options defaults;
  db_options_init(&defaults);
  const minidb_options* effective = options != nullptr ? options : &defaults;

  mdb::Options internal;
  internal.memtable_size_bytes =
      effective->memtable_size_bytes > 0 ? effective->memtable_size_bytes
                                         : defaults.memtable_size_bytes;
  internal.block_size_bytes =
      effective->block_size_bytes > 0 ? effective->block_size_bytes : defaults.block_size_bytes;
  internal.compaction_trigger_files = effective->compaction_trigger_files > 0
                                          ? effective->compaction_trigger_files
                                          : defaults.compaction_trigger_files;
  internal.bloom_bits_per_key = effective->bloom_bits_per_key > 0
                                    ? effective->bloom_bits_per_key
                                    : defaults.bloom_bits_per_key;
  internal.sync_on_write = effective->sync_on_write != 0;

  db_handle* handle = new db_handle();
  handle->impl = new mdb::DBImpl();
  std::string error;
  if (!handle->impl->Open(path, internal, &error)) {
    delete handle->impl;
    delete handle;
    return nullptr;
  }
  return handle;
}

int db_put(minidb* db, const char* key, size_t key_len, const char* value, size_t value_len) {
  if (db == nullptr || db->impl == nullptr) return DB_ERR;
  if (key == nullptr || key_len == 0) return DB_ERR;
  if (value == nullptr && value_len != 0) return DB_ERR;
  std::string key_text(key, key_len);
  std::string value_text;
  if (value_len != 0) value_text.assign(value, value_len);
  return db->impl->Put(key_text, value_text) == mdb::kOk ? DB_OK : DB_ERR;
}

int db_get(minidb* db, const char* key, size_t key_len, char** value, size_t* value_len) {
  if (db == nullptr || db->impl == nullptr) return DB_ERR;
  if (key == nullptr || key_len == 0 || value == nullptr || value_len == nullptr) return DB_ERR;
  *value = nullptr;
  *value_len = 0;
  std::string key_text(key, key_len);
  std::string result;
  int status = db->impl->Get(key_text, &result);
  if (status == mdb::kNotFound) return DB_NOT_FOUND;
  if (status != mdb::kOk) return DB_ERR;
  size_t bytes = result.size();
  char* buffer = static_cast<char*>(std::malloc(bytes == 0 ? 1 : bytes));
  if (buffer == nullptr) return DB_ERR;
  if (bytes != 0) std::memcpy(buffer, result.data(), bytes);
  *value = buffer;
  *value_len = bytes;
  return DB_OK;
}

void db_free_value(char* value) {
  std::free(value);
}

int db_delete(minidb* db, const char* key, size_t key_len) {
  if (db == nullptr || db->impl == nullptr) return DB_ERR;
  if (key == nullptr || key_len == 0) return DB_ERR;
  std::string key_text(key, key_len);
  return db->impl->Delete(key_text) == mdb::kOk ? DB_OK : DB_ERR;
}

int db_flush(minidb* db) {
  if (db == nullptr || db->impl == nullptr) return DB_ERR;
  std::string error;
  return db->impl->Flush(&error) ? DB_OK : DB_ERR;
}

int db_compact(minidb* db) {
  if (db == nullptr || db->impl == nullptr) return DB_ERR;
  std::string error;
  return db->impl->Compact(&error) ? DB_OK : DB_ERR;
}

void db_get_stats(minidb* db, minidb_stats* stats) {
  if (db == nullptr || db->impl == nullptr || stats == nullptr) return;
  db->impl->GetStats(stats);
}

const char* db_last_error(minidb* db) {
  if (db == nullptr || db->impl == nullptr) return "invalid database handle";
  return db->impl->LastError();
}

int db_close(minidb* db) {
  if (db == nullptr) return DB_ERR;
  if (db->impl != nullptr) {
    db->impl->Close();
    delete db->impl;
    db->impl = nullptr;
  }
  delete db;
  return DB_OK;
}

}
