#ifndef MINIDB_RESP_H
#define MINIDB_RESP_H

#include <cstddef>
#include <string>
#include <vector>

namespace resp {

const size_t kMaxBulkBytes = 64u * 1024u * 1024u;
const size_t kMaxArrayElements = 1024u * 1024u;
const size_t kMaxInlineBytes = 1024u * 1024u;
const size_t kMaxPendingBytes = 256u * 1024u * 1024u;

enum ParseStatus {
  kComplete,
  kIncomplete,
  kError
};

inline std::string ToText(unsigned long long value) {
  char buffer[32];
  int length = 0;
  do {
    buffer[length++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);
  std::string result;
  result.reserve(static_cast<size_t>(length));
  while (length > 0) result.push_back(buffer[--length]);
  return result;
}

inline std::string ToUpper(const std::string& text) {
  std::string result = text;
  for (size_t i = 0; i < result.size(); ++i) {
    char c = result[i];
    if (c >= 'a' && c <= 'z') result[i] = static_cast<char>(c - 'a' + 'A');
  }
  return result;
}

inline std::string ToLower(const std::string& text) {
  std::string result = text;
  for (size_t i = 0; i < result.size(); ++i) {
    char c = result[i];
    if (c >= 'A' && c <= 'Z') result[i] = static_cast<char>(c - 'A' + 'a');
  }
  return result;
}

inline std::string Printable(const std::string& text, size_t limit) {
  std::string result;
  for (size_t i = 0; i < text.size(); ++i) {
    if (result.size() >= limit) {
      result.append("...");
      break;
    }
    unsigned char c = static_cast<unsigned char>(text[i]);
    result.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
  }
  return result;
}

inline std::string Simple(const std::string& text) {
  return "+" + text + "\r\n";
}

inline std::string Error(const std::string& message) {
  return "-ERR " + message + "\r\n";
}

inline std::string PrefixedError(const std::string& prefix, const std::string& message) {
  return "-" + prefix + " " + message + "\r\n";
}

inline std::string Integer(long long value) {
  char buffer[32];
  int length = 0;
  bool negative = value < 0;
  unsigned long long magnitude =
      negative ? static_cast<unsigned long long>(-(value + 1)) + 1
               : static_cast<unsigned long long>(value);
  do {
    buffer[length++] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude > 0);
  std::string result;
  if (negative) result.push_back('-');
  while (length > 0) result.push_back(buffer[--length]);
  return ":" + result + "\r\n";
}

inline std::string Bulk(const std::string& value) {
  return "$" + ToText(value.size()) + "\r\n" + value + "\r\n";
}

inline std::string NullBulk() {
  return "$-1\r\n";
}

inline std::string ArrayHeader(size_t count) {
  return "*" + ToText(count) + "\r\n";
}

inline bool ParseSigned(const std::string& text, long long* value) {
  if (text.empty()) return false;
  size_t position = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    position = 1;
  } else if (text[0] == '+') {
    position = 1;
  }
  if (position >= text.size()) return false;
  unsigned long long magnitude = 0;
  for (; position < text.size(); ++position) {
    char c = text[position];
    if (c < '0' || c > '9') return false;
    unsigned long long digit = static_cast<unsigned long long>(c - '0');
    if (magnitude > (0x7fffffffffffffffULL - digit) / 10) return false;
    magnitude = magnitude * 10 + digit;
  }
  *value = negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
  return true;
}

inline bool FindLine(const std::string& buffer, size_t offset, std::string* line,
                     size_t* next) {
  size_t position = buffer.find("\r\n", offset);
  if (position == std::string::npos) return false;
  *line = buffer.substr(offset, position - offset);
  *next = position + 2;
  return true;
}

inline void SplitInline(const std::string& line, std::vector<std::string>* args) {
  size_t position = 0;
  while (position < line.size()) {
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) {
      ++position;
    }
    size_t start = position;
    while (position < line.size() && line[position] != ' ' && line[position] != '\t') {
      ++position;
    }
    if (position > start) args->push_back(line.substr(start, position - start));
  }
}

inline ParseStatus ParseArray(const std::string& buffer, std::vector<std::string>* args,
                              size_t* consumed, std::string* error) {
  std::string line;
  size_t next = 0;
  if (!FindLine(buffer, 1, &line, &next)) {
    if (buffer.size() > kMaxInlineBytes) {
      *error = "invalid multibulk length";
      return kError;
    }
    return kIncomplete;
  }
  long long count = 0;
  if (!ParseSigned(line, &count) || count < 0 ||
      count > static_cast<long long>(kMaxArrayElements)) {
    *error = "invalid multibulk length";
    return kError;
  }
  size_t offset = next;
  if (count == 0) {
    *consumed = offset;
    return kComplete;
  }
  args->reserve(static_cast<size_t>(count));
  for (long long index = 0; index < count; ++index) {
    if (offset >= buffer.size()) return kIncomplete;
    if (buffer[offset] != '$') {
      *error = "expected '$' before bulk argument";
      return kError;
    }
    size_t element_next = 0;
    if (!FindLine(buffer, offset + 1, &line, &element_next)) {
      if (buffer.size() - offset > kMaxInlineBytes) {
        *error = "invalid bulk length";
        return kError;
      }
      return kIncomplete;
    }
    long long length = 0;
    if (!ParseSigned(line, &length) || length < 0 ||
        length > static_cast<long long>(kMaxBulkBytes)) {
      *error = "invalid bulk length";
      return kError;
    }
    size_t body = element_next;
    size_t needed = body + static_cast<size_t>(length);
    if (needed < body || buffer.size() < needed + 2) return kIncomplete;
    if (buffer.compare(needed, 2, "\r\n") != 0) {
      *error = "invalid bulk terminator";
      return kError;
    }
    args->push_back(buffer.substr(body, static_cast<size_t>(length)));
    offset = needed + 2;
  }
  *consumed = offset;
  return kComplete;
}

inline ParseStatus ParseRequest(const std::string& buffer, std::vector<std::string>* args,
                                size_t* consumed, std::string* error) {
  args->clear();
  *consumed = 0;
  if (buffer.empty()) return kIncomplete;
  if (buffer[0] == '*') return ParseArray(buffer, args, consumed, error);
  size_t line_end = buffer.find("\r\n");
  if (line_end == std::string::npos) {
    if (buffer.size() > kMaxInlineBytes) {
      *error = "inline request too long";
      return kError;
    }
    return kIncomplete;
  }
  if (line_end > kMaxInlineBytes) {
    *error = "inline request too long";
    return kError;
  }
  SplitInline(buffer.substr(0, line_end), args);
  *consumed = line_end + 2;
  return kComplete;
}

}

#endif
