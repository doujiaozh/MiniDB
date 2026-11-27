#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "net_client.h"

namespace {

std::string ToText(long long value) {
  char buffer[32];
  int length = 0;
  bool negative = value < 0;
  unsigned long long magnitude = negative ? static_cast<unsigned long long>(-value)
                                          : static_cast<unsigned long long>(value);
  do {
    buffer[length++] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude > 0);
  std::string result;
  if (negative) result.push_back('-');
  while (length > 0) result.push_back(buffer[--length]);
  return result;
}

bool Execute(net::Connection& connection, const std::string& line) {
  if (!connection.SendLine(line)) {
    std::printf("send failed\n");
    return false;
  }
  net::Response response;
  if (!connection.ReadResponse(&response)) {
    std::printf("read failed\n");
    return false;
  }
  if (response.type == '$') {
    if (response.number < 0) {
      std::printf("(nil)\n");
    } else {
      if (!response.text.empty()) std::fwrite(response.text.data(), 1, response.text.size(), stdout);
      std::printf("\n");
    }
    return true;
  }
  if (response.type == ':') {
    std::printf("%s\n", ToText(response.number).c_str());
    return true;
  }
  if (response.type == '+') {
    std::printf("%s\n", response.text.c_str());
    return true;
  }
  if (response.type == '-') {
    std::printf("ERR %s\n", response.text.c_str());
    return false;
  }
  std::printf("%s\n", response.text.c_str());
  return false;
}

void PrintUsage() {
  std::printf("usage: minidb_cli [-h host] [-p port] [command ...]\n");
  std::printf("commands: PUT key value | GET key | DEL key | PING | STATS | LEVELS\n");
  std::printf("          | CHECK | FLUSH | COMPACT | SHUTDOWN\n");
}

}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 6379;
  std::vector<std::string> command;

  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if ((argument == "-h" || argument == "--host") && i + 1 < argc) {
      host = argv[++i];
    } else if ((argument == "-p" || argument == "--port") && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (argument == "--help") {
      PrintUsage();
      return 0;
    } else {
      command.push_back(argument);
    }
  }

  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    std::printf("WSAStartup failed\n");
    return 1;
  }

  net::Connection connection;
  if (!connection.Connect(host, port, 5000)) {
    std::printf("cannot connect to %s:%d\n", host.c_str(), port);
    WSACleanup();
    return 1;
  }

  int result = 0;
  if (!command.empty()) {
    std::string line;
    for (size_t i = 0; i < command.size(); ++i) {
      if (i != 0) line.push_back(' ');
      line.append(command[i]);
    }
    result = Execute(connection, line) ? 0 : 1;
    connection.Close();
    WSACleanup();
    return result;
  }

  char buffer[65536];
  for (;;) {
    std::printf("minidb> ");
    std::fflush(stdout);
    if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) break;
    std::string line(buffer);
    while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r')) {
      line.erase(line.size() - 1);
    }
    if (line.empty()) continue;
    if (line == "quit" || line == "exit") break;
    if (!Execute(connection, line)) result = 1;
  }

  connection.Close();
  WSACleanup();
  return result;
}
