#include "sstable.h"

#include <cstring>
#include <utility>

namespace mdb {

namespace {

const uint64_t kSSTableMagic = 0x4d696e6944423031ull;
const size_t kFooterSize = 56;
const size_t kEntryHeaderSize = 17;
const uint32_t kMaxBlockSize = 64u * 1024u * 1024u;

struct BlockEntry {
  const char* key;
  uint32_t key_length;
  const char* value;
  uint32_t value_length;
  uint64_t seq;
  ValueType type;
};

const char* BlockEntryKey(const std::string& block, uint32_t count, uint32_t index,
                          uint32_t* key_length) {
  if (block.size() < 4) return nullptr;
  if (index >= count) return nullptr;
  size_t table_size = 4 + static_cast<size_t>(count) * 4;
  if (table_size > block.size()) return nullptr;
  uint32_t offset = DecodeFixed32(block.data() + 4 + static_cast<size_t>(index) * 4);
  size_t entry_offset = table_size + offset;
  if (entry_offset + kEntryHeaderSize > block.size()) return nullptr;
  uint32_t key_len = DecodeFixed32(block.data() + entry_offset);
  uint32_t value_len = DecodeFixed32(block.data() + entry_offset + 4);
  if (entry_offset + kEntryHeaderSize + key_len + value_len > block.size()) return nullptr;
  *key_length = key_len;
  return block.data() + entry_offset + kEntryHeaderSize;
}

bool ParseBlockEntry(const std::string& block, uint32_t count, uint32_t index, BlockEntry* entry) {
  uint32_t key_length = 0;
  const char* key = BlockEntryKey(block, count, index, &key_length);
  if (key == nullptr) return false;
  size_t entry_offset = static_cast<size_t>(key - block.data()) - kEntryHeaderSize;
  uint32_t value_length = DecodeFixed32(block.data() + entry_offset + 4);
  size_t value_offset = entry_offset + kEntryHeaderSize + key_length;
  if (value_offset + value_length > block.size()) return false;
  entry->key = key;
  entry->key_length = key_length;
  entry->value = block.data() + value_offset;
  entry->value_length = value_length;
  entry->seq = DecodeFixed64(block.data() + entry_offset + 8);
  entry->type = static_cast<ValueType>(static_cast<unsigned char>(block[entry_offset + 16]));
  return true;
}

}

SSTableBuilder::SSTableBuilder(const std::string& path, size_t block_size, int bloom_bits_per_key)
    : path_(path),
      tmp_path_(path + ".tmp"),
      file_(new FileWriter()),
      block_size_(block_size < 512 ? 512 : block_size),
      bloom_bits_per_key_(bloom_bits_per_key < 1 ? 1 : bloom_bits_per_key),
      last_seq_(0),
      have_last_(false),
      entry_count_(0),
      max_seq_(0),
      finished_(false),
      failed_(false) {
  if (!file_->Open(tmp_path_, false)) failed_ = true;
}

SSTableBuilder::~SSTableBuilder() {
  if (!finished_) Abandon();
}

bool SSTableBuilder::Add(const std::string& key, const std::string& value, uint64_t seq,
                         ValueType type) {
  if (failed_ || finished_) return false;
  if (have_last_) {
    int compare = CompareStrings(key, last_key_);
    if (compare < 0) {
      failed_ = true;
      return false;
    }
    if (compare == 0 && seq >= last_seq_) {
      failed_ = true;
      return false;
    }
  }

  uint32_t offset = static_cast<uint32_t>(block_buffer_.size());
  PutFixed32(&block_buffer_, static_cast<uint32_t>(key.size()));
  PutFixed32(&block_buffer_, static_cast<uint32_t>(value.size()));
  PutFixed64(&block_buffer_, seq);
  block_buffer_.push_back(static_cast<char>(type));
  block_buffer_.append(key);
  block_buffer_.append(value);
  block_offsets_.push_back(offset);

  keys_.push_back(key);
  last_key_ = key;
  last_seq_ = seq;
  have_last_ = true;
  ++entry_count_;
  if (seq > max_seq_) max_seq_ = seq;

  if (block_buffer_.size() >= block_size_) return FlushBlock();
  return true;
}

bool SSTableBuilder::FlushBlock() {
  if (block_offsets_.empty()) return true;
  uint32_t count = static_cast<uint32_t>(block_offsets_.size());
  std::string block;
  block.reserve(4 + static_cast<size_t>(count) * 4 + block_buffer_.size());
  PutFixed32(&block, count);
  for (size_t i = 0; i < block_offsets_.size(); ++i) {
    PutFixed32(&block, block_offsets_[i]);
  }
  block.append(block_buffer_);

  uint64_t offset = file_->size();
  if (!file_->Append(block.data(), block.size())) {
    failed_ = true;
    return false;
  }
  if (block.size() > kMaxBlockSize) {
    failed_ = true;
    return false;
  }
  IndexEntry entry;
  entry.offset = offset;
  entry.size = static_cast<uint32_t>(block.size());
  entry.key = last_key_;
  index_.push_back(entry);
  block_buffer_.clear();
  block_offsets_.clear();
  return true;
}

bool SSTableBuilder::Finish(uint64_t max_seq_hint, uint64_t* file_size, uint64_t* entry_count) {
  if (failed_ || finished_) return false;
  finished_ = true;
  if (!FlushBlock()) {
    Abandon();
    return false;
  }

  std::string index;
  PutFixed32(&index, static_cast<uint32_t>(index_.size()));
  for (size_t i = 0; i < index_.size(); ++i) {
    PutFixed64(&index, index_[i].offset);
    PutFixed32(&index, index_[i].size);
    PutFixed32(&index, static_cast<uint32_t>(index_[i].key.size()));
    index.append(index_[i].key);
  }
  uint64_t index_offset = file_->size();
  if (!file_->Append(index.data(), index.size())) {
    Abandon();
    return false;
  }

  BloomFilter bloom;
  bloom.Build(keys_, bloom_bits_per_key_);
  std::string bloom_data;
  bloom.Serialize(&bloom_data);
  uint64_t bloom_offset = file_->size();
  if (!file_->Append(bloom_data.data(), bloom_data.size())) {
    Abandon();
    return false;
  }

  uint64_t final_max_seq = max_seq_ > max_seq_hint ? max_seq_ : max_seq_hint;
  std::string footer;
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, index.size());
  PutFixed64(&footer, bloom_offset);
  PutFixed64(&footer, bloom_data.size());
  PutFixed64(&footer, entry_count_);
  PutFixed64(&footer, final_max_seq);
  PutFixed64(&footer, kSSTableMagic);
  if (!file_->Append(footer.data(), footer.size())) {
    Abandon();
    return false;
  }
  if (!file_->Sync()) {
    Abandon();
    return false;
  }
  uint64_t total_size = file_->size();
  file_->Close();
  if (!RenameFile(tmp_path_, path_)) {
    failed_ = true;
    return false;
  }
  if (file_size != nullptr) *file_size = total_size;
  if (entry_count != nullptr) *entry_count = entry_count_;
  return true;
}

void SSTableBuilder::Abandon() {
  finished_ = true;
  file_->Close();
  RemoveFile(tmp_path_);
}

uint64_t SSTableBuilder::CurrentSize() const {
  return file_ ? file_->size() : 0;
}

SSTableReader::SSTableReader()
    : file_number_(0), entry_count_(0), max_seq_(0) {}

SSTableReader::~SSTableReader() {}

bool SSTableReader::Open(const std::string& path, uint64_t file_number) {
  path_ = path;
  file_number_ = file_number;
  if (!file_.Open(path)) return false;
  uint64_t size = file_.size();
  if (size < kFooterSize) return false;

  std::string footer;
  footer.resize(kFooterSize);
  if (!file_.ReadAtExact(size - kFooterSize, &footer[0], kFooterSize)) return false;
  if (DecodeFixed64(footer.data() + 48) != kSSTableMagic) return false;

  uint64_t index_offset = DecodeFixed64(footer.data());
  uint64_t index_size = DecodeFixed64(footer.data() + 8);
  uint64_t bloom_offset = DecodeFixed64(footer.data() + 16);
  uint64_t bloom_size = DecodeFixed64(footer.data() + 24);
  entry_count_ = DecodeFixed64(footer.data() + 32);
  max_seq_ = DecodeFixed64(footer.data() + 40);

  if (index_offset + index_size > size - kFooterSize) return false;
  if (bloom_offset + bloom_size > size - kFooterSize) return false;
  if (index_size > 256ull * 1024ull * 1024ull) return false;

  std::string index_data;
  index_data.resize(static_cast<size_t>(index_size));
  if (index_size > 0 && !file_.ReadAtExact(index_offset, &index_data[0], index_data.size())) {
    return false;
  }
  if (index_data.size() < 4) return false;
  uint32_t count = DecodeFixed32(index_data.data());
  size_t position = 4;
  index_.clear();
  index_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (position + 16 > index_data.size()) return false;
    IndexEntry entry;
    entry.offset = DecodeFixed64(index_data.data() + position);
    entry.size = DecodeFixed32(index_data.data() + position + 8);
    uint32_t key_length = DecodeFixed32(index_data.data() + position + 12);
    position += 16;
    if (position + key_length > index_data.size()) return false;
    entry.last_key.assign(index_data.data() + position, key_length);
    position += key_length;
    if (entry.size == 0 || entry.size > kMaxBlockSize) return false;
    if (entry.offset + entry.size > size - kFooterSize) return false;
    index_.push_back(std::move(entry));
  }

  if (!index_.empty()) {
    std::string first_block;
    if (!ReadBlock(index_[0].offset, index_[0].size, &first_block)) return false;
    if (first_block.size() < 4) return false;
    uint32_t first_count = DecodeFixed32(first_block.data());
    if (first_count > 0) {
      BlockEntry first_entry;
      if (!ParseBlockEntry(first_block, first_count, 0, &first_entry)) return false;
      smallest_key_.assign(first_entry.key, first_entry.key_length);
    }
    largest_key_ = index_.back().last_key;
  }

  std::string bloom_data;
  bloom_data.resize(static_cast<size_t>(bloom_size));
  if (bloom_size > 0 && !file_.ReadAtExact(bloom_offset, &bloom_data[0], bloom_data.size())) {
    return false;
  }
  if (!bloom_.Parse(bloom_data.data(), bloom_data.size())) return false;
  return true;
}

bool SSTableReader::ReadBlock(uint64_t offset, uint32_t size, std::string* out) const {
  if (out == nullptr || size == 0) return false;
  if (offset + size > file_.size()) return false;
  out->resize(size);
  return file_.ReadAtExact(offset, &(*out)[0], size);
}

bool SSTableReader::Get(const std::string& key, LookupResult* result) const {
  if (!bloom_.MayContain(key)) return false;

  size_t low = 0;
  size_t high = index_.size();
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (CompareStrings(index_[middle].last_key, key) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low >= index_.size()) return false;

  const IndexEntry& block_entry = index_[low];
  std::string block;
  if (!ReadBlock(block_entry.offset, block_entry.size, &block)) return false;
  if (block.size() < 4) return false;
  uint32_t count = DecodeFixed32(block.data());
  if (4 + static_cast<uint64_t>(count) * 4 > block.size()) return false;

  uint32_t left = 0;
  uint32_t right = count;
  while (left < right) {
    uint32_t middle = left + (right - left) / 2;
    uint32_t key_length = 0;
    const char* candidate = BlockEntryKey(block, count, middle, &key_length);
    if (candidate == nullptr) return false;
    if (CompareBytes(candidate, key_length, key.data(), key.size()) < 0) {
      left = middle + 1;
    } else {
      right = middle;
    }
  }
  if (left >= count) return false;

  BlockEntry entry;
  if (!ParseBlockEntry(block, count, left, &entry)) return false;
  if (CompareBytes(entry.key, entry.key_length, key.data(), key.size()) != 0) return false;
  if (result != nullptr) {
    result->type = entry.type;
    result->seq = entry.seq;
    result->value.assign(entry.value, entry.value_length);
  }
  return true;
}

SSTableReader::Iterator::Iterator(const SSTableReader* table)
    : table_(table),
      block_index_(0),
      entry_index_(0),
      entry_count_(0),
      seq_(0),
      type_(kTypeValue),
      valid_(false) {}

bool SSTableReader::Iterator::LoadBlock(size_t index) {
  if (index >= table_->index_.size()) return false;
  const IndexEntry& entry = table_->index_[index];
  if (!table_->ReadBlock(entry.offset, entry.size, &block_)) return false;
  if (block_.size() < 4) return false;
  entry_count_ = DecodeFixed32(block_.data());
  if (4 + static_cast<uint64_t>(entry_count_) * 4 > block_.size()) return false;
  return true;
}

bool SSTableReader::Iterator::DecodeCurrent() {
  if (entry_index_ >= entry_count_) return false;
  BlockEntry entry;
  if (!ParseBlockEntry(block_, entry_count_, entry_index_, &entry)) return false;
  key_.assign(entry.key, entry.key_length);
  value_.assign(entry.value, entry.value_length);
  seq_ = entry.seq;
  type_ = entry.type;
  return true;
}

bool SSTableReader::Iterator::SeekToFirst() {
  valid_ = false;
  block_index_ = 0;
  entry_index_ = 0;
  if (table_->index_.empty()) return false;
  if (!LoadBlock(0)) return false;
  if (entry_count_ == 0) return false;
  valid_ = DecodeCurrent();
  return valid_;
}

void SSTableReader::Iterator::Next() {
  if (!valid_) return;
  ++entry_index_;
  while (entry_index_ >= entry_count_) {
    ++block_index_;
    if (block_index_ >= table_->index_.size()) {
      valid_ = false;
      return;
    }
    if (!LoadBlock(block_index_)) {
      valid_ = false;
      return;
    }
    entry_index_ = 0;
  }
  if (!DecodeCurrent()) valid_ = false;
}

std::unique_ptr<SSTableReader::Iterator> SSTableReader::NewIterator() const {
  return std::unique_ptr<Iterator>(new Iterator(this));
}

}
