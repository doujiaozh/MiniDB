#ifndef MINIDB_WAL_H
#define MINIDB_WAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "platform.h"
#include "util.h"

namespace mdb {

struct WalRecord {
  uint64_t seq;
  ValueType type;
  std::string key;
  std::string value;
};

class WalWriter {
 public:
  WalWriter();
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  bool Open(const std::string& path, bool append);
  bool Append(uint64_t seq, ValueType type, const std::string& key, const std::string& value);
  bool Sync();
  bool Close();
  bool IsOpen() const { return file_.IsOpen(); }
  const std::string& path() const { return path_; }
  uint64_t size() const { return file_.size(); }

 private:
  FileWriter file_;
  std::string path_;
};

bool LoadWalFile(const std::string& path, std::vector<WalRecord>* records, uint64_t* valid_bytes);

}

#endif
