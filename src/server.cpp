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
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "minidb.h"
#include "resp.h"

namespace {

const size_t kMaxPendingBytes = resp::kMaxPendingBytes;
const size_t kMaxQueuedCommands = 1024u * 1024u;
const size_t kMaxLegacyLineBytes = 1024u * 1024u;

bool ParseUnsigned(const std::string& text, uint64_t* value) {
  if (text.empty()) return false;
  uint64_t result = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c < '0' || c > '9') return false;
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (result > (0xffffffffffffffffULL - digit) / 10) return false;
    result = result * 10 + digit;
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

enum LegacyStatus {
  kLegacyNone,
  kLegacyComplete,
  kLegacyIncomplete,
  kLegacyError
};

LegacyStatus ParseLegacyPutb(const std::string& buffer, std::vector<std::string>* args,
                             size_t* consumed, std::string* error) {
  if (buffer.size() < 5) {
    if (std::string("PUTB ").compare(0, buffer.size(), buffer) == 0) return kLegacyIncomplete;
    return kLegacyNone;
  }
  if (buffer.compare(0, 5, "PUTB ") != 0) return kLegacyNone;
  size_t line_end = buffer.find("\r\n");
  if (line_end == std::string::npos) {
    if (buffer.size() > kMaxLegacyLineBytes) {
      *error = "line too long";
      return kLegacyError;
    }
    return kLegacyIncomplete;
  }
  std::vector<std::string> tokens;
  SplitSpaces(buffer.substr(0, line_end), &tokens);
  uint64_t key_length = 0;
  uint64_t value_length = 0;
  if (tokens.size() != 3 || !ParseUnsigned(tokens[1], &key_length) ||
      !ParseUnsigned(tokens[2], &value_length)) {
    *error = "syntax error";
    return kLegacyError;
  }
  if (key_length == 0 || key_length > resp::kMaxBulkBytes ||
      value_length > resp::kMaxBulkBytes) {
    *error = "invalid bulk length";
    return kLegacyError;
  }
  size_t body = line_end + 2;
  size_t key_size = static_cast<size_t>(key_length);
  size_t value_size = static_cast<size_t>(value_length);
  size_t needed = body + key_size + value_size + 2;
  if (buffer.size() < needed) return kLegacyIncomplete;
  if (buffer.compare(body + key_size + value_size, 2, "\r\n") != 0) {
    *error = "missing bulk terminator";
    return kLegacyError;
  }
  args->clear();
  args->push_back("SET");
  args->push_back(buffer.substr(body, key_size));
  args->push_back(buffer.substr(body + key_size, value_size));
  *consumed = needed;
  return kLegacyComplete;
}

class Server {
 public:
  Server()
      : db_(nullptr),
        listener_(INVALID_SOCKET),
        stopping_(false),
        shutdown_requested_(false),
        next_client_id_(0),
        connected_clients_(0) {}

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
      if (error != nullptr) *error = "cannot bind port " + resp::ToText(
                                                    static_cast<unsigned long long>(config_.port));
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
  struct ClientState {
    ClientState() : in_multi(false), multi_error(false), id(0) {}

    bool in_multi;
    bool multi_error;
    std::vector<std::vector<std::string>> queue;
    std::string name;
    unsigned long long id;
  };

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
    ClientState state;
    state.id = next_client_id_.fetch_add(1) + 1;
    connected_clients_.fetch_add(1);
    std::string pending;
    char chunk[16384];
    for (;;) {
      int received = recv(client, chunk, sizeof(chunk), 0);
      if (received <= 0) break;
      pending.append(chunk, static_cast<size_t>(received));
      if (pending.size() > kMaxPendingBytes) {
        std::string response = resp::Error("request too large");
        SendAll(client, response.data(), response.size());
        break;
      }
      bool close_after = false;
      if (!ProcessBuffer(client, &pending, &state, &close_after)) break;
      if (close_after) break;
    }
    connected_clients_.fetch_sub(1);
  }

  bool ProcessBuffer(SOCKET client, std::string* pending, ClientState* state,
                     bool* close_after) {
    for (;;) {
      if (pending->empty()) return true;
      std::vector<std::string> args;
      size_t consumed = 0;
      std::string parse_error;
      LegacyStatus legacy = ParseLegacyPutb(*pending, &args, &consumed, &parse_error);
      if (legacy == kLegacyError) {
        std::string response = resp::Error(parse_error);
        SendAll(client, response.data(), response.size());
        return false;
      }
      if (legacy == kLegacyIncomplete) return true;
      if (legacy == kLegacyNone) {
        resp::ParseStatus status = resp::ParseRequest(*pending, &args, &consumed, &parse_error);
        if (status == resp::kIncomplete) return true;
        if (status == resp::kError) {
          std::string response = resp::Error("Protocol error: " + parse_error);
          SendAll(client, response.data(), response.size());
          return false;
        }
      }
      pending->erase(0, consumed);
      if (args.empty()) continue;

      std::string response;
      bool request_close = false;
      ExecuteCommand(args, state, &response, &request_close);
      if (!response.empty() && !SendAll(client, response.data(), response.size())) return false;
      if (request_close) {
        *close_after = true;
        return true;
      }
    }
  }

  static bool IsWriteCommand(const std::string& name) {
    return name == "SET" || name == "PUT" || name == "DEL" || name == "MSET" ||
           name == "FLUSH" || name == "COMPACT";
  }

  static bool ValidateCommand(const std::vector<std::string>& args, std::string* error) {
    const std::string name = resp::ToUpper(args[0]);
    const size_t argc = args.size();
    bool valid = false;
    if (name == "PING") {
      valid = argc == 1 || argc == 2;
    } else if (name == "ECHO") {
      valid = argc == 2;
    } else if (name == "SET") {
      valid = argc >= 3;
    } else if (name == "PUT") {
      valid = argc == 3;
    } else if (name == "GET" || name == "TYPE") {
      valid = argc == 2;
    } else if (name == "DEL" || name == "EXISTS" || name == "MGET") {
      valid = argc >= 2;
    } else if (name == "MSET") {
      valid = argc >= 3 && (argc % 2) == 1;
    } else if (name == "MULTI" || name == "EXEC" || name == "DISCARD" || name == "QUIT" ||
               name == "SHUTDOWN" || name == "UNWATCH" || name == "FLUSH" ||
               name == "COMPACT" || name == "CHECK" || name == "STATS" ||
               name == "LEVELS") {
      valid = argc == 1;
    } else if (name == "WATCH") {
      valid = argc >= 2;
    } else if (name == "CLIENT") {
      valid = argc >= 2;
    } else if (name == "COMMAND") {
      valid = true;
    } else if (name == "INFO") {
      valid = argc <= 2;
    } else if (name == "SELECT") {
      valid = argc == 2;
    } else if (name == "AUTH") {
      valid = argc >= 2;
    } else {
      *error = "unknown command '" + resp::Printable(resp::ToLower(args[0]), 64) + "'";
      return false;
    }
    if (!valid) {
      *error = "wrong number of arguments for '" +
               resp::Printable(resp::ToLower(args[0]), 64) + "' command";
      return false;
    }
    return true;
  }

  void ExecuteCommand(const std::vector<std::string>& args, ClientState* state,
                      std::string* response, bool* close_after) {
    *close_after = false;
    const std::string name = resp::ToUpper(args[0]);

    if (state->in_multi) {
      std::string validation_error;
      if (!ValidateCommand(args, &validation_error)) {
        state->multi_error = true;
        *response = resp::Error(validation_error);
        return;
      }
      ExecuteQueued(args, name, state, response);
      return;
    }

    std::string validation_error;
    if (!ValidateCommand(args, &validation_error)) {
      *response = resp::Error(validation_error);
      return;
    }

    if (name == "MULTI") {
      state->in_multi = true;
      state->multi_error = false;
      state->queue.clear();
      *response = resp::Simple("OK");
      return;
    }
    if (name == "EXEC") {
      *response = resp::Error("EXEC without MULTI");
      return;
    }
    if (name == "DISCARD") {
      *response = resp::Error("DISCARD without MULTI");
      return;
    }
    if (name == "WATCH" || name == "UNWATCH") {
      *response = resp::Error("WATCH is not supported");
      return;
    }

    if (IsWriteCommand(name)) {
      std::unique_lock<std::shared_mutex> lock(gate_);
      *response = Dispatch(args, state, close_after);
    } else {
      std::shared_lock<std::shared_mutex> lock(gate_);
      *response = Dispatch(args, state, close_after);
    }
  }

  void ExecuteQueued(const std::vector<std::string>& args, const std::string& name,
                     ClientState* state, std::string* response) {
    if (name == "MULTI") {
      *response = resp::Error("MULTI calls can not be nested");
      return;
    }
    if (name == "EXEC") {
      RunTransaction(state, response);
      return;
    }
    if (name == "DISCARD") {
      state->in_multi = false;
      state->multi_error = false;
      state->queue.clear();
      *response = resp::Simple("OK");
      return;
    }
    if (name == "WATCH" || name == "UNWATCH") {
      state->multi_error = true;
      *response = resp::Error("WATCH is not supported");
      return;
    }
    if (name == "SHUTDOWN" || name == "QUIT") {
      state->multi_error = true;
      *response = resp::Error("command is not allowed in transactions");
      return;
    }
    if (state->queue.size() >= kMaxQueuedCommands) {
      state->multi_error = true;
      *response = resp::Error("transaction queue overflow");
      return;
    }
    state->queue.push_back(args);
    *response = resp::Simple("QUEUED");
  }

  void RunTransaction(ClientState* state, std::string* response) {
    const bool dirty = state->multi_error;
    std::vector<std::vector<std::string>> queue;
    queue.swap(state->queue);
    state->in_multi = false;
    state->multi_error = false;
    if (dirty) {
      *response = resp::PrefixedError("EXECABORT",
                                      "Transaction discarded because of previous errors.");
      return;
    }
    std::unique_lock<std::shared_mutex> lock(gate_);
    std::string output = resp::ArrayHeader(queue.size());
    for (size_t i = 0; i < queue.size(); ++i) {
      bool close_after = false;
      output.append(Dispatch(queue[i], state, &close_after));
    }
    *response = output;
  }

  bool Lookup(const std::string& key, std::string* value) {
    char* raw = nullptr;
    size_t length = 0;
    int status = db_get(db_, key.data(), key.size(), &raw, &length);
    if (status == DB_OK) {
      value->assign(raw, length);
      db_free_value(raw);
      return true;
    }
    if (raw != nullptr) db_free_value(raw);
    return false;
  }

  std::string CommandSet(const std::vector<std::string>& args) {
    if (args[1].empty()) return resp::Error("key must not be empty");
    bool nx = false;
    bool xx = false;
    bool get = false;
    for (size_t i = 3; i < args.size(); ++i) {
      const std::string option = resp::ToUpper(args[i]);
      if (option == "NX") {
        nx = true;
      } else if (option == "XX") {
        xx = true;
      } else if (option == "GET") {
        get = true;
      } else {
        return resp::Error("syntax error");
      }
    }
    if (nx && xx) return resp::Error("syntax error");
    std::string previous;
    const bool existed = Lookup(args[1], &previous);
    const std::string previous_reply = existed ? resp::Bulk(previous) : resp::NullBulk();
    if ((nx && existed) || (xx && !existed)) return get ? previous_reply : resp::NullBulk();
    if (db_put(db_, args[1].data(), args[1].size(), args[2].data(), args[2].size()) != DB_OK) {
      return resp::Error("set failed");
    }
    return get ? previous_reply : resp::Simple("OK");
  }

  std::string CommandGet(const std::vector<std::string>& args) {
    std::string value;
    if (!Lookup(args[1], &value)) return resp::NullBulk();
    return resp::Bulk(value);
  }

  std::string CommandDel(const std::vector<std::string>& args) {
    long long removed = 0;
    for (size_t i = 1; i < args.size(); ++i) {
      std::string ignored;
      if (!Lookup(args[i], &ignored)) continue;
      if (db_delete(db_, args[i].data(), args[i].size()) != DB_OK) {
        return resp::Error("del failed");
      }
      ++removed;
    }
    return resp::Integer(removed);
  }

  std::string CommandExists(const std::vector<std::string>& args) {
    long long count = 0;
    for (size_t i = 1; i < args.size(); ++i) {
      std::string ignored;
      if (Lookup(args[i], &ignored)) ++count;
    }
    return resp::Integer(count);
  }

  std::string CommandMget(const std::vector<std::string>& args) {
    std::string output = resp::ArrayHeader(args.size() - 1);
    for (size_t i = 1; i < args.size(); ++i) {
      std::string value;
      if (Lookup(args[i], &value)) {
        output.append(resp::Bulk(value));
      } else {
        output.append(resp::NullBulk());
      }
    }
    return output;
  }

  std::string CommandMset(const std::vector<std::string>& args) {
    for (size_t i = 1; i < args.size(); i += 2) {
      if (args[i].empty()) return resp::Error("key must not be empty");
    }
    for (size_t i = 1; i < args.size(); i += 2) {
      if (db_put(db_, args[i].data(), args[i].size(), args[i + 1].data(),
                 args[i + 1].size()) != DB_OK) {
        return resp::Error("mset failed");
      }
    }
    return resp::Simple("OK");
  }

  std::string CommandType(const std::vector<std::string>& args) {
    std::string ignored;
    return resp::Simple(Lookup(args[1], &ignored) ? "string" : "none");
  }

  std::string CommandClient(const std::vector<std::string>& args, ClientState* state) {
    const std::string subcommand = resp::ToUpper(args[1]);
    if (subcommand == "SETINFO") {
      return resp::Simple("OK");
    }
    if (subcommand == "SETNAME") {
      if (args.size() != 3) {
        return resp::Error("wrong number of arguments for 'client|setname' command");
      }
      state->name = args[2];
      return resp::Simple("OK");
    }
    if (subcommand == "GETNAME") {
      if (args.size() != 2) {
        return resp::Error("wrong number of arguments for 'client|getname' command");
      }
      return resp::Bulk(state->name);
    }
    if (subcommand == "ID") {
      if (args.size() != 2) {
        return resp::Error("wrong number of arguments for 'client|id' command");
      }
      return resp::Integer(static_cast<long long>(state->id));
    }
    if (subcommand == "INFO") {
      if (args.size() != 2) {
        return resp::Error("wrong number of arguments for 'client|info' command");
      }
      std::string info = "id=" + resp::ToText(state->id);
      info.append(" name=").append(state->name);
      return resp::Bulk(info);
    }
    return resp::Error("unknown subcommand '" + resp::Printable(resp::ToLower(args[1]), 64) +
                       "'");
  }

  std::string CommandStats() {
    return resp::Bulk(StatsText());
  }

  std::string CommandLevels() {
    char* text = nullptr;
    size_t length = 0;
    std::string body;
    if (db_levels_text(db_, &text, &length) == DB_OK) {
      body.assign(text, length);
      db_free_value(text);
    }
    return resp::Bulk(body);
  }

  std::string CommandInfo() {
    std::string text;
    text.append("# Server\r\n");
    text.append("redis_version:6.2.0\r\n");
    text.append("redis_mode:standalone\r\n");
    text.append("minidb_version:0.3.0\r\n");
    text.append("# Clients\r\n");
    text.append("connected_clients:").append(
        resp::ToText(static_cast<unsigned long long>(connected_clients_.load()))).append("\r\n");
    text.append("# Replication\r\n");
    text.append("role:master\r\n");
    text.append("connected_slaves:0\r\n");
    text.append("# Cluster\r\n");
    text.append("cluster_enabled:0\r\n");
    return resp::Bulk(text);
  }

  std::string Dispatch(const std::vector<std::string>& args, ClientState* state,
                       bool* close_after) {
    const std::string name = resp::ToUpper(args[0]);
    if (name == "PING") {
      if (args.size() == 1) return resp::Simple("PONG");
      return resp::Bulk(args[1]);
    }
    if (name == "ECHO") return resp::Bulk(args[1]);
    if (name == "SET") return CommandSet(args);
    if (name == "PUT") {
      std::vector<std::string> mapped;
      mapped.push_back("SET");
      mapped.push_back(args[1]);
      mapped.push_back(args[2]);
      return CommandSet(mapped);
    }
    if (name == "GET") return CommandGet(args);
    if (name == "DEL") return CommandDel(args);
    if (name == "EXISTS") return CommandExists(args);
    if (name == "MGET") return CommandMget(args);
    if (name == "MSET") return CommandMset(args);
    if (name == "TYPE") return CommandType(args);
    if (name == "CLIENT") return CommandClient(args, state);
    if (name == "COMMAND") {
      if (args.size() == 2 && resp::ToUpper(args[1]) == "COUNT") return resp::Integer(0);
      return resp::ArrayHeader(0);
    }
    if (name == "INFO") return CommandInfo();
    if (name == "SELECT") {
      long long index = 0;
      if (!resp::ParseSigned(args[1], &index)) return resp::Error("invalid DB index");
      if (index != 0) return resp::Error("DB index is out of range");
      return resp::Simple("OK");
    }
    if (name == "AUTH") {
      return resp::Error("Client sent AUTH, but no password is set");
    }
    if (name == "STATS") return CommandStats();
    if (name == "LEVELS") return CommandLevels();
    if (name == "CHECK") {
      return db_verify_levels(db_) == DB_OK ? resp::Simple("OK")
                                            : resp::Error("levels inconsistent");
    }
    if (name == "FLUSH") {
      return db_flush(db_) == DB_OK ? resp::Simple("OK") : resp::Error("flush failed");
    }
    if (name == "COMPACT") {
      return db_compact(db_) == DB_OK ? resp::Simple("OK") : resp::Error("compaction failed");
    }
    if (name == "QUIT") {
      *close_after = true;
      return resp::Simple("OK");
    }
    if (name == "SHUTDOWN") {
      shutdown_requested_.store(true);
      *close_after = true;
      return resp::Simple("OK");
    }
    return resp::Error("unknown command '" + resp::Printable(resp::ToLower(args[0]), 64) + "'");
  }

  std::string StatsText() {
    minidb_stats stats;
    std::memset(&stats, 0, sizeof(stats));
    db_get_stats(db_, &stats);
    std::string output;
    output.append("memtable_entries=").append(resp::ToText(stats.memtable_entries)).append("\n");
    output.append("immutable_entries=").append(resp::ToText(stats.immutable_entries)).append("\n");
    output.append("sstable_files=").append(resp::ToText(stats.sstable_count)).append("\n");
    output.append("sstable_entries=").append(resp::ToText(stats.sstable_entries)).append("\n");
    for (int level = 0; level < 4; ++level) {
      output.append("level").append(resp::ToText(static_cast<unsigned long long>(level)))
          .append("_files=").append(resp::ToText(stats.level_files[level])).append("\n");
      output.append("level").append(resp::ToText(static_cast<unsigned long long>(level)))
          .append("_bytes=").append(resp::ToText(stats.level_bytes[level])).append("\n");
    }
    output.append("flush_count=").append(resp::ToText(stats.flush_count)).append("\n");
    output.append("compaction_count=").append(resp::ToText(stats.compaction_count)).append("\n");
    output.append("last_sequence=").append(resp::ToText(stats.last_sequence)).append("\n");
    output.append("puts=").append(resp::ToText(stats.put_count)).append("\n");
    output.append("gets=").append(resp::ToText(stats.get_count)).append("\n");
    output.append("deletes=").append(resp::ToText(stats.delete_count)).append("\n");
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
  std::atomic<unsigned long long> next_client_id_;
  std::atomic<int> connected_clients_;
  std::shared_mutex gate_;
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
              config.workers, resp::ToText(config.memtable_bytes / 1024).c_str(),
              config.l0_trigger);
  std::fflush(stdout);

  server.Run();

  std::printf("[INFO] minidb server stopped\n");
  std::fflush(stdout);
  return 0;
}
