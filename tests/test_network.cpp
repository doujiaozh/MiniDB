#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_client.h"

namespace {

struct Config {
  std::string host;
  int port;
  int clients;
  int operations;
  int keys;

  Config() : host("127.0.0.1"), port(6379), clients(16), operations(10000), keys(2000) {}
};

struct ClientResult {
  int failures;
  int puts;
  int gets;
  int deletes;
  std::map<std::string, std::string> model;

  ClientResult() : failures(0), puts(0), gets(0), deletes(0) {}
};

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

std::string Value(int index, int client, int operation) {
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "value-%08d-c%02d-o%05d", index, client, operation);
  return std::string(buffer);
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

void RunClient(const Config& config, int index, int operations, ClientResult* result) {
  net::Connection connection;
  if (!connection.Connect(config.host, config.port, 5000)) {
    ++result->failures;
    return;
  }
  int keys_per_client = config.keys / config.clients;
  if (keys_per_client < 1) keys_per_client = 1;
  int first = index * keys_per_client;
  std::mt19937 rng(static_cast<unsigned int>(1000 + index));

  for (int operation = 0; operation < operations; ++operation) {
    int key_index = first + static_cast<int>(rng() % static_cast<unsigned int>(keys_per_client));
    std::string key = Key(key_index);
    unsigned int action = rng() % 100u;

    if (action < 70) {
      std::string value = Value(key_index, index, operation);
      if (!connection.SendLine("PUT " + key + " " + value)) {
        ++result->failures;
        break;
      }
      net::Response response;
      if (!connection.ReadResponse(&response) || response.type != '+') {
        ++result->failures;
        break;
      }
      result->model[key] = value;
      ++result->puts;
    } else if (action < 90) {
      if (!connection.SendLine("GET " + key)) {
        ++result->failures;
        break;
      }
      net::Response response;
      if (!connection.ReadResponse(&response)) {
        ++result->failures;
        break;
      }
      std::map<std::string, std::string>::const_iterator entry = result->model.find(key);
      if (entry == result->model.end()) {
        if (response.type != '$' || response.number != -1) ++result->failures;
      } else if (response.type != '$' || response.text != entry->second) {
        ++result->failures;
      }
      ++result->gets;
    } else {
      if (!connection.SendLine("DEL " + key)) {
        ++result->failures;
        break;
      }
      net::Response response;
      if (!connection.ReadResponse(&response) || response.type != ':') {
        ++result->failures;
        break;
      }
      result->model.erase(key);
      ++result->deletes;
    }
  }
  connection.Close();
}

bool WaitForServer(const Config& config) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    net::Connection probe;
    if (probe.Connect(config.host, config.port, 500)) {
      if (probe.SendLine("PING")) {
        net::Response response;
        if (probe.ReadResponse(&response) && response.type == '+' && response.text == "PONG") {
          probe.Close();
          return true;
        }
      }
      probe.Close();
    }
    ::Sleep(50);
  }
  return false;
}

bool ParseArguments(int argc, char** argv, Config* config, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--host" && i + 1 < argc) {
      config->host = argv[++i];
    } else if (argument == "--port" && i + 1 < argc) {
      config->port = std::atoi(argv[++i]);
    } else if (argument == "--clients" && i + 1 < argc) {
      config->clients = std::atoi(argv[++i]);
    } else if (argument == "--ops" && i + 1 < argc) {
      config->operations = std::atoi(argv[++i]);
    } else if (argument == "--keys" && i + 1 < argc) {
      config->keys = std::atoi(argv[++i]);
    } else {
      *error = "unknown argument: " + argument;
      return false;
    }
  }
  if (config->clients < 1 || config->operations < 1 || config->keys < config->clients) {
    *error = "invalid client, operation or key count";
    return false;
  }
  return true;
}

}

int main(int argc, char** argv) {
  Config config;
  std::string error;
  if (!ParseArguments(argc, argv, &config, &error)) {
    std::printf("[ERROR] %s\n", error.c_str());
    return 2;
  }

  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    std::printf("[FAIL] WSAStartup failed\n");
    return 1;
  }
  if (!WaitForServer(config)) {
    std::printf("[FAIL] server is not reachable on %s:%d\n", config.host.c_str(), config.port);
    WSACleanup();
    return 1;
  }

  std::printf("[STAGE] %d concurrent clients, %d mixed operations, %d keys\n", config.clients,
              config.operations, config.keys);

  int operations_per_client = config.operations / config.clients;
  if (operations_per_client < 1) operations_per_client = 1;
  int total_operations = operations_per_client * config.clients;

  std::vector<ClientResult> results(static_cast<size_t>(config.clients));
  std::vector<std::thread> threads;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  for (int i = 0; i < config.clients; ++i) {
    threads.push_back(std::thread(RunClient, config, i, operations_per_client,
                                  &results[static_cast<size_t>(i)]));
  }
  for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  int failures = 0;
  int puts = 0;
  int gets = 0;
  int deletes = 0;
  std::map<std::string, std::string> model;
  for (size_t i = 0; i < results.size(); ++i) {
    failures += results[i].failures;
    puts += results[i].puts;
    gets += results[i].gets;
    deletes += results[i].deletes;
    for (std::map<std::string, std::string>::const_iterator entry = results[i].model.begin();
         entry != results[i].model.end(); ++entry) {
      model[entry->first] = entry->second;
    }
  }

  net::Connection connection;
  if (!connection.Connect(config.host, config.port, 5000)) {
    std::printf("[FAIL] cannot open verification connection\n");
    WSACleanup();
    return 1;
  }

  int mismatches = 0;
  int missing = 0;
  for (int i = 0; i < config.keys; ++i) {
    std::string key = Key(i);
    if (!connection.SendLine("GET " + key)) {
      ++mismatches;
      break;
    }
    net::Response response;
    if (!connection.ReadResponse(&response)) {
      ++mismatches;
      break;
    }
    std::map<std::string, std::string>::const_iterator entry = model.find(key);
    if (entry == model.end()) {
      if (response.type != '$' || response.number != -1) ++mismatches;
      else ++missing;
    } else if (response.type != '$' || response.text != entry->second) {
      ++mismatches;
    }
  }

  net::Connection probe;
  probe.Connect(config.host, config.port, 5000);
  int protocol_failures = 0;

  probe.SendLine("GET missing-key-for-network-test");
  net::Response nil_response;
  if (!probe.ReadResponse(&nil_response) || !nil_response.IsNil()) ++protocol_failures;

  probe.SendLine("BOGUS command");
  net::Response error_response;
  if (!probe.ReadResponse(&error_response) || error_response.type != '-') ++protocol_failures;

  probe.SendLine("GET");
  net::Response arity_response;
  if (!probe.ReadResponse(&arity_response) || arity_response.type != '-') ++protocol_failures;

  probe.SendLine("PING");
  net::Response ping_response;
  if (!probe.ReadResponse(&ping_response) || ping_response.text != "PONG") ++protocol_failures;

  probe.SendLine("STATS");
  net::Response stats_response;
  if (!probe.ReadResponse(&stats_response) ||
      stats_response.text.find("level0_files=") == std::string::npos ||
      stats_response.text.find("compaction_count=") == std::string::npos) {
    ++protocol_failures;
  }

  probe.SendLine("CHECK");
  net::Response check_response;
  if (!probe.ReadResponse(&check_response) || check_response.type != '+') ++protocol_failures;

  std::string level_summary;
  size_t compaction_position = stats_response.text.find("compaction_count=");
  std::string compaction_text = "-";
  if (compaction_position != std::string::npos) {
    size_t end = stats_response.text.find('\n', compaction_position);
    compaction_text = stats_response.text.substr(
        compaction_position + std::strlen("compaction_count="),
        (end == std::string::npos ? stats_response.text.size() : end) -
            (compaction_position + std::strlen("compaction_count=")));
  }
  for (int level = 0; level < 4; ++level) {
    std::string token = "level" + ToText(static_cast<unsigned long long>(level)) + "_files=";
    size_t position = stats_response.text.find(token);
    std::string value = "0";
    if (position != std::string::npos) {
      size_t end = stats_response.text.find('\n', position);
      value = stats_response.text.substr(position + token.size(),
                                         (end == std::string::npos ? stats_response.text.size() : end) -
                                             (position + token.size()));
    }
    level_summary.append("L");
    level_summary.append(ToText(static_cast<unsigned long long>(level)));
    level_summary.append("=");
    level_summary.append(value);
    level_summary.append(" ");
  }
  std::printf("[INFO] server levels: %scompaction=%s\n", level_summary.c_str(),
              compaction_text.c_str());

  probe.SendLine("PUT sentinel-key sentinel-value-20261127");
  net::Response sentinel_response;
  if (!probe.ReadResponse(&sentinel_response) || sentinel_response.type != '+') {
    ++protocol_failures;
  }

  probe.SendLine("LEVELS");
  net::Response levels_response;
  if (!probe.ReadResponse(&levels_response)) {
    ++protocol_failures;
  } else {
    std::vector<std::string> lines;
    size_t position = 0;
    while (position < levels_response.text.size()) {
      size_t end = levels_response.text.find('\n', position);
      if (end == std::string::npos) end = levels_response.text.size();
      if (end > position) lines.push_back(levels_response.text.substr(position, end - position));
      position = end + 1;
    }
    if (lines.empty()) {
      std::printf("[INFO] no sstable yet, level ordering trivially valid\n");
    }
    std::string previous_largest;
    bool have_previous = false;
    for (size_t i = 0; i < lines.size(); ++i) {
      std::vector<std::string> tokens;
      SplitSpaces(lines[i], &tokens);
      if (tokens.size() != 6 || tokens[0] != "LEVEL") {
        ++protocol_failures;
        break;
      }
      if (tokens[1] != "0") {
        if (have_previous && tokens[4] <= previous_largest) ++protocol_failures;
        previous_largest = tokens[5];
        have_previous = true;
      }
    }
  }

  std::string binary_key = "binary-key";
  std::string binary_value;
  binary_value.push_back('\0');
  binary_value.push_back('\001');
  binary_value.push_back('\r');
  binary_value.push_back('\n');
  binary_value.push_back(static_cast<char>(0xff));
  std::string binary_header =
      "PUTB " + ToText(binary_key.size()) + " " + ToText(binary_value.size()) + "\r\n";
  bool binary_ok = probe.SendRaw(binary_header.data(), binary_header.size()) &&
                   probe.SendRaw(binary_key.data(), binary_key.size()) &&
                   probe.SendRaw(binary_value.data(), binary_value.size()) &&
                   probe.SendRaw("\r\n", 2);
  net::Response binary_put;
  if (!binary_ok || !probe.ReadResponse(&binary_put) || binary_put.type != '+') {
    ++protocol_failures;
  }
  probe.SendLine("GET " + binary_key);
  net::Response binary_get;
  if (!probe.ReadResponse(&binary_get) || binary_get.type != '$' ||
      binary_get.text != binary_value) {
    ++protocol_failures;
  }

  std::printf("[INFO] writes: put=%d get=%d delete=%d elapsed=%.3fs throughput=%.0f ops/s\n",
              puts, gets, deletes, elapsed,
              static_cast<double>(total_operations) / elapsed);
  std::printf("[INFO] network verify: keys=%d live=%d absent=%d mismatches=%d\n", config.keys,
              static_cast<int>(model.size()), missing, mismatches);
  std::printf("[INFO] protocol checks failures=%d\n", protocol_failures);

  probe.SendLine("SHUTDOWN");
  net::Response shutdown_response;
  if (!probe.ReadResponse(&shutdown_response) || shutdown_response.type != '+') {
    ++protocol_failures;
  }
  probe.Close();
  connection.Close();
  WSACleanup();

  int result = 0;
  if (failures != 0) {
    std::printf("[FAIL] client operation failures=%d\n", failures);
    result = 1;
  }
  if (mismatches != 0) {
    std::printf("[FAIL] network data mismatches=%d\n", mismatches);
    result = 1;
  }
  if (protocol_failures != 0) {
    std::printf("[FAIL] protocol validation failures=%d\n", protocol_failures);
    result = 1;
  }
  if (result == 0) {
    std::printf("[RESULT] network path consistent: %d mixed operations, zero errors\n",
                total_operations);
  }
  return result;
}
