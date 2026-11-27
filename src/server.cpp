#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "minidb.h"

namespace {

const size_t kMaxLineBytes = 1024 * 1024;
const size_t kMaxBulkBytes = 64u * 1024u * 1024u;
const size_t kMaxPendingBytes = 128u * 1024u * 1024u;

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

bool ParseUnsigned(const std::string& text, uint64_t* value) {
  if (text.empty()) return false;
  uint64_t result = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c < '0' || c > '9') return false;
    result = result * 10 + static_cast<uint64_t>(c - '0');
  }
  *value = result;
  return true;
}

void SplitSpaces(const std::string& line, std::vector<std::string>* tokens) {
  size_t position = 0;
  while (position < line.size()) {
    while (position < line.size() && line[position] == ' ') ++position;
    size_t start = position;
    while (position < line.size() && line[position] != ' ') ++position;
    if (position > start) tokens->push_back(line.substr(start, position - start));
  }
}

std::string Bulk(const std::string& data) {
  return "$" + ToText(data.size()) + "\r\n" + data + "\r\n";
}

std::string Error(const std::string& message) {
  return "-ERR " + message + "\r\n";
}

struct Config {
  std::string data_dir;
  int port;
  int workers;
  size_t memtable_bytes;
  size_t block_bytes;
  int l0_trigger;
  size_t level_base_bytes;
  int level_multiplier;
  size_t target_sstable_bytes;
  int sync_on_write;

  Config()
      : data_dir("testdata\\server"),
        port(6379),
        workers(32),
        memtable_bytes(4 * 1024 * 1024),
        block_bytes(4096),
        l0_trigger(4),
        level_base_bytes(1024 * 1024),
        level_multiplier(4),
        target_sstable_bytes(1024 * 1024),
        sync_on_write(0) {}
};

bool SendAll(SOCKET socket, const char* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    size_t remaining = size - sent;
    int chunk = static_cast<int>(remaining > (1u << 20) ? (1u << 20) : remaining);
    int written = send(socket, data + sent, chunk, 0);
    if (written <= 0) return false;
    sent += static_cast<size_t>(written);
  }
  return true;
}

class Server {
 public:
  Server() : db_(nullptr), listener_(INVALID_SOCKET), stopping_(false),
             shutdown_requested_(false) {}

  ~Server() {
    StopWorkers();
    if (db_ != nullptr) {
      db_close(db_);
      db_ = nullptr;
    }
    if (listener_ != INVALID_SOCKET) {
      closesocket(listener_);
      listener_ = INVALID_SOCKET;
    }
  }

  bool Start(const Config& config, std::string* error) {
    config_ = config;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      if (error != nullptr) *error = "WSAStartup failed";
      return false;
    }
    wsa_started_ = true;

    minidb_options options;
    db_options_init(&options);
    options.memtable_size_bytes = config_.memtable_bytes;
    options.block_size_bytes = config_.block_bytes;
    options.compaction_trigger_files = config_.l0_trigger;
    options.level_base_size_bytes = config_.level_base_bytes;
    options.level_size_multiplier = config_.level_multiplier;
    options.target_sstable_size_bytes = config_.target_sstable_bytes;
    options.sync_on_write = config_.sync_on_write;
    db_ = db_open(config_.data_dir.c_str(), &options);
    if (db_ == nullptr) {
      if (error != nullptr) *error = "cannot open database";
      return false;
    }

    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) {
      if (error != nullptr) *error = "cannot create listening socket";
      return false;
    }
    BOOL reuse = TRUE;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(config_.port));
    if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      if (error != nullptr) *error = "cannot bind port " + ToText(config_.port);
      return false;
    }
    if (listen(listener_, 128) != 0) {
      if (error != nullptr) *error = "cannot listen";
      return false;
    }

    int worker_count = config_.workers < 1 ? 1 : config_.workers;
    workers_.reserve(static_cast<size_t>(worker_count));
    for (int i = 0; i < worker_count; ++i) {
      workers_.push_back(std::thread(&Server::WorkerLoop, this));
    }
    return true;
  }

  void Run() {
    AcceptLoop();
    StopWorkers();
    if (db_ != nullptr) {
      db_close(db_);
      db_ = nullptr;
    }
    if (listener_ != INVALID_SOCKET) {
      closesocket(listener_);
      listener_ = INVALID_SOCKET;
    }
    if (wsa_started_) {
      WSACleanup();
      wsa_started_ = false;
    }
  }

 private:
  void AcceptLoop() {
    while (!shutdown_requested_.load()) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(listener_, &read_set);
      timeval timeout;
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;
      int ready = select(0, &read_set, nullptr, nullptr, &timeout);
      if (shutdown_requested_.load()) break;
      if (ready <= 0) continue;
      SOCKET client = accept(listener_, nullptr, nullptr);
      if (client == INVALID_SOCKET) {
        ::Sleep(5);
        continue;
      }
      DWORD limit = 120000;
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&limit),
                 sizeof(limit));
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&limit),
                 sizeof(limit));
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(client);
      }
      queue_cv_.notify_one();
    }
  }

  void StopWorkers() {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      stopping_.store(true);
    }
    queue_cv_.notify_all();
    for (size_t i = 0; i < workers_.size(); ++i) {
      if (workers_[i].joinable()) workers_[i].join();
    }
    workers_.clear();
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!queue_.empty()) {
      closesocket(queue_.front());
      queue_.pop();
    }
  }

  void WorkerLoop() {
    for (;;) {
      SOCKET client = INVALID_SOCKET;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this]() { return stopping_.load() || !queue_.empty(); });
        if (queue_.empty()) {
          if (stopping_.load()) return;
          continue;
        }
        client = queue_.front();
        queue_.pop();
      }
      HandleClient(client);
      closesocket(client);
    }
  }

  void HandleClient(SOCKET client) {
    std::string pending;
    char chunk[16384];
    for (;;) {
      int received = recv(client, chunk, sizeof(chunk), 0);
      if (received <= 0) break;
      pending.append(chunk, static_cast<size_t>(received));
      if (pending.size() > kMaxPendingBytes) {
        std::string response = Error("request too large");
        SendAll(client, response.data(), response.size());
        break;
      }
      bool close_after = false;
      if (!ProcessBuffer(client, &pending, &close_after)) break;
      if (close_after) break;
    }
  }

  bool ProcessBuffer(SOCKET client, std::string* pending, bool* close_after) {
    for (;;) {
      size_t position = pending->find("\r\n");
      if (position == std::string::npos) {
        if (pending->size() > kMaxLineBytes) {
          std::string response = Error("line too long");
          SendAll(client, response.data(), response.size());
          return false;
        }
        return true;
      }
      std::string line = pending->substr(0, position);
      std::vector<std::string> tokens;
      SplitSpaces(line, &tokens);
      if (tokens.empty()) {
        pending->erase(0, position + 2);
        continue;
      }

      if (tokens[0] == "PUTB") {
        uint64_t key_length = 0;
        uint64_t value_length = 0;
        if (tokens.size() != 3 || !ParseUnsigned(tokens[1], &key_length) ||
            !ParseUnsigned(tokens[2], &value_length)) {
          pending->erase(0, position + 2);
          std::string response = Error("syntax error");
          if (!SendAll(client, response.data(), response.size())) return false;
          continue;
        }
        if (key_length == 0 || key_length > kMaxBulkBytes || value_length > kMaxBulkBytes) {
          pending->erase(0, position + 2);
          std::string response = Error("invalid bulk length");
          if (!SendAll(client, response.data(), response.size())) return false;
          continue;
        }
        size_t body = position + 2;
        size_t needed = body + static_cast<size_t>(key_length + value_length) + 2;
        if (pending->size() < needed) return true;
        if (pending->compare(needed - 2, 2, "\r\n") != 0) {
          pending->erase(0, needed);
          std::string response = Error("missing bulk terminator");
          if (!SendAll(client, response.data(), response.size())) return false;
          continue;
        }
        std::string key = pending->substr(body, static_cast<size_t>(key_length));
        std::string value = pending->substr(body + key_length, static_cast<size_t>(value_length));
        pending->erase(0, needed);
        int status = db_put(db_, key.data(), key.size(), value.data(), value.size());
        std::string response = status == DB_OK ? "+OK\r\n" : Error("put failed");
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      pending->erase(0, position + 2);
      const std::string& command = tokens[0];

      if (command == "PUT") {
        if (tokens.size() != 3) {
          std::string response = Error("wrong number of arguments");
          if (!SendAll(client, response.data(), response.size())) return false;
          continue;
        }
        int status = db_put(db_, tokens[1].data(), tokens[1].size(), tokens[2].data(),
                            tokens[2].size());
        std::string response = status == DB_OK ? "+OK\r\n" : Error("put failed");
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "GET") {
        if (tokens.size() != 2) {
          std::string response = Error("wrong number of arguments");
          if (!SendAll(client, response.data(), response.size())) return false;
          continue;
        }
        char* value = nullptr;
        size_t length = 0;
        int status = db_get(db_, tokens[1].data(), tokens[1].size(), &value, &length);
        std::string response;
        if (status == DB_OK) {
          response = Bulk(std::string(value, length));
        } else {
          response = "$-1\r\n";
        }
        if (value != nullptr) db_free_value(value);
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "DEL") {
        if (tokens.size() != 2) {
          std::string response = Error("wrong number of arguments");
          if (!SendAll(client, response.data(), response.size())) return false;
          continue;
        }
        int status = db_delete(db_, tokens[1].data(), tokens[1].size());
        std::string response = status == DB_OK ? ":1\r\n" : Error("delete failed");
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "PING") {
        const char* response = "+PONG\r\n";
        if (!SendAll(client, response, std::strlen(response))) return false;
        continue;
      }

      if (command == "STATS") {
        std::string response = Bulk(StatsText());
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "LEVELS") {
        char* text = nullptr;
        size_t length = 0;
        std::string body;
        if (db_levels_text(db_, &text, &length) == DB_OK) {
          body.assign(text, length);
          db_free_value(text);
        }
        std::string response = Bulk(body);
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "CHECK") {
        std::string response = db_verify_levels(db_) == DB_OK ? "+OK\r\n"
                                                              : Error("levels inconsistent");
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "FLUSH") {
        std::string response = db_flush(db_) == DB_OK ? "+OK\r\n" : Error("flush failed");
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "COMPACT") {
        std::string response = db_compact(db_) == DB_OK ? "+OK\r\n" : Error("compaction failed");
        if (!SendAll(client, response.data(), response.size())) return false;
        continue;
      }

      if (command == "SHUTDOWN") {
        const char* response = "+OK\r\n";
        SendAll(client, response, std::strlen(response));
        shutdown_requested_.store(true);
        *close_after = true;
        return true;
      }

      std::string response = Error("unknown command");
      if (!SendAll(client, response.data(), response.size())) return false;
    }
  }

  std::string StatsText() {
    minidb_stats stats;
    std::memset(&stats, 0, sizeof(stats));
    db_get_stats(db_, &stats);
    std::string output;
    output.append("memtable_entries=").append(ToText(stats.memtable_entries)).append("\n");
    output.append("immutable_entries=").append(ToText(stats.immutable_entries)).append("\n");
    output.append("sstable_files=").append(ToText(stats.sstable_count)).append("\n");
    output.append("sstable_entries=").append(ToText(stats.sstable_entries)).append("\n");
    for (int level = 0; level < 4; ++level) {
      output.append("level").append(ToText(static_cast<unsigned long long>(level)))
          .append("_files=").append(ToText(stats.level_files[level])).append("\n");
      output.append("level").append(ToText(static_cast<unsigned long long>(level)))
          .append("_bytes=").append(ToText(stats.level_bytes[level])).append("\n");
    }
    output.append("flush_count=").append(ToText(stats.flush_count)).append("\n");
    output.append("compaction_count=").append(ToText(stats.compaction_count)).append("\n");
    output.append("last_sequence=").append(ToText(stats.last_sequence)).append("\n");
    output.append("puts=").append(ToText(stats.put_count)).append("\n");
    output.append("gets=").append(ToText(stats.get_count)).append("\n");
    output.append("deletes=").append(ToText(stats.delete_count)).append("\n");
    return output;
  }

  Config config_;
  minidb* db_;
  SOCKET listener_;
  std::queue<SOCKET> queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::vector<std::thread> workers_;
  std::atomic<bool> stopping_;
  std::atomic<bool> shutdown_requested_;
  bool wsa_started_ = false;
};

void PrintUsage() {
  std::printf("usage: minidb_server [--data DIR] [--port N] [--threads N] [--memtable-kb N]\n");
  std::printf("                     [--l0-trigger N] [--level-base-kb N] [--target-kb N]\n");
  std::printf("                     [--sync 0|1]\n");
}

bool ParseArguments(int argc, char** argv, Config* config, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--data" && i + 1 < argc) {
      config->data_dir = argv[++i];
    } else if (argument == "--port" && i + 1 < argc) {
      config->port = std::atoi(argv[++i]);
    } else if (argument == "--threads" && i + 1 < argc) {
      config->workers = std::atoi(argv[++i]);
    } else if (argument == "--memtable-kb" && i + 1 < argc) {
      config->memtable_bytes = static_cast<size_t>(std::atoi(argv[++i])) * 1024;
    } else if (argument == "--l0-trigger" && i + 1 < argc) {
      config->l0_trigger = std::atoi(argv[++i]);
    } else if (argument == "--level-base-kb" && i + 1 < argc) {
      config->level_base_bytes = static_cast<size_t>(std::atoi(argv[++i])) * 1024;
    } else if (argument == "--target-kb" && i + 1 < argc) {
      config->target_sstable_bytes = static_cast<size_t>(std::atoi(argv[++i])) * 1024;
    } else if (argument == "--sync" && i + 1 < argc) {
      config->sync_on_write = std::atoi(argv[++i]);
    } else if (argument == "--help") {
      PrintUsage();
      std::exit(0);
    } else {
      *error = "unknown argument: " + argument;
      return false;
    }
  }
  if (config->port <= 0 || config->port > 65535) {
    *error = "invalid port";
    return false;
  }
  if (config->memtable_bytes < 4096) config->memtable_bytes = 4096;
  return true;
}

}

int main(int argc, char** argv) {
  Config config;
  std::string error;
  if (!ParseArguments(argc, argv, &config, &error)) {
    std::printf("[ERROR] %s\n", error.c_str());
    PrintUsage();
    return 2;
  }

  Server server;
  if (!server.Start(config, &error)) {
    std::printf("[ERROR] %s\n", error.c_str());
    return 1;
  }
  std::printf("[INFO] minidb server listening on 0.0.0.0:%d\n", config.port);
  std::printf("[INFO] data=%s workers=%d memtable=%sKB l0_trigger=%d\n", config.data_dir.c_str(),
              config.workers, ToText(config.memtable_bytes / 1024).c_str(), config.l0_trigger);
  std::fflush(stdout);

  server.Run();

  std::printf("[INFO] minidb server stopped\n");
  std::fflush(stdout);
  return 0;
}
