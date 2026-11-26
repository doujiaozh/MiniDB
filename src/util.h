#ifndef MINIDB_UTIL_H
#define MINIDB_UTIL_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace mdb {

const int kOk = 0;
const int kNotFound = 1;
const int kError = -1;

uint32_t Crc32(const char* data, size_t size, uint32_t init = 0);
uint32_t Crc32Extend(uint32_t crc, const char* data, size_t size);
uint64_t Hash64(const char* data, size_t size, uint64_t seed);

inline int CompareBytes(const char* a, size_t a_size, const char* b, size_t b_size) {
  size_t common = a_size < b_size ? a_size : b_size;
  int result = common == 0 ? 0 : std::memcmp(a, b, common);
  if (result != 0) return result;
  if (a_size < b_size) return -1;
  if (a_size > b_size) return 1;
  return 0;
}

inline int CompareStrings(const std::string& a, const std::string& b) {
  return CompareBytes(a.data(), a.size(), b.data(), b.size());
}

inline void EncodeFixed32(char* dst, uint32_t value) {
  dst[0] = static_cast<char>(value & 0xffu);
  dst[1] = static_cast<char>((value >> 8) & 0xffu);
  dst[2] = static_cast<char>((value >> 16) & 0xffu);
  dst[3] = static_cast<char>((value >> 24) & 0xffu);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((value >> (8 * i)) & 0xffu);
  }
}

inline uint32_t DecodeFixed32(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t low = DecodeFixed32(p);
  uint64_t high = DecodeFixed32(p + 4);
  return low | (high << 32);
}

inline void PutFixed32(std::string* dst, uint32_t value) {
  char buffer[4];
  EncodeFixed32(buffer, value);
  dst->append(buffer, 4);
}

inline void PutFixed64(std::string* dst, uint64_t value) {
  char buffer[8];
  EncodeFixed64(buffer, value);
  dst->append(buffer, 8);
}

enum ValueType : uint8_t {
  kTypeDeletion = 0,
  kTypeValue = 1
};

struct LookupResult {
  ValueType type;
  uint64_t seq;
  std::string value;

  LookupResult() : type(kTypeDeletion), seq(0) {}
};

}

#endif
