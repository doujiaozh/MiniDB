#include "bloom.h"

#include "util.h"

namespace mdb {

BloomFilter::BloomFilter() : k_(0) {}

void BloomFilter::Clear() {
  bits_.clear();
  k_ = 0;
}

void BloomFilter::Build(const std::vector<std::string>& keys, int bits_per_key) {
  Clear();
  if (keys.empty()) return;
  if (bits_per_key < 1) bits_per_key = 1;
  if (bits_per_key > 30) bits_per_key = 30;
  size_t bit_count = keys.size() * static_cast<size_t>(bits_per_key);
  if (bit_count < 64) bit_count = 64;
  size_t byte_count = (bit_count + 7) / 8;
  bits_.assign(byte_count, 0);
  k_ = static_cast<uint32_t>(static_cast<double>(bits_per_key) * 0.69);
  if (k_ < 1) k_ = 1;
  if (k_ > 30) k_ = 30;
  for (size_t i = 0; i < keys.size(); ++i) {
    AddKey(keys[i]);
  }
}

void BloomFilter::AddKey(const std::string& key) {
  size_t bit_count = bits_.size() * 8;
  if (bit_count == 0) return;
  uint64_t hash = Hash64(key.data(), key.size(), 0xbc9f1d34ull);
  uint64_t delta = (hash >> 33) | (hash << 31);
  if (delta == 0) delta = 0x9e3779b97f4a7c15ull;
  for (uint32_t i = 0; i < k_; ++i) {
    size_t bit = static_cast<size_t>(hash % bit_count);
    bits_[bit >> 3] = static_cast<char>(static_cast<unsigned char>(bits_[bit >> 3]) |
                                        static_cast<unsigned char>(1u << (bit & 7)));
    hash += delta;
  }
}

bool BloomFilter::MayContain(const std::string& key) const {
  size_t bit_count = bits_.size() * 8;
  if (bit_count == 0) return true;
  if (k_ == 0) return true;
  uint64_t hash = Hash64(key.data(), key.size(), 0xbc9f1d34ull);
  uint64_t delta = (hash >> 33) | (hash << 31);
  if (delta == 0) delta = 0x9e3779b97f4a7c15ull;
  for (uint32_t i = 0; i < k_; ++i) {
    size_t bit = static_cast<size_t>(hash % bit_count);
    if ((static_cast<unsigned char>(bits_[bit >> 3]) &
         static_cast<unsigned char>(1u << (bit & 7))) == 0) {
      return false;
    }
    hash += delta;
  }
  return true;
}

void BloomFilter::Serialize(std::string* dst) const {
  PutFixed32(dst, k_);
  PutFixed32(dst, static_cast<uint32_t>(bits_.size()));
  dst->append(bits_);
}

bool BloomFilter::Parse(const char* data, size_t size) {
  Clear();
  if (size < 8) return false;
  uint32_t k = DecodeFixed32(data);
  uint32_t byte_count = DecodeFixed32(data + 4);
  if (static_cast<size_t>(byte_count) + 8 != size) return false;
  if (k > 30) return false;
  k_ = k;
  bits_.assign(data + 8, byte_count);
  return true;
}

}
