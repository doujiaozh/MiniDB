#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "minidb.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
  if (condition) return;
  ++g_failures;
  std::printf("[FAIL] %s\n", message.c_str());
}

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
  std::snprintf(buffer, sizeof(buffer), "value-%08d-v%d", index, version);
  return std::string(buffer);
}

int ValueVersion(int index) {
  return (index >= 100 && index < 200) ? 2 : 1;
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

std::string ExecutablePath() {
  char buffer[MAX_PATH * 2];
  DWORD length = ::GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
  if (length == 0) return std::string("test_persist.exe");
  return std::string(buffer, length);
}

std::string ProjectRoot() {
  std::string path = ExecutablePath();
  size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos) return std::string(".");
  return path.substr(0, slash) + "\\..";
}

int RunProcess(const std::string& executable, const std::string& arguments) {
  std::string command = "\"" + executable + "\" " + arguments;
  std::vector<char> command_line(command.begin(), command.end());
  command_line.push_back('\0');
  STARTUPINFOA startup;
  std::memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information;
  std::memset(&information, 0, sizeof(information));
  if (!::CreateProcessA(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &information)) {
    std::printf("[FAIL] cannot start child process, error=%lu\n", ::GetLastError());
    return -1;
  }
  ::WaitForSingleObject(information.hProcess, INFINITE);
  DWORD exit_code = 1;
  ::GetExitCodeProcess(information.hProcess, &exit_code);
  ::CloseHandle(information.hThread);
  ::CloseHandle(information.hProcess);
  return static_cast<int>(exit_code);
}

int CrashWriter(const std::string& directory) {
  minidb_options options;
  db_options_init(&options);
  options.memtable_size_bytes = 1024 * 1024;
  minidb* db = db_open(directory.c_str(), &options);
  if (db == nullptr) {
    std::printf("[FAIL] crash writer cannot open database\n");
    return 2;
  }
  for (int i = 0; i < 500; ++i) {
    std::string key = Key(i);
    std::string value = Value(i, 1);
    if (db_put(db, key.data(), key.size(), value.data(), value.size()) != DB_OK) return 3;
  }
  for (int i = 100; i < 200; ++i) {
    std::string key = Key(i);
    std::string value = Value(i, 2);
    if (db_put(db, key.data(), key.size(), value.data(), value.size()) != DB_OK) return 4;
  }
  for (int i = 0; i < 500; i += 3) {
    std::string key = Key(i);
    if (db_delete(db, key.data(), key.size()) != DB_OK) return 5;
  }
  std::printf("[INFO] crash writer committed 500 puts, 100 overwrites, 167 deletes\n");
  std::printf("[INFO] crash writer exits without close or flush\n");
  std::fflush(stdout);
  std::_Exit(0);
}

void VerifyRecoveredState(const std::string& directory, const char* label, bool expect_sstable) {
  minidb_options options;
  db_options_init(&options);
  minidb* db = db_open(directory.c_str(), &options);
  Check(db != nullptr, std::string(label) + ": open database");
  if (db == nullptr) {
    std::printf("[FAIL] %s: %s\n", label, db_last_error(nullptr));
    return;
  }

  int present = 0;
  int missing = 0;
  int resurrected = 0;
  for (int i = 0; i < 500; ++i) {
    std::string key = Key(i);
    char* value = nullptr;
    size_t length = 0;
    int status = db_get(db, key.data(), key.size(), &value, &length);
    bool deleted = (i % 3) == 0;
    if (deleted) {
      if (status != DB_NOT_FOUND) ++resurrected;
    } else {
      std::string expected = Value(i, ValueVersion(i));
      if (status == DB_OK && length == expected.size() &&
          std::memcmp(value, expected.data(), length) == 0) {
        ++present;
      } else {
        ++missing;
      }
    }
    if (value != nullptr) db_free_value(value);
  }

  minidb_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  db_get_stats(db, &stats);
  std::printf("[INFO] %s: live=%d missing=%d resurrected=%d sst_files=%s flush=%s compaction=%s\n",
              label, present, missing, resurrected, ToText(stats.sstable_count).c_str(),
              ToText(stats.flush_count).c_str(), ToText(stats.compaction_count).c_str());

  Check(missing == 0, std::string(label) + ": all live records recovered");
  Check(resurrected == 0, std::string(label) + ": deleted records stay deleted");
  Check(present == 333, std::string(label) + ": exact live record count");
  if (expect_sstable) {
    Check(stats.sstable_count >= 1, std::string(label) + ": recovery produced a persistent table");
  }
  db_close(db);
}

void RunCrashRecovery() {
  std::printf("[STAGE] crash recovery through the write ahead log\n");
  const std::string directory = ProjectRoot() + "\\testdata\\persist";
  RemoveTree(directory);

  int child_status = RunProcess(ExecutablePath(), "crash \"" + directory + "\"");
  Check(child_status == 0, "crash writer child process completed");

  VerifyRecoveredState(directory, "after crash", true);
  VerifyRecoveredState(directory, "after clean restart", false);
}

void VerifyComputedState(const std::string& directory, const char* label) {
  minidb_options options;
  db_options_init(&options);
  minidb* db = db_open(directory.c_str(), &options);
  Check(db != nullptr, std::string(label) + ": open database");
  if (db == nullptr) return;

  int live_ok = 0;
  int live_bad = 0;
  int absent_ok = 0;
  int absent_bad = 0;
  for (int i = 0; i < 4000; ++i) {
    std::string key = Key(i);
    char* value = nullptr;
    size_t length = 0;
    int status = db_get(db, key.data(), key.size(), &value, &length);
    if ((i % 2) == 0) {
      if (status == DB_NOT_FOUND) {
        ++absent_ok;
      } else {
        ++absent_bad;
      }
    } else {
      std::string expected = Value(i, 1);
      if (status == DB_OK && length == expected.size() &&
          std::memcmp(value, expected.data(), length) == 0) {
        ++live_ok;
      } else {
        ++live_bad;
      }
    }
    if (value != nullptr) db_free_value(value);
  }

  minidb_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  db_get_stats(db, &stats);
  std::printf("[INFO] %s: live_ok=%d live_bad=%d absent_ok=%d absent_bad=%d sst_files=%s sst_entries=%s\n",
              label, live_ok, live_bad, absent_ok, absent_bad,
              ToText(stats.sstable_count).c_str(), ToText(stats.sstable_entries).c_str());
  Check(live_bad == 0, std::string(label) + ": live records correct");
  Check(absent_bad == 0, std::string(label) + ": deleted records absent");
  Check(live_ok == 2000, std::string(label) + ": exact live count");
  db_close(db);
}

void RunCompaction() {
  std::printf("[STAGE] background compaction and tombstone collection\n");
  const std::string directory = ProjectRoot() + "\\testdata\\compact";
  RemoveTree(directory);

  minidb_options options;
  db_options_init(&options);
  options.memtable_size_bytes = 64 * 1024;
  options.compaction_trigger_files = 4;

  minidb* db = db_open(directory.c_str(), &options);
  Check(db != nullptr, "compaction database opened");
  if (db == nullptr) return;

  int put_failures = 0;
  int delete_failures = 0;
  for (int i = 0; i < 4000; ++i) {
    std::string key = Key(i);
    std::string value = Value(i, 1);
    if (db_put(db, key.data(), key.size(), value.data(), value.size()) != DB_OK) ++put_failures;
  }
  for (int i = 0; i < 4000; i += 2) {
    std::string key = Key(i);
    if (db_delete(db, key.data(), key.size()) != DB_OK) ++delete_failures;
  }
  Check(put_failures == 0, "all compaction puts accepted");
  Check(delete_failures == 0, "all compaction deletes accepted");

  minidb_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  for (int attempt = 0; attempt < 250; ++attempt) {
    db_get_stats(db, &stats);
    if (stats.compaction_count > 0) break;
    ::Sleep(20);
  }
  std::printf("[INFO] background compaction count=%s sst_files=%s\n",
              ToText(stats.compaction_count).c_str(), ToText(stats.sstable_count).c_str());
  Check(stats.compaction_count > 0, "background compaction triggered automatically");

  Check(db_flush(db) == DB_OK, "explicit flush before forced compaction");
  Check(db_compact(db) == DB_OK, "forced compaction completed");

  std::memset(&stats, 0, sizeof(stats));
  db_get_stats(db, &stats);
  std::printf("[INFO] after forced compaction: sst_files=%s sst_entries=%s compaction=%s\n",
              ToText(stats.sstable_count).c_str(), ToText(stats.sstable_entries).c_str(),
              ToText(stats.compaction_count).c_str());
  Check(stats.sstable_count == 1, "compaction reduced storage to one table");
  Check(stats.sstable_entries == 2000, "tombstones and stale versions removed");

  db_close(db);
  VerifyComputedState(directory, "after compaction restart");
}

}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "crash") == 0) {
    return CrashWriter(argv[2]);
  }

  std::printf("[STAGE] minidb persistence and recovery verification\n");
  RunCrashRecovery();
  RunCompaction();

  if (g_failures == 0) {
    std::printf("[RESULT] all persistence checks passed\n");
    return 0;
  }
  std::printf("[RESULT] failures=%d\n", g_failures);
  return 1;
}
