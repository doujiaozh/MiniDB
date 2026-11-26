#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "minidb.h"

namespace {

std::string Key(int index) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "key-%08d", index);
  return std::string(buffer);
}

std::string Value(int index) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "value-%08d-fixed", index);
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

}

int main() {
  const std::string directory = ExeDirectory() + "\\..\\testdata\\concurrent";
  RemoveTree(directory);

  minidb_options options;
  db_options_init(&options);
  options.memtable_size_bytes = 128 * 1024;
  options.compaction_trigger_files = 3;

  minidb* db = db_open(directory.c_str(), &options);
  if (db == nullptr) {
    std::printf("[FAIL] cannot open database\n");
    return 1;
  }

  const int kKeysPerWriter = 500;
  const int kWriterCount = 4;
  const int kKeyCount = kKeysPerWriter * kWriterCount;
  const int kOpsPerWriter = 2500;
  const int kReadsPerReader = 20000;
  const int kReaderCount = 4;

  std::vector<int> final_state(static_cast<size_t>(kKeyCount), 0);
  std::atomic<int> write_failures(0);
  std::atomic<int> read_failures(0);
  std::atomic<int> torn_reads(0);

  std::printf("[STAGE] concurrent writers and readers\n");
  std::printf("[INFO] writers=%d readers=%d keys=%d ops_per_writer=%d\n", kWriterCount,
              kReaderCount, kKeyCount, kOpsPerWriter);

  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;

  for (int writer = 0; writer < kWriterCount; ++writer) {
    threads.push_back(std::thread([&, writer]() {
      std::mt19937 rng(static_cast<unsigned int>(7777 + writer));
      int first = writer * kKeysPerWriter;
      for (int op = 0; op < kOpsPerWriter; ++op) {
        int index = first + static_cast<int>(rng() % static_cast<unsigned int>(kKeysPerWriter));
        std::string key = Key(index);
        unsigned int action = rng() % 100u;
        if (action < 70) {
          std::string value = Value(index);
          if (db_put(db, key.data(), key.size(), value.data(), value.size()) != DB_OK) {
            write_failures.fetch_add(1);
            continue;
          }
          final_state[static_cast<size_t>(index)] = 1;
        } else {
          if (db_delete(db, key.data(), key.size()) != DB_OK) {
            write_failures.fetch_add(1);
            continue;
          }
          final_state[static_cast<size_t>(index)] = 0;
        }
      }
    }));
  }

  for (int reader = 0; reader < kReaderCount; ++reader) {
    threads.push_back(std::thread([&, reader]() {
      std::mt19937 rng(static_cast<unsigned int>(9999 + reader));
      for (int op = 0; op < kReadsPerReader; ++op) {
        int index = static_cast<int>(rng() % static_cast<unsigned int>(kKeyCount));
        std::string key = Key(index);
        char* value = nullptr;
        size_t length = 0;
        int status = db_get(db, key.data(), key.size(), &value, &length);
        if (status == DB_OK) {
          std::string expected = Value(index);
          if (length != expected.size() || std::memcmp(value, expected.data(), length) != 0) {
            torn_reads.fetch_add(1);
          }
        } else if (status != DB_NOT_FOUND) {
          read_failures.fetch_add(1);
        }
        if (value != nullptr) db_free_value(value);
      }
    }));
  }

  for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  int final_mismatches = 0;
  for (int index = 0; index < kKeyCount; ++index) {
    std::string key = Key(index);
    char* value = nullptr;
    size_t length = 0;
    int status = db_get(db, key.data(), key.size(), &value, &length);
    if (final_state[static_cast<size_t>(index)] == 1) {
      std::string expected = Value(index);
      if (status != DB_OK || length != expected.size() ||
          std::memcmp(value, expected.data(), length) != 0) {
        ++final_mismatches;
      }
    } else if (status != DB_NOT_FOUND) {
      ++final_mismatches;
    }
    if (value != nullptr) db_free_value(value);
  }

  minidb_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  db_get_stats(db, &stats);

  db_close(db);

  std::printf("[INFO] elapsed=%.3fs total_ops=%d throughput=%.0f ops/s\n", elapsed,
              kWriterCount * kOpsPerWriter + kReaderCount * kReadsPerReader,
              static_cast<double>(kWriterCount * kOpsPerWriter + kReaderCount * kReadsPerReader) /
                  elapsed);
  std::printf("[INFO] write_failures=%d read_failures=%d torn_reads=%d final_mismatches=%d\n",
              write_failures.load(), read_failures.load(), torn_reads.load(), final_mismatches);

  int result = 0;
  if (write_failures.load() != 0 || read_failures.load() != 0 || torn_reads.load() != 0 ||
      final_mismatches != 0) {
    std::printf("[FAIL] concurrency validation failed\n");
    result = 1;
  } else {
    std::printf("[RESULT] concurrent access consistent, no torn reads, no cross key corruption\n");
  }
  return result;
}
