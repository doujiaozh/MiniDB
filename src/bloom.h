#ifndef MINIDB_BLOOM_H
#define MINIDB_BLOOM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdb {

class BloomFilter {
 public:
  BloomFilter();

  void Build(const std::vector<std::string>& keys, int bits_per_key);
  bool MayContain(const std::string& key) const;
  void Serialize(std::string* dst) const;
  bool Parse(const char* data, size_t size);
  void Clear();

  bool Empty() const { return bits_.empty(); }

 private:
  void AddKey(const std::string& key);

  std::string bits_;
  uint32_t k_;
};

}

#endif
