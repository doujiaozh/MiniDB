#ifndef MINIDB_PLATFORM_H
#define MINIDB_PLATFORM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdb {

std::string PathJoin(const std::string& dir, const std::string& name);
bool EnsureDir(const std::string& path);
bool FileExists(const std::string& path);
bool RemoveFile(const std::string& path);
bool RenameFile(const std::string& from, const std::string& to);
void ListFiles(const std::string& dir, std::vector<std::string>* names);
bool GetFileSize(const std::string& path, uint64_t* size);
bool TruncateFile(const std::string& path, uint64_t size);

class FileWriter {
 public:
  FileWriter();
  ~FileWriter();

  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;

  bool Open(const std::string& path, bool append);
  bool Append(const char* data, size_t size);
  bool Sync();
  bool Close();
  bool IsOpen() const { return handle_ != nullptr; }
  uint64_t size() const { return size_; }

 private:
  void* handle_;
  uint64_t size_;
};

class FileReader {
 public:
  FileReader();
  ~FileReader();

  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  bool Open(const std::string& path);
  bool ReadAt(uint64_t offset, void* buffer, size_t size, size_t* bytes_read) const;
  bool ReadAtExact(uint64_t offset, void* buffer, size_t size) const;
  uint64_t size() const { return size_; }
  bool IsOpen() const { return handle_ != nullptr; }
  void Close();

 private:
  void* handle_;
  uint64_t size_;
};

}

#endif
