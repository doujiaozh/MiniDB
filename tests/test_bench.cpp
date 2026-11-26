#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "minidb.h"

namespace {

std::string ToText(unsigned long long value) {
  char buffer[32];
  int length = 0;
  do {
    buffer[length++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);
  std::string result;
  while (length > 0) result.push_back(buffer[--length]);
  return result;
}

std::string Key(int index) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "key-%08d", index);
  return std::string(buffer);
}

std::string Value(int index, int version) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "value-%08d-rev-%03d", index, version % 1000);
  return std::string(buffer);
}

void RemoveTree(const std::string& path) {
  std::string pattern = path + "\\*";
  WIN32_FIND_DATAA data;
  HANDLE find = ::FindFirstFileA(pattern.c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (std::strcmp(data.cFileName, ".") == 0) continue;
      if (std::strcmp(data.cFileName, "..") == 0) continue;
      std::string child = path + "\\" + data.cFileName;
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        RemoveTree(child);
      } else {
        ::DeleteFileA(child.c_str());
      }
    } while (::FindNextFileA(find, &data));
    ::FindClose(find);
  }
  ::RemoveDirectoryA(path.c_str());
}

std::string ExeDirectory() {
  char buffer[MAX_PATH * 2];
  DWORD length = ::GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
  std::string path(buffer, length == 0 ? 0 : length);
  size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos) return std::string(".");
  return path.substr(0, slash);
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}

int main(int argc, char** argv) {
  const std::string root = ExeDirectory();
  const std::string directory = root + "\\..\\testdata\\bench";
  RemoveTree(directory);

  int memtable_kb = 64;
  if (argc > 1) {
    int requested = std::atoi(argv[1]);
    if (requested >= 16) memtable_kb = requested;
  }

  minidb_options options;
  db_options_init(&options);
  options.memtable_size_bytes = static_cast<size_t>(memtable_kb) * 1024;
  options.compaction_trigger_files = 3;
  options.sync_on_write = 0;

  minidb* db = db_open(directory.c_str(), &options);
  if (db == nullptr) {
    std::printf("[FAIL] cannot open database: %s\n", db_last_error(nullptr));
    return 1;
  }

  const int kOperations = 10000;
  const int kKeys = 2000;
  std::map<std::string, std::string> model;
  std::mt19937 rng(20261127u);

  int put_count = 0;
  int get_count = 0;
  int delete_count = 0;
  int failures = 0;

  std::printf("[INFO] open database: %s\n", directory.c_str());
  std::printf("[INFO] memtable=%dKB seed=20261127 keys=%d operations=%d\n", memtable_kb,
              kKeys, kOperations);
  std::printf("[STAGE] 10000 random mixed read write operations\n");

  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  for (int i = 0; i < kOperations; ++i) {
    int key_index = static_cast<int>(rng() % static_cast<unsigned int>(kKeys));
    std::string key = Key(key_index);
    unsigned int action = rng() % 100u;

    if (action < 60) {
      std::string value = Value(key_index, i);
      int status = db_put(db, key.data(), key.size(), value.data(), value.size());
      if (status != DB_OK) {
        ++failures;
        continue;
      }
      model[key] = value;
      ++put_count;
    } else if (action < 85) {
      char* value = nullptr;
      size_t length = 0;
      int status = db_get(db, key.data(), key.size(), &value, &length);
      std::map<std::string, std::string>::const_iterator entry = model.find(key);
      if (entry == model.end()) {
        if (status != DB_NOT_FOUND) ++failures;
      } else {
        if (status != DB_OK || length != entry->second.size() ||
            std::memcmp(value, entry->second.data(), length) != 0) {
          ++failures;
        }
      }
      if (value != nullptr) db_free_value(value);
      ++get_count;
    } else {
      int status = db_delete(db, key.data(), key.size());
      if (status != DB_OK) {
        ++failures;
        continue;
      }
      model.erase(key);
      ++delete_count;
    }
  }
  double mixed_seconds = SecondsSince(start);

  int model_mismatches = 0;
  int absent_mismatches = 0;
  for (int i = 0; i < kKeys; ++i) {
    std::string key = Key(i);
    std::map<std::string, std::string>::const_iterator entry = model.find(key);
    char* value = nullptr;
    size_t length = 0;
    int status = db_get(db, key.data(), key.size(), &value, &length);
    if (entry == model.end()) {
      if (status != DB_NOT_FOUND) ++absent_mismatches;
    } else {
      if (status != DB_OK || length != entry->second.size() ||
          std::memcmp(value, entry->second.data(), length) != 0) {
        ++model_mismatches;
      }
      ++get_count;
    }
    if (value != nullptr) db_free_value(value);
  }

  minidb_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  for (int attempt = 0; attempt < 250; ++attempt) {
    db_get_stats(db, &stats);
    if (stats.compaction_count > 0) break;
    ::Sleep(20);
  }

  double total_operations = static_cast<double>(kOperations);
  std::printf("[INFO] mixed ops: put=%d get=%d delete=%d elapsed=%.3fs throughput=%.0f ops/s\n",
              put_count, get_count, delete_count, mixed_seconds, total_operations / mixed_seconds);
  std::printf("[INFO] model verify: live=%d mismatches=%d absent_mismatches=%d\n",
              static_cast<int>(model.size()), model_mismatches, absent_mismatches);
  std::printf("[INFO] stats: mem_entries=%s sst_files=%s sst_entries=%s flush=%s compaction=%s seq=%s\n",
              ToText(stats.memtable_entries).c_str(), ToText(stats.sstable_count).c_str(),
              ToText(stats.sstable_entries).c_str(), ToText(stats.flush_count).c_str(),
              ToText(stats.compaction_count).c_str(), ToText(stats.last_sequence).c_str());

  int close_status = db_close(db);
  if (close_status != DB_OK) {
    std::printf("[FAIL] clean close failed\n");
    return 1;
  }
  db = db_open(directory.c_str(), &options);
  bool reopen_ok = db != nullptr;
  int reopen_mismatches = 0;

  if (reopen_ok) {
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < kOperations; ++i) {
      int key_index = static_cast<int>(rng() % static_cast<unsigned int>(kKeys));
      std::string key = Key(key_index);
      char* value = nullptr;
      size_t length = 0;
      int status = db_get(db, key.data(), key.size(), &value, &length);
      std::map<std::string, std::string>::const_iterator entry = model.find(key);
      if (entry == model.end()) {
        if (status != DB_NOT_FOUND) ++reopen_mismatches;
      } else {
        if (status != DB_OK || length != entry->second.size() ||
            std::memcmp(value, entry->second.data(), length) != 0) {
          ++reopen_mismatches;
        }
      }
      if (value != nullptr) db_free_value(value);
    }
    for (int i = 0; i < kKeys; ++i) {
      std::string key = Key(i);
      char* value = nullptr;
      size_t length = 0;
      int status = db_get(db, key.data(), key.size(), &value, &length);
      std::map<std::string, std::string>::const_iterator entry = model.find(key);
      if (entry == model.end()) {
        if (status != DB_NOT_FOUND) ++reopen_mismatches;
      } else if (status != DB_OK || length != entry->second.size() ||
                 std::memcmp(value, entry->second.data(), length) != 0) {
        ++reopen_mismatches;
      }
      if (value != nullptr) db_free_value(value);
    }
    double read_seconds = SecondsSince(start);
    std::printf("[INFO] reopen read: 10000 random gets + %d key scan elapsed=%.3fs throughput=%.0f gets/s\n",
                kKeys, read_seconds, (static_cast<double>(kOperations + kKeys)) / read_seconds);
    minidb_stats reopened;
    std::memset(&reopened, 0, sizeof(reopened));
    db_get_stats(db, &reopened);
    std::printf("[INFO] reopened stats: sst_files=%s sst_entries=%s flush=%s compaction=%s seq=%s\n",
                ToText(reopened.sstable_count).c_str(), ToText(reopened.sstable_entries).c_str(),
                ToText(reopened.flush_count).c_str(), ToText(reopened.compaction_count).c_str(),
                ToText(reopened.last_sequence).c_str());
    db_close(db);
  }

  int result = 0;
  if (failures != 0) {
    std::printf("[FAIL] random operation verification failures=%d\n", failures);
    result = 1;
  }
  if (model_mismatches != 0 || absent_mismatches != 0) {
    std::printf("[FAIL] final model verification mismatches\n");
    result = 1;
  }
  if (!reopen_ok) {
    std::printf("[FAIL] reopen after close failed\n");
    result = 1;
  }
  if (reopen_mismatches != 0) {
    std::printf("[FAIL] reopened reads do not match model\n");
    result = 1;
  }
  if (memtable_kb <= 256 && stats.compaction_count == 0) {
    std::printf("[FAIL] compaction did not trigger during the workload\n");
    result = 1;
  }
  if (memtable_kb > 256) {
    std::printf("[INFO] large memtable run: background compaction not required for this workload\n");
  }
  if (result == 0) {
    if (memtable_kb <= 256) {
      std::printf("[RESULT] 10000 random operations correct, persistent, compaction triggered\n");
    } else {
      std::printf("[RESULT] 10000 random operations correct and persistent\n");
    }
  }
  return result;
}
