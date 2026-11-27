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
      sync_on_write(false),
      level_base_size_bytes(1024 * 1024),
      level_size_multiplier(4),
      target_sstable_size_bytes(1024 * 1024) {}

Version::Version() : levels(kMaxLevels), max_seq(0) {}

namespace {

const char* kManifestName = "MANIFEST";
const char* kManifestTempName = "MANIFEST.tmp";
const char* kManifestHeader = "MINIDB-MANIFEST-2";
const char* kManifestHeaderLegacy = "MINIDB-MANIFEST-1";

std::string FormatFileNumber(uint64_t number) {
  char buffer[6];
  for (int i = 5; i >= 0; --i) {
    buffer[i] = static_cast<char>('0' + number % 10);
    number /= 10;
  }
  return std::string(buffer, 6);
}

std::string UIntToString(uint64_t value) {
  char buffer[24];
  int length = 0;
  do {
    buffer[length++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);
  std::string result;
  while (length > 0) result.push_back(buffer[--length]);
  return result;
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

void SplitSpaces(const std::string& line, std::vector<std::string>* tokens) {
  size_t position = 0;
  while (position < line.size()) {
    while (position < line.size() && line[position] == ' ') ++position;
    size_t start = position;
    while (position < line.size() && line[position] != ' ') ++position;
    if (position > start) tokens->push_back(line.substr(start, position - start));
  }
}

std::string HexEncode(const std::string& input) {
  static const char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(input.size() * 2);
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned char value = static_cast<unsigned char>(input[i]);
    output.push_back(digits[value >> 4]);
    output.push_back(digits[value & 15]);
  }
  return output;
}

struct ReaderLess {
  bool operator()(const std::shared_ptr<SSTableReader>& left,
                  const std::shared_ptr<SSTableReader>& right) const {
    int compare = CompareStrings(left->smallest_key(), right->smallest_key());
    if (compare != 0) return compare < 0;
    return left->file_number() < right->file_number();
  }
};

bool ContainsReader(const std::vector<std::shared_ptr<SSTableReader>>& readers,
                    const std::shared_ptr<SSTableReader>& target) {
  for (size_t i = 0; i < readers.size(); ++i) {
    if (readers[i].get() == target.get()) return true;
  }
  return false;
}

bool RangesOverlap(const std::string& left_smallest, const std::string& left_largest,
                   const std::string& right_smallest, const std::string& right_largest) {
  if (CompareStrings(left_largest, right_smallest) < 0) return false;
  if (CompareStrings(right_largest, left_smallest) < 0) return false;
  return true;
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

int DBImpl::Level0Trigger() const {
  return options_.compaction_trigger_files < 2 ? 2 : options_.compaction_trigger_files;
}

uint64_t DBImpl::LevelLimit(int level) const {
  uint64_t limit = options_.level_base_size_bytes;
  for (int i = 1; i < level; ++i) {
    limit *= static_cast<uint64_t>(options_.level_size_multiplier);
  }
  return limit;
}

uint64_t DBImpl::LevelSize(const Version& version, int level) const {
  if (level < 0 || level >= kMaxLevels) return 0;
  uint64_t total = 0;
  for (size_t i = 0; i < version.levels[level].size(); ++i) {
    total += version.levels[level][i]->file_size();
  }
  return total;
}

uint64_t DBImpl::AllocateFileNumber() {
  std::lock_guard<std::mutex> lock(mu_);
  return next_file_number_++;
}

std::vector<std::vector<uint64_t>> DBImpl::ManifestNumbers(const Version& version) const {
  std::vector<std::vector<uint64_t>> numbers(kMaxLevels);
  for (int level = 0; level < kMaxLevels; ++level) {
    for (size_t i = 0; i < version.levels[level].size(); ++i) {
      numbers[level].push_back(version.levels[level][i]->file_number());
    }
  }
  return numbers;
}

bool DBImpl::WriteManifest(const std::vector<std::vector<uint64_t>>& levels, uint64_t max_seq,
                           std::string* error) {
  std::string content;
  content.append(kManifestHeader);
  content.push_back('\n');
  content.append("seq ");
  AppendUnsigned(&content, max_seq);
  content.push_back('\n');
  for (int level = 0; level < static_cast<int>(levels.size()); ++level) {
    for (size_t i = 0; i < levels[level].size(); ++i) {
      content.append("file ");
      AppendUnsigned(&content, static_cast<uint64_t>(level));
      content.push_back(' ');
      AppendUnsigned(&content, levels[level][i]);
      content.push_back('\n');
    }
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

bool DBImpl::LoadManifest(std::vector<std::vector<uint64_t>>* levels, uint64_t* max_seq,
                          bool* found, std::string* error) {
  levels->assign(kMaxLevels, std::vector<uint64_t>());
  *max_seq = 0;
  *found = false;

  FileReader reader;
  if (!reader.Open(ManifestPath())) return true;
  uint64_t size = reader.size();
  if (size == 0 || size > 32ull * 1024ull * 1024ull) {
    if (error != nullptr) *error = "manifest has invalid size";
    return false;
  }
  std::string data;
  data.resize(static_cast<size_t>(size));
  if (!reader.ReadAtExact(0, &data[0], data.size())) {
    if (error != nullptr) *error = "cannot read manifest";
    return false;
  }

  int version = 0;
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
    if (version == 0) {
      if (line == kManifestHeader) {
        version = 2;
      } else if (line == kManifestHeaderLegacy) {
        version = 1;
      } else {
        if (error != nullptr) *error = "manifest header mismatch";
        return false;
      }
      continue;
    }
    std::vector<std::string> tokens;
    SplitSpaces(line, &tokens);
    if (tokens.size() == 2 && tokens[0] == "seq") {
      uint64_t value = 0;
      if (!ParseUnsigned(tokens[1], &value)) {
        if (error != nullptr) *error = "manifest sequence is invalid";
        return false;
      }
      *max_seq = value;
    } else if (tokens.size() == 2 && tokens[0] == "file" && version == 1) {
      uint64_t number = 0;
      if (!ParseUnsigned(tokens[1], &number)) {
        if (error != nullptr) *error = "manifest file number is invalid";
        return false;
      }
      (*levels)[0].push_back(number);
    } else if (tokens.size() == 3 && tokens[0] == "file") {
      uint64_t level = 0;
      uint64_t number = 0;
      if (!ParseUnsigned(tokens[1], &level) || !ParseUnsigned(tokens[2], &number) ||
          level >= static_cast<uint64_t>(kMaxLevels)) {
        if (error != nullptr) *error = "manifest file entry is invalid";
        return false;
      }
      (*levels)[static_cast<int>(level)].push_back(number);
    } else {
      if (error != nullptr) *error = "manifest contains unknown entry";
      return false;
    }
  }
  if (version == 0) {
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

  std::vector<std::vector<uint64_t>> level_numbers;
  uint64_t persisted_seq = 0;
  bool manifest_found = false;
  if (!LoadManifest(&level_numbers, &persisted_seq, &manifest_found, error)) return false;
  if (!manifest_found) {
    level_numbers.assign(kMaxLevels, std::vector<uint64_t>());
    level_numbers[0] = sst_numbers;
  }

  std::vector<uint64_t> live_numbers;
  std::shared_ptr<Version> version(new Version());
  for (int level = 0; level < kMaxLevels; ++level) {
    for (size_t i = 0; i < level_numbers[level].size(); ++i) {
      uint64_t number = level_numbers[level][i];
      std::shared_ptr<SSTableReader> reader(new SSTableReader());
      if (!reader->Open(SstPath(number), number)) {
        if (error != nullptr) *error = "cannot open sstable " + FormatFileNumber(number);
        return false;
      }
      version->levels[level].push_back(reader);
      live_numbers.push_back(number);
      if (reader->max_seq() > persisted_seq) persisted_seq = reader->max_seq();
      if (number > max_number) max_number = number;
    }
  }
  version->max_seq = persisted_seq;
  std::sort(version->levels[0].begin(), version->levels[0].end(),
            [](const std::shared_ptr<SSTableReader>& left,
               const std::shared_ptr<SSTableReader>& right) {
              return left->file_number() < right->file_number();
            });
  for (int level = 1; level < kMaxLevels; ++level) {
    std::sort(version->levels[level].begin(), version->levels[level].end(), ReaderLess());
  }
  version_ = version;

  std::sort(live_numbers.begin(), live_numbers.end());
  for (size_t i = 0; i < sst_numbers.size(); ++i) {
    if (!std::binary_search(live_numbers.begin(), live_numbers.end(), sst_numbers[i])) {
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
  if (options_.level_base_size_bytes < 64 * 1024) options_.level_base_size_bytes = 64 * 1024;
  if (options_.level_size_multiplier < 2) options_.level_size_multiplier = 2;
  if (options_.target_sstable_size_bytes < 64 * 1024) {
    options_.target_sstable_size_bytes = 64 * 1024;
  }

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
      uint64_t new_number = AllocateFileNumber();
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

  uint64_t sst_number = AllocateFileNumber();
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

  std::shared_ptr<Version> next(new Version());
  {
    std::lock_guard<std::mutex> lock(mu_);
    next->levels = version_->levels;
  }
  next->levels[0].push_back(reader);
  next->max_seq = version_->max_seq > frozen->MaxSeq() ? version_->max_seq : frozen->MaxSeq();

  if (!WriteManifest(ManifestNumbers(*next), next->max_seq, error)) {
    RemoveFile(sst_path);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
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
  if (!version) return kNotFound;

  for (int level = 0; level < kMaxLevels; ++level) {
    const std::vector<std::shared_ptr<SSTableReader>>& files = version->levels[level];
    if (files.empty()) continue;
    if (level == 0) {
      for (size_t i = files.size(); i > 0; --i) {
        if (files[i - 1]->Get(key, &result)) {
          if (result.type == kTypeValue) {
            if (value != nullptr) *value = result.value;
            return kOk;
          }
          return kNotFound;
        }
      }
    } else {
      size_t low = 0;
      size_t high = files.size();
      while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (CompareStrings(files[middle]->smallest_key(), key) <= 0) {
          low = middle + 1;
        } else {
          high = middle;
        }
      }
      if (low == 0) continue;
      const std::shared_ptr<SSTableReader>& candidate = files[low - 1];
      if (CompareStrings(key, candidate->largest_key()) > 0) continue;
      if (candidate->Get(key, &result)) {
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

int DBImpl::PickLevel(const Version& version) const {
  if (static_cast<int>(version.levels[0].size()) >= Level0Trigger()) return 0;
  for (int level = 1; level < kMaxLevels - 1; ++level) {
    if (LevelSize(version, level) > LevelLimit(level)) return level;
  }
  return -1;
}

bool DBImpl::NeedsCompaction(std::shared_ptr<Version>* snapshot) {
  std::shared_ptr<Version> current;
  {
    std::lock_guard<std::mutex> lock(mu_);
    current = version_;
  }
  if (snapshot != nullptr) *snapshot = current;
  if (!current) return false;
  return PickLevel(*current) >= 0;
}

bool DBImpl::RunMerge(const std::vector<std::shared_ptr<SSTableReader>>& inputs,
                      int output_level, uint64_t max_seq,
                      std::vector<std::shared_ptr<SSTableReader>>* outputs,
                      std::string* error) {
  outputs->clear();
  if (inputs.empty()) return true;
  const bool drop_tombstones = output_level == kMaxLevels - 1;

  struct Cursor {
    std::shared_ptr<SSTableReader> reader;
    std::unique_ptr<SSTableReader::Iterator> iterator;
    bool valid;

    Cursor() : valid(false) {}
  };

  std::vector<Cursor> cursors(inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    cursors[i].reader = inputs[i];
    cursors[i].iterator = inputs[i]->NewIterator();
    cursors[i].valid = cursors[i].iterator->SeekToFirst();
  }

  std::unique_ptr<SSTableBuilder> builder;
  uint64_t builder_number = 0;

  auto abandon_outputs = [&]() {
    if (builder) {
      builder->Abandon();
      builder.reset();
    }
    for (size_t i = 0; i < outputs->size(); ++i) {
      RemoveFile((*outputs)[i]->path());
    }
    outputs->clear();
  };

  auto start_builder = [&]() -> bool {
    builder_number = AllocateFileNumber();
    builder.reset(new SSTableBuilder(SstPath(builder_number), options_.block_size_bytes,
                                     options_.bloom_bits_per_key));
    return builder->ok();
  };

  auto finish_builder = [&]() -> bool {
    if (!builder) return true;
    uint64_t size = 0;
    uint64_t entries = 0;
    std::string path = SstPath(builder_number);
    if (!builder->Finish(max_seq, &size, &entries)) {
      builder->Abandon();
      builder.reset();
      RemoveFile(path);
      return false;
    }
    std::shared_ptr<SSTableReader> reader(new SSTableReader());
    if (!reader->Open(path, builder_number)) {
      RemoveFile(path);
      builder.reset();
      return false;
    }
    outputs->push_back(reader);
    builder.reset();
    return true;
  };

  std::string current_key;
  bool have_current = false;
  std::string last_emitted_key;

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
      } else if (compare == 0 && cursors[i].iterator->seq() >
                                     cursors[static_cast<size_t>(best)].iterator->seq()) {
        best = static_cast<int>(i);
      }
    }
    if (best < 0) break;

    Cursor& cursor = cursors[static_cast<size_t>(best)];
    const std::string& key = cursor.iterator->key();
    bool new_key = !have_current || CompareStrings(key, current_key) != 0;
    if (new_key) {
      current_key = key;
      have_current = true;
      bool emit = cursor.iterator->type() == kTypeValue || !drop_tombstones;
      if (emit) {
        if (builder && builder->CurrentSize() >= options_.target_sstable_size_bytes) {
          if (!finish_builder()) {
            abandon_outputs();
            if (error != nullptr) *error = "cannot split compaction output";
            return false;
          }
        }
        if (!builder) {
          if (!start_builder()) {
            abandon_outputs();
            if (error != nullptr) *error = "cannot create compaction output";
            return false;
          }
        }
        if (!builder->Add(key, cursor.iterator->value(), cursor.iterator->seq(),
                          cursor.iterator->type())) {
          abandon_outputs();
          if (error != nullptr) *error = "compaction add failed";
          return false;
        }
        last_emitted_key = key;
      }
    }
    cursor.iterator->Next();
    if (!cursor.iterator->Valid()) cursor.valid = false;
  }

  if (!finish_builder()) {
    abandon_outputs();
    if (error != nullptr) *error = "cannot finish compaction output";
    return false;
  }
  return true;
}

bool DBImpl::CompactLevel(int level, std::string* error) {
  std::lock_guard<std::mutex> maintenance_lock(maintenance_mu_);
  std::shared_ptr<Version> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot = version_;
  }
  if (!snapshot || level < 0 || level >= kMaxLevels - 1) return true;

  std::vector<std::shared_ptr<SSTableReader>> inputs;
  if (level == 0) {
    inputs = snapshot->levels[0];
  } else if (!snapshot->levels[level].empty()) {
    inputs.push_back(snapshot->levels[level][0]);
  }
  if (inputs.empty()) return true;

  std::string smallest = inputs[0]->smallest_key();
  std::string largest = inputs[0]->largest_key();
  for (size_t i = 1; i < inputs.size(); ++i) {
    if (CompareStrings(inputs[i]->smallest_key(), smallest) < 0) smallest = inputs[i]->smallest_key();
    if (CompareStrings(inputs[i]->largest_key(), largest) > 0) largest = inputs[i]->largest_key();
  }

  int output_level = level + 1;
  std::vector<std::shared_ptr<SSTableReader>> merged = inputs;
  for (size_t i = 0; i < snapshot->levels[output_level].size(); ++i) {
    const std::shared_ptr<SSTableReader>& file = snapshot->levels[output_level][i];
    if (RangesOverlap(file->smallest_key(), file->largest_key(), smallest, largest)) {
      merged.push_back(file);
    }
  }

  std::vector<std::shared_ptr<SSTableReader>> outputs;
  if (!RunMerge(merged, output_level, snapshot->max_seq, &outputs, error)) return false;

  std::shared_ptr<Version> next(new Version());
  next->max_seq = snapshot->max_seq;
  for (int current = 0; current < kMaxLevels; ++current) {
    const std::vector<std::shared_ptr<SSTableReader>>& source = snapshot->levels[current];
    for (size_t i = 0; i < source.size(); ++i) {
      if ((current == level || current == output_level) && ContainsReader(merged, source[i])) {
        continue;
      }
      next->levels[current].push_back(source[i]);
    }
  }
  for (size_t i = 0; i < outputs.size(); ++i) {
    next->levels[output_level].push_back(outputs[i]);
  }
  std::sort(next->levels[output_level].begin(), next->levels[output_level].end(), ReaderLess());

  if (!WriteManifest(ManifestNumbers(*next), next->max_seq, error)) {
    for (size_t i = 0; i < outputs.size(); ++i) RemoveFile(outputs[i]->path());
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    version_ = next;
  }
  for (size_t i = 0; i < merged.size(); ++i) RemoveFile(merged[i]->path());
  compaction_count_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool DBImpl::Compact(std::string* error) {
  std::lock_guard<std::mutex> maintenance_lock(maintenance_mu_);
  std::shared_ptr<Version> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot = version_;
  }
  if (!snapshot) return true;

  std::vector<std::shared_ptr<SSTableReader>> merged;
  for (int level = 0; level < kMaxLevels; ++level) {
    for (size_t i = 0; i < snapshot->levels[level].size(); ++i) {
      merged.push_back(snapshot->levels[level][i]);
    }
  }
  if (merged.empty()) return true;

  std::vector<std::shared_ptr<SSTableReader>> outputs;
  if (!RunMerge(merged, kMaxLevels - 1, snapshot->max_seq, &outputs, error)) return false;

  std::shared_ptr<Version> next(new Version());
  next->max_seq = snapshot->max_seq;
  next->levels[kMaxLevels - 1] = outputs;
  std::sort(next->levels[kMaxLevels - 1].begin(), next->levels[kMaxLevels - 1].end(),
            ReaderLess());

  if (!WriteManifest(ManifestNumbers(*next), next->max_seq, error)) {
    for (size_t i = 0; i < outputs.size(); ++i) RemoveFile(outputs[i]->path());
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    version_ = next;
  }
  for (size_t i = 0; i < merged.size(); ++i) RemoveFile(merged[i]->path());
  compaction_count_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool DBImpl::VerifyLevels(std::string* error) {
  std::shared_ptr<Version> version;
  {
    std::lock_guard<std::mutex> lock(mu_);
    version = version_;
  }
  if (!version) {
    if (error != nullptr) *error = "version is not initialized";
    return false;
  }
  for (int level = 1; level < kMaxLevels; ++level) {
    for (size_t i = 1; i < version->levels[level].size(); ++i) {
      const std::shared_ptr<SSTableReader>& previous = version->levels[level][i - 1];
      const std::shared_ptr<SSTableReader>& current = version->levels[level][i];
      if (CompareStrings(previous->smallest_key(), current->smallest_key()) >= 0) {
        if (error != nullptr) *error = "level files are not sorted";
        return false;
      }
      if (CompareStrings(previous->largest_key(), current->smallest_key()) >= 0) {
        if (error != nullptr) *error = "level files overlap";
        return false;
      }
    }
  }
  return true;
}

std::string DBImpl::LevelsText() {
  std::shared_ptr<Version> version;
  {
    std::lock_guard<std::mutex> lock(mu_);
    version = version_;
  }
  std::string output;
  if (!version) return output;
  for (int level = 0; level < kMaxLevels; ++level) {
    for (size_t i = 0; i < version->levels[level].size(); ++i) {
      const std::shared_ptr<SSTableReader>& file = version->levels[level][i];
      output.append("LEVEL ");
      output.append(UIntToString(static_cast<uint64_t>(level)));
      output.push_back(' ');
      output.append(FormatFileNumber(file->file_number()));
      output.push_back(' ');
      output.append(UIntToString(file->entry_count()));
      output.push_back(' ');
      output.append(file->smallest_key().empty() ? std::string("-")
                                                 : HexEncode(file->smallest_key()));
      output.push_back(' ');
      output.append(file->largest_key().empty() ? std::string("-")
                                                : HexEncode(file->largest_key()));
      output.push_back('\n');
    }
  }
  return output;
}

void DBImpl::BackgroundLoop() {
  for (;;) {
    std::unique_lock<std::mutex> lock(bg_mu_);
    bg_cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
      return shutdown_.load(std::memory_order_relaxed) ||
             compact_requested_.load(std::memory_order_relaxed);
    });
    if (shutdown_.load(std::memory_order_relaxed)) break;
    compact_requested_.store(false, std::memory_order_relaxed);
    lock.unlock();

    for (int step = 0; step < 8; ++step) {
      std::shared_ptr<Version> snapshot;
      if (!NeedsCompaction(&snapshot)) break;
      int level = PickLevel(*snapshot);
      if (level < 0) break;
      std::string error;
      if (!CompactLevel(level, &error)) {
        if (!error.empty()) SetError(error);
        break;
      }
    }
  }
}

void DBImpl::GetStats(minidb_stats* stats) {
  if (stats == nullptr) return;
  std::memset(stats, 0, sizeof(*stats));
  std::lock_guard<std::mutex> lock(mu_);
  stats->memtable_entries = mem_ ? mem_->Count() : 0;
  stats->immutable_entries = imm_ ? imm_->Count() : 0;
  uint64_t entries = 0;
  uint64_t files = 0;
  if (version_) {
    for (int level = 0; level < kMaxLevels; ++level) {
      stats->level_files[level] = version_->levels[level].size();
      uint64_t bytes = 0;
      for (size_t i = 0; i < version_->levels[level].size(); ++i) {
        entries += version_->levels[level][i]->entry_count();
        bytes += version_->levels[level][i]->file_size();
      }
      stats->level_bytes[level] = bytes;
      files += version_->levels[level].size();
    }
  }
  stats->sstable_count = files;
  stats->sstable_entries = entries;
  stats->wal_bytes = (wal_ && wal_->IsOpen()) ? wal_->size() : 0;
  stats->last_sequence = last_seq_.load(std::memory_order_relaxed);
  stats->flush_count = flush_count_.load(std::memory_order_relaxed);
  stats->compaction_count = compaction_count_.load(std::memory_order_relaxed);
  stats->get_count = get_count_.load(std::memory_order_relaxed);
  stats->put_count = put_count_.load(std::memory_order_relaxed);
  stats->delete_count = delete_count_.load(std::memory_order_relaxed);
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
  options->level_base_size_bytes = 1024u * 1024u;
  options->level_size_multiplier = 4;
  options->target_sstable_size_bytes = 1024u * 1024u;
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
  internal.level_base_size_bytes = effective->level_base_size_bytes > 0
                                       ? effective->level_base_size_bytes
                                       : defaults.level_base_size_bytes;
  internal.level_size_multiplier = effective->level_size_multiplier > 0
                                       ? effective->level_size_multiplier
                                       : defaults.level_size_multiplier;
  internal.target_sstable_size_bytes = effective->target_sstable_size_bytes > 0
                                           ? effective->target_sstable_size_bytes
                                           : defaults.target_sstable_size_bytes;

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

int db_verify_levels(minidb* db) {
  if (db == nullptr || db->impl == nullptr) return DB_ERR;
  std::string error;
  return db->impl->VerifyLevels(&error) ? DB_OK : DB_ERR;
}

int db_levels_text(minidb* db, char** text, size_t* text_len) {
  if (db == nullptr || db->impl == nullptr || text == nullptr || text_len == nullptr) return DB_ERR;
  *text = nullptr;
  *text_len = 0;
  std::string summary = db->impl->LevelsText();
  char* buffer = static_cast<char*>(std::malloc(summary.size() == 0 ? 1 : summary.size()));
  if (buffer == nullptr) return DB_ERR;
  if (!summary.empty()) std::memcpy(buffer, summary.data(), summary.size());
  *text = buffer;
  *text_len = summary.size();
  return DB_OK;
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
