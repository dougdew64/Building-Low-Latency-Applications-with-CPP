#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>

namespace Common {
  /// Fillscope's fork: where the exchange and its clients meet, the book's addresses by default, and
  /// the command-line options that move them. A test runs its exchange and clients on ports of its
  /// own, so it never meets another program on the machine sending or listening on the book's: a
  /// debugger stepping a replay, or another run of the tests.
  struct Stream {
    std::string group;
    int port = 0;
  };

  struct Endpoints {
    int order_port = 12345;
    Stream snapshot{"233.252.14.1", 20000};
    Stream incremental{"233.252.14.3", 20001};
  };

  constexpr const char *kEndpointsUsage = "[--order-port PORT] [--snapshot GROUP:PORT] [--incremental GROUP:PORT]";

  /// A port, 1 to 65535, all of `text` digits.
  inline auto parsePort(const std::string &text) -> std::optional<int> {
    if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) {
      return std::nullopt;
    }
    const long port = std::strtol(text.c_str(), nullptr, 10);
    return port >= 1 && port <= 65535 ? std::optional<int>{static_cast<int>(port)} : std::nullopt;
  }

  /// "GROUP:PORT": an IPv4 multicast address and a port.
  inline auto parseStream(const std::string &text) -> std::optional<Stream> {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
      return std::nullopt;
    }
    const auto group = text.substr(0, colon);
    in_addr address{};
    const auto port = parsePort(text.substr(colon + 1));
    if (!port || ::inet_pton(AF_INET, group.c_str(), &address) != 1 || !IN_MULTICAST(ntohl(address.s_addr))) {
      return std::nullopt;
    }
    return Stream{group, *port};
  }

  /// If argv[i] is --order-port, --snapshot or --incremental, reads it and its value (argv[i + 1])
  /// into `endpoints` and returns 2, the arguments it took; returns -1 if its value is missing or
  /// isn't one, and 0 if argv[i] is some other argument.
  inline auto parseEndpointOption(int argc, char **argv, int i, Endpoints &endpoints) -> int {
    const std::string option = argv[i];
    if (option != "--order-port" && option != "--snapshot" && option != "--incremental") {
      return 0;
    }
    if (i + 1 >= argc) {
      return -1;
    }
    if (option == "--order-port") {
      const auto port = parsePort(argv[i + 1]);
      if (!port) {
        return -1;
      }
      endpoints.order_port = *port;
    } else {
      const auto stream = parseStream(argv[i + 1]);
      if (!stream) {
        return -1;
      }
      (option == "--snapshot" ? endpoints.snapshot : endpoints.incremental) = *stream;
    }
    return 2;
  }
}
