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

std::string Value(int index, int generation) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer),
                "value-%08d-generation-%02d-payload-0123456789-abcdefghijklmnopqrstuvwxyz",
                index, generation % 100);
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

struct LevelEntry {
  int level;
  std::string number;
  unsigned long long entries;
  std::string smallest;
  std::string largest;
};

void SplitSpaces(const std::string& line, std::vector<std::string>* tokens) {
  size_t position = 0;
  while (position < line.size()) {
    while (position < line.size() && line[position] == ' ') ++position;
    size_t start = position;
    while (position < line.size() && line[position] != ' ') ++position;
    if (position > start) tokens->push_back(line.substr(start, position - start));
  }
}

unsigned long long ParseNumber(const std::string& text) {
  unsigned long long value = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return 0;
    value = value * 10 + static_cast<unsigned long long>(text[i] - '0');
  }
  return value;
}

bool ParseLevels(const std::string& text, std::vector<LevelEntry>* entries) {
  entries->clear();
  size_t position = 0;
  while (position < text.size()) {
    size_t end = text.find('\n', position);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(position, end - position);
    position = end + 1;
    if (line.empty()) continue;
    std::vector<std::string> tokens;
    SplitSpaces(line, &tokens);
    if (tokens.size() != 6 || tokens[0] != "LEVEL") return false;
    LevelEntry entry;
    entry.level = static_cast<int>(ParseNumber(tokens[1]));
    entry.number = tokens[2];
    entry.entries = ParseNumber(tokens[3]);
    entry.smallest = tokens[4];
    entry.largest = tokens[5];
    if (entry.level < 0 || entry.level > 3) return false;
    if (entry.entries == 0) return false;
    entries->push_back(entry);
  }
  return true;
}

void VerifyLevelOrder(const std::vector<LevelEntry>& entries, const char* label) {
  for (int level = 1; level < 4; ++level) {
    std::string previous_largest;
    bool have_previous = false;
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].level != level) continue;
      if (have_previous) {
        Check(entries[i].smallest > previous_largest,
              std::string(label) + ": level " + ToText(level) + " ranges do not overlap");
      }
      previous_largest = entries[i].largest;
      have_previous = true;
    }
  }
}

bool WaitForDeepLevels(minidb* db, int timeout_ms) {
  int stable = 0;
  for (int elapsed = 0; elapsed < timeout_ms; elapsed += 25) {
    minidb_stats stats;
    std::memset(&stats, 0, sizeof(stats));
    db_get_stats(db, &stats);
    bool has_level1 = stats.level_files[1] > 0;
    bool has_deeper = stats.level_files[2] > 0 || stats.level_files[3] > 0;
    if (has_level1 && has_deeper) {
      ++stable;
      if (stable >= 4) return true;
    } else {
      stable = 0;
    }
    ::Sleep(25);
  }
  return false;
}

void PrintStats(const char* label, const minidb_stats& stats) {
  std::printf("[INFO] %s levels=%s/%s/%s/%s entries=%s compaction=%s\n", label,
              ToText(stats.level_files[0]).c_str(), ToText(stats.level_files[1]).c_str(),
              ToText(stats.level_files[2]).c_str(), ToText(stats.level_files[3]).c_str(),
              ToText(stats.sstable_entries).c_str(), ToText(stats.compaction_count).c_str());
}

int VerifyKeys(minidb* db, int total, int stride, int generation) {
  int mismatches = 0;
  for (int i = 0; i < total; ++i) {
    std::string key = Key(i);
    char* value = nullptr;
    size_t length = 0;
    int status = db_get(db, key.data(), key.size(), &value, &length);
    if (stride > 0 && (i % stride) == 0) {
      if (status != DB_NOT_FOUND) ++mismatches;
    } else {
      std::string expected = Value(i, generation);
      if (status != DB_OK || length != expected.size() ||
          std::memcmp(value, expected.data(), length) != 0) {
        ++mismatches;
      }
    }
    if (value != nullptr) db_free_value(value);
  }
  return mismatches;
}

}

int main() {
  const std::string directory = ExeDirectory() + "\\..\\testdata\\levels";
  RemoveTree(directory);

  minidb_options options;
  db_options_init(&options);
  options.memtable_size_bytes = 64 * 1024;
  options.compaction_trigger_files = 3;
  options.level_base_size_bytes = 128 * 1024;
  options.level_size_multiplier = 4;
  options.target_sstable_size_bytes = 128 * 1024;

  std::printf("[STAGE] leveled compaction verification\n");

  minidb* db = db_open(directory.c_str(), &options);
  Check(db != nullptr, "open leveled database");
  if (db == nullptr) {
    std::printf("[FAIL] %s\n", db_last_error(nullptr));
    return 1;
  }

  const int kKeys = 8000;
  int put_failures = 0;
  for (int i = 0; i < kKeys; ++i) {
    std::string key = Key(i);
    std::string value = Value(i, 1);
    if (db_put(db, key.data(), key.size(), value.data(), value.size()) != DB_OK) ++put_failures;
  }
  Check(put_failures == 0, "all leveled puts accepted");
  Check(db_flush(db) == DB_OK, "flush before leveled verification");

  bool reached = WaitForDeepLevels(db, 20000);
  Check(reached, "background compaction populated level 1 and a deeper level");

  minidb_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  db_get_stats(db, &stats);
  PrintStats("after writes", stats);
  Check(stats.compaction_count > 0, "leveled compaction executed");

  Check(db_verify_levels(db) == DB_OK, "levels are sorted and non overlapping");

  char* levels_text = nullptr;
  size_t levels_len = 0;
  Check(db_levels_text(db, &levels_text, &levels_len) == DB_OK, "level summary available");
  std::string summary;
  if (levels_text != nullptr) {
    summary.assign(levels_text, levels_len);
    db_free_value(levels_text);
  }
  std::vector<LevelEntry> entries;
  Check(ParseLevels(summary, &entries), "level summary parses");
  VerifyLevelOrder(entries, "after writes");

  int mismatches = VerifyKeys(db, kKeys, 0, 1);
  Check(mismatches == 0, "all keys read correctly across levels");

  int delete_failures = 0;
  for (int i = 0; i < kKeys; i += 3) {
    std::string key = Key(i);
    if (db_delete(db, key.data(), key.size()) != DB_OK) ++delete_failures;
  }
  Check(delete_failures == 0, "cross level deletes accepted");
  mismatches = VerifyKeys(db, kKeys, 3, 1);
  Check(mismatches == 0, "tombstones shadow values in deeper levels");

  Check(db_flush(db) == DB_OK, "flush tombstones");
  WaitForDeepLevels(db, 20000);
  Check(db_verify_levels(db) == DB_OK, "levels remain ordered after tombstones");
  mismatches = VerifyKeys(db, kKeys, 3, 1);
  Check(mismatches == 0, "deleted keys stay deleted after flush");

  Check(db_compact(db) == DB_OK, "forced full compaction to bottom level");
  std::memset(&stats, 0, sizeof(stats));
  db_get_stats(db, &stats);
  PrintStats("after full compaction", stats);
  Check(stats.level_files[0] == 0 && stats.level_files[1] == 0 && stats.level_files[2] == 0,
        "full compaction moved all tables to the bottom level");
  Check(stats.level_files[3] > 0, "bottom level contains compacted tables");
  Check(db_verify_levels(db) == DB_OK, "bottom level is sorted and non overlapping");

  db_close(db);

  db = db_open(directory.c_str(), &options);
  Check(db != nullptr, "reopen leveled database");
  if (db != nullptr) {
    mismatches = VerifyKeys(db, kKeys, 3, 1);
    Check(mismatches == 0, "leveled data survives restart");
    Check(db_verify_levels(db) == DB_OK, "recovered levels are ordered");
    std::memset(&stats, 0, sizeof(stats));
    db_get_stats(db, &stats);
    PrintStats("after restart", stats);
    db_close(db);
  }

  if (g_failures == 0) {
    std::printf("[RESULT] leveled compaction valid: multi level, ordered, tombstone safe\n");
    return 0;
  }
  std::printf("[RESULT] failures=%d\n", g_failures);
  return 1;
}
