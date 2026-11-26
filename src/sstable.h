#ifndef MINIDB_SSTABLE_H
#define MINIDB_SSTABLE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bloom.h"
#include "platform.h"
#include "util.h"

namespace mdb {

class SSTableBuilder {
 public:
  SSTableBuilder(const std::string& path, size_t block_size, int bloom_bits_per_key);
  ~SSTableBuilder();

  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;

  bool Add(const std::string& key, const std::string& value, uint64_t seq, ValueType type);
  bool Finish(uint64_t max_seq_hint, uint64_t* file_size, uint64_t* entry_count);
  void Abandon();

  bool ok() const { return !failed_; }

 private:
  bool FlushBlock();

  struct IndexEntry {
    uint64_t offset;
    uint32_t size;
    std::string key;
  };

  std::string path_;
  std::string tmp_path_;
  std::unique_ptr<FileWriter> file_;
  size_t block_size_;
  int bloom_bits_per_key_;
  std::string block_buffer_;
  std::vector<uint32_t> block_offsets_;
  std::string last_key_;
  uint64_t last_seq_;
  bool have_last_;
  std::vector<IndexEntry> index_;
  std::vector<std::string> keys_;
  uint64_t entry_count_;
  uint64_t max_seq_;
  bool finished_;
  bool failed_;
};

class SSTableReader {
 public:
  SSTableReader();
  ~SSTableReader();

  SSTableReader(const SSTableReader&) = delete;
  SSTableReader& operator=(const SSTableReader&) = delete;

  bool Open(const std::string& path, uint64_t file_number);
  bool Get(const std::string& key, LookupResult* result) const;

  uint64_t file_number() const { return file_number_; }
  uint64_t entry_count() const { return entry_count_; }
  uint64_t max_seq() const { return max_seq_; }
  const std::string& path() const { return path_; }

  class Iterator {
   public:
    explicit Iterator(const SSTableReader* table);
    bool Valid() const { return valid_; }
    void Next();
    bool SeekToFirst();
    const std::string& key() const { return key_; }
    const std::string& value() const { return value_; }
    uint64_t seq() const { return seq_; }
    ValueType type() const { return type_; }

   private:
    bool LoadBlock(size_t index);
    bool DecodeCurrent();

    const SSTableReader* table_;
    size_t block_index_;
    uint32_t entry_index_;
    uint32_t entry_count_;
    std::string block_;
    std::string key_;
    std::string value_;
    uint64_t seq_;
    ValueType type_;
    bool valid_;
  };

  std::unique_ptr<Iterator> NewIterator() const;

 private:
  struct IndexEntry {
    uint64_t offset;
    uint32_t size;
    std::string last_key;
  };

  bool ReadBlock(uint64_t offset, uint32_t size, std::string* out) const;

  FileReader file_;
  std::string path_;
  uint64_t file_number_;
  std::vector<IndexEntry> index_;
  BloomFilter bloom_;
  uint64_t entry_count_;
  uint64_t max_seq_;
};

}

#endif
