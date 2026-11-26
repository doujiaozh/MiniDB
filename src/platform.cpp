#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "platform.h"

#include <algorithm>
#include <cstring>

namespace mdb {

namespace {

HANDLE ToHandle(void* handle) {
  return static_cast<HANDLE>(handle);
}

void* FromHandle(HANDLE handle) {
  return static_cast<void*>(handle);
}

}

std::string PathJoin(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  char last = dir[dir.size() - 1];
  if (last == '\\' || last == '/') return dir + name;
  return dir + "\\" + name;
}

bool EnsureDir(const std::string& path) {
  if (path.empty()) return false;
  std::string normalized = path;
  for (size_t i = 0; i < normalized.size(); ++i) {
    if (normalized[i] == '/') normalized[i] = '\\';
  }
  std::string current;
  size_t index = 0;
  if (normalized.size() >= 2 && normalized[1] == ':') {
    current = normalized.substr(0, 2);
    index = 2;
  }
  for (; index <= normalized.size(); ++index) {
    if (index == normalized.size() || normalized[index] == '\\') {
      bool drive_only = current.size() == 2 && current[1] == ':';
      if (!current.empty() && !drive_only) {
        if (!::CreateDirectoryA(current.c_str(), nullptr)) {
          DWORD code = ::GetLastError();
          if (code != ERROR_ALREADY_EXISTS) return false;
        }
      }
      if (index < normalized.size()) current.push_back('\\');
    } else {
      current.push_back(normalized[index]);
    }
  }
  return true;
}

bool FileExists(const std::string& path) {
  DWORD attributes = ::GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool RemoveFile(const std::string& path) {
  if (::DeleteFileA(path.c_str())) return true;
  return ::GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool RenameFile(const std::string& from, const std::string& to) {
  if (::MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  return false;
}

void ListFiles(const std::string& dir, std::vector<std::string>* names) {
  if (names == nullptr) return;
  std::string pattern = PathJoin(dir, "*");
  WIN32_FIND_DATAA data;
  HANDLE find = ::FindFirstFileA(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    names->push_back(std::string(data.cFileName));
  } while (::FindNextFileA(find, &data));
  ::FindClose(find);
}

bool GetFileSize(const std::string& path, uint64_t* size) {
  HANDLE handle = ::CreateFileA(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER info;
  info.QuadPart = 0;
  bool ok = ::GetFileSizeEx(handle, &info) != 0;
  ::CloseHandle(handle);
  if (!ok) return false;
  *size = static_cast<uint64_t>(info.QuadPart);
  return true;
}

bool TruncateFile(const std::string& path, uint64_t size) {
  HANDLE handle = ::CreateFileA(path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER position;
  position.QuadPart = static_cast<LONGLONG>(size);
  bool ok = ::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0;
  if (ok) ok = ::SetEndOfFile(handle) != 0;
  ::CloseHandle(handle);
  return ok;
}

FileWriter::FileWriter() : handle_(nullptr), size_(0) {}

FileWriter::~FileWriter() {
  Close();
}

bool FileWriter::Open(const std::string& path, bool append) {
  Close();
  HANDLE handle = ::CreateFileA(path.c_str(), GENERIC_WRITE | GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER info;
  info.QuadPart = 0;
  if (append) {
    if (!::GetFileSizeEx(handle, &info)) {
      ::CloseHandle(handle);
      return false;
    }
  } else {
    if (!::SetEndOfFile(handle)) {
      ::CloseHandle(handle);
      return false;
    }
  }
  LARGE_INTEGER position;
  position.QuadPart = info.QuadPart;
  if (!::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    ::CloseHandle(handle);
    return false;
  }
  handle_ = FromHandle(handle);
  size_ = static_cast<uint64_t>(info.QuadPart);
  return true;
}

bool FileWriter::Append(const char* data, size_t size) {
  if (handle_ == nullptr) return false;
  size_t total = 0;
  while (total < size) {
    size_t remaining = size - total;
    DWORD chunk = static_cast<DWORD>(remaining > (1u << 30) ? (1u << 30) : remaining);
    DWORD written = 0;
    if (!::WriteFile(ToHandle(handle_), data + total, chunk, &written, nullptr)) return false;
    if (written == 0) return false;
    total += written;
  }
  size_ += size;
  return true;
}

bool FileWriter::Sync() {
  if (handle_ == nullptr) return false;
  return ::FlushFileBuffers(ToHandle(handle_)) != 0;
}

bool FileWriter::Close() {
  if (handle_ == nullptr) return true;
  bool ok = ::CloseHandle(ToHandle(handle_)) != 0;
  handle_ = nullptr;
  return ok;
}

FileReader::FileReader() : handle_(nullptr), size_(0) {}

FileReader::~FileReader() {
  Close();
}

bool FileReader::Open(const std::string& path) {
  Close();
  HANDLE handle = ::CreateFileA(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER info;
  info.QuadPart = 0;
  if (!::GetFileSizeEx(handle, &info)) {
    ::CloseHandle(handle);
    return false;
  }
  handle_ = FromHandle(handle);
  size_ = static_cast<uint64_t>(info.QuadPart);
  return true;
}

bool FileReader::ReadAt(uint64_t offset, void* buffer, size_t size, size_t* bytes_read) const {
  if (bytes_read != nullptr) *bytes_read = 0;
  if (handle_ == nullptr) return false;
  char* destination = static_cast<char*>(buffer);
  size_t total = 0;
  while (total < size) {
    size_t remaining = size - total;
    DWORD chunk = static_cast<DWORD>(remaining > (1u << 30) ? (1u << 30) : remaining);
    uint64_t position = offset + total;
    OVERLAPPED overlapped;
    std::memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = static_cast<DWORD>(position & 0xffffffffull);
    overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
    DWORD received = 0;
    if (!::ReadFile(ToHandle(handle_), destination + total, chunk, &received, &overlapped)) {
      DWORD code = ::GetLastError();
      if (code == ERROR_IO_PENDING) {
        if (!::GetOverlappedResult(ToHandle(handle_), &overlapped, &received, TRUE)) {
          DWORD wait_code = ::GetLastError();
          if (wait_code == ERROR_HANDLE_EOF) {
            if (bytes_read != nullptr) *bytes_read = total;
            return true;
          }
          return false;
        }
      } else if (code == ERROR_HANDLE_EOF) {
        if (bytes_read != nullptr) *bytes_read = total;
        return true;
      } else {
        return false;
      }
    }
    total += received;
    if (received == 0) break;
  }
  if (bytes_read != nullptr) *bytes_read = total;
  return true;
}

bool FileReader::ReadAtExact(uint64_t offset, void* buffer, size_t size) const {
  size_t received = 0;
  if (!ReadAt(offset, buffer, size, &received)) return false;
  return received == size;
}

void FileReader::Close() {
  if (handle_ == nullptr) return;
  ::CloseHandle(ToHandle(handle_));
  handle_ = nullptr;
  size_ = 0;
}

}
