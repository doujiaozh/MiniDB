#include "wal.h"

namespace mdb {

namespace {
const size_t kWalHeaderSize = 8;
const size_t kWalPayloadHeaderSize = 17;
const uint32_t kWalMaxRecordSize = 64u * 1024u * 1024u;
}

WalWriter::WalWriter() {}

WalWriter::~WalWriter() {
  Close();
}

bool WalWriter::Open(const std::string& path, bool append) {
  path_ = path;
  return file_.Open(path, append);
}

bool WalWriter::Append(uint64_t seq, ValueType type, const std::string& key, const std::string& value) {
  if (!file_.IsOpen()) return false;
  std::string payload;
  payload.reserve(kWalPayloadHeaderSize + key.size() + value.size());
  PutFixed64(&payload, seq);
  payload.push_back(static_cast<char>(type));
  PutFixed32(&payload, static_cast<uint32_t>(key.size()));
  PutFixed32(&payload, static_cast<uint32_t>(value.size()));
  payload.append(key);
  payload.append(value);

  std::string record;
  record.reserve(kWalHeaderSize + payload.size());
  PutFixed32(&record, static_cast<uint32_t>(payload.size()));
  PutFixed32(&record, Crc32(payload.data(), payload.size(), 0));
  record.append(payload);
  return file_.Append(record.data(), record.size());
}

bool WalWriter::Sync() {
  return file_.Sync();
}

bool WalWriter::Close() {
  return file_.Close();
}

bool LoadWalFile(const std::string& path, std::vector<WalRecord>* records, uint64_t* valid_bytes) {
  if (records == nullptr || valid_bytes == nullptr) return false;
  *valid_bytes = 0;
  FileReader reader;
  if (!reader.Open(path)) return false;
  uint64_t file_size = reader.size();
  if (file_size == 0) return true;
  if (file_size > 0x7fffffffull) return false;

  std::string data;
  data.resize(static_cast<size_t>(file_size));
  if (!reader.ReadAtExact(0, &data[0], data.size())) return false;

  size_t position = 0;
  while (position + kWalHeaderSize <= data.size()) {
    uint32_t length = DecodeFixed32(data.data() + position);
    uint32_t checksum = DecodeFixed32(data.data() + position + 4);
    if (length < kWalPayloadHeaderSize || length > kWalMaxRecordSize) break;
    size_t payload_offset = position + kWalHeaderSize;
    if (payload_offset + length > data.size()) break;
    const char* payload = data.data() + payload_offset;
    if (Crc32(payload, length, 0) != checksum) break;

    uint32_t key_length = DecodeFixed32(payload + 9);
    uint32_t value_length = DecodeFixed32(payload + 13);
    if (static_cast<uint64_t>(kWalPayloadHeaderSize) + key_length + value_length != length) break;
    unsigned char raw_type = static_cast<unsigned char>(payload[8]);
    if (raw_type != static_cast<unsigned char>(kTypeValue) &&
        raw_type != static_cast<unsigned char>(kTypeDeletion)) {
      break;
    }

    WalRecord record;
    record.seq = DecodeFixed64(payload);
    record.type = static_cast<ValueType>(raw_type);
    record.key.assign(payload + kWalPayloadHeaderSize, key_length);
    record.value.assign(payload + kWalPayloadHeaderSize + key_length, value_length);
    records->push_back(record);

    position = payload_offset + length;
    *valid_bytes = position;
  }
  return true;
}

}
