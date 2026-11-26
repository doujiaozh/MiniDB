#ifndef MINIDB_DB_IMPL_H
#define MINIDB_DB_IMPL_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "minidb.h"
#include "skiplist.h"
#include "sstable.h"
#include "wal.h"

namespace mdb {

struct Options {
  size_t memtable_size_bytes;
  size_t block_size_bytes;
  int compaction_trigger_files;
  int bloom_bits_per_key;
  bool sync_on_write;

  Options();
};

struct Version {
  std::vector<std::shared_ptr<SSTableReader>> files;
  uint64_t max_seq;

  Version() : max_seq(0) {}
};

class DBImpl {
 public:
  DBImpl();
  ~DBImpl();

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  bool Open(const std::string& directory, const Options& options, std::string* error);
  int Put(const std::string& key, const std::string& value);
  int Get(const std::string& key, std::string* value);
  int Delete(const std::string& key);
  bool Flush(std::string* error);
  bool Compact(std::string* error);
  void GetStats(minidb_stats* stats);
  const char* LastError();
  void Close();

 private:
  bool Recover(std::string* error);
  bool WriteManifest(const std::vector<uint64_t>& files, uint64_t max_seq, std::string* error);
  bool LoadManifest(std::vector<uint64_t>* files, uint64_t* max_seq, bool* found,
                    std::string* error);
  int WriteBatch(ValueType type, const std::string& key, const std::string& value);
  bool MaybeCompact(std::string* error);
  void BackgroundLoop();
  void SetError(const std::string& message);

  std::string SstPath(uint64_t number) const;
  std::string WalPath(uint64_t number) const;
  std::string ManifestPath() const;
  std::string ManifestTempPath() const;

  std::string directory_;
  Options options_;

  std::mutex mu_;
  std::mutex writer_mu_;
  std::mutex maintenance_mu_;
  std::shared_ptr<SkipList> mem_;
  std::shared_ptr<SkipList> imm_;
  std::shared_ptr<Version> version_;
  std::unique_ptr<WalWriter> wal_;
  uint64_t wal_number_;
  uint64_t next_file_number_;
  std::atomic<uint64_t> last_seq_;

  std::atomic<bool> shutdown_;
  std::atomic<bool> compact_requested_;
  std::mutex bg_mu_;
  std::condition_variable bg_cv_;
  std::thread bg_thread_;

  std::mutex error_mu_;
  std::string last_error_;

  std::atomic<uint64_t> flush_count_;
  std::atomic<uint64_t> compaction_count_;
  std::atomic<uint64_t> get_count_;
  std::atomic<uint64_t> put_count_;
  std::atomic<uint64_t> delete_count_;

  bool opened_;
};

}

#endif
