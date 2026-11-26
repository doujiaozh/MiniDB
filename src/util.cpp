#include "util.h"

namespace mdb {

namespace {

struct Crc32Table {
  uint32_t entries[256];

  Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      entries[i] = value;
    }
  }
};

const Crc32Table& CrcTable() {
  static const Crc32Table table;
  return table;
}

}

uint32_t Crc32Extend(uint32_t crc, const char* data, size_t size) {
  const Crc32Table& table = CrcTable();
  uint32_t value = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    value = table.entries[(value ^ static_cast<unsigned char>(data[i])) & 0xFFu] ^ (value >> 8);
  }
  return value ^ 0xFFFFFFFFu;
}

uint32_t Crc32(const char* data, size_t size, uint32_t init) {
  return Crc32Extend(init, data, size);
}

uint64_t Hash64(const char* data, size_t size, uint64_t seed) {
  uint64_t value = seed ^ 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    value ^= static_cast<unsigned char>(data[i]);
    value *= 1099511628211ull;
  }
  value ^= static_cast<uint64_t>(size);
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33;
  return value;
}

}
