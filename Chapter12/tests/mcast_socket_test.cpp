// Multicast stays on the interface a socket is given (common/socket_utils.h, mcast_socket.h). The
// exchange and the trading client name the loopback interface, "lo"; the book's sockets ignored the
// name, so the exchange's market data left on the interface the system routes the groups to (on a
// machine with Wi-Fi, onto the local network), and listeners joined there too.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "common/mcast_socket.h"
#include "test_support.h"

namespace {
  constexpr const char *kGroup = "233.252.15.9";
  constexpr int kPort = 21005;

  auto logger() -> Common::Logger & {
    static auto *const instance = new Common::Logger("tests_mcast_socket.log");
    return *instance;
  }

  // The interface a socket's multicast leaves on.
  auto multicastInterface(int fd) -> std::string {
    in_addr address{};
    socklen_t size = sizeof(address);
    if (getsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &address, &size) != 0) {
      return "getsockopt failed: " + std::string(std::strerror(errno));
    }
    return inet_ntoa(address);
  }

  // Sends one datagram to the group, leaving on the interface with address `iface_ip`.
  auto sendFrom(const char *iface_ip, const std::string &payload) -> bool {
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const in_addr iface{inet_addr(iface_ip)};
    sockaddr_in group{AF_INET, htons(kPort), {inet_addr(kGroup)}, {}};
    const bool sent = fd >= 0 && setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) == 0 &&
                      sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&group), sizeof(group)) ==
                          static_cast<ssize_t>(payload.size());
    close(fd);
    return sent;
  }
}

// Before the fix, its multicast left on the default interface: IP_MULTICAST_IF read 0.0.0.0.
TEST(McastSocket, ASenderSendsOnTheInterfaceItIsGiven) {
  Common::McastSocket sender(logger());
  ASSERT_GE(sender.init(kGroup, "lo", kPort, /*is_listening*/ false), 0);
  EXPECT_EQ(multicastInterface(sender.socket_fd_), "127.0.0.1");
  close(sender.socket_fd_);
}

// Before the fix, it joined on the default interface, and what was sent on loopback never arrived.
TEST(McastSocket, AListenerJoinsOnTheInterfaceItIsGiven) {
  Common::McastSocket listener(logger());
  ASSERT_GE(listener.init(kGroup, "lo", kPort, /*is_listening*/ true), 0);
  ASSERT_TRUE(listener.join(kGroup));
  std::string received;
  listener.recv_callback_ = [&](Common::McastSocket *socket) {
    received.assign(socket->inbound_data_.data(), socket->next_rcv_valid_index_);
    socket->next_rcv_valid_index_ = 0;
  };
  ASSERT_TRUE(sendFrom("127.0.0.1", "on loopback"));
  for (int i = 0; i < 100 && received.empty(); ++i) {
    listener.sendAndRecv();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  EXPECT_EQ(received, "on loopback");
  close(listener.socket_fd_);
}

TEST(McastSocket, AListenerCantJoinOnAnInterfaceThatDoesntExist) {
  Common::McastSocket listener(logger());
  ASSERT_GE(listener.init(kGroup, "no-such-interface", kPort, /*is_listening*/ true), 0);
  EXPECT_FALSE(listener.join(kGroup));
  close(listener.socket_fd_);
}

// A sender given an interface it can't send on stops the program as it starts, rather than sending
// somewhere else. It exits, so it runs in a child (runInChild), which exits 0 only if it survives.
TEST(McastSocket, ASenderOnAnInterfaceThatDoesntExistStopsTheProgram) {
  if (std::getenv("LOWLATENCY_NO_SUCH_INTERFACE")) {
    Common::McastSocket sender(logger());
    sender.init(kGroup, "no-such-interface", kPort, /*is_listening*/ false);
    ::_exit(0);
  }
  const int status = LowLatencyTests::runInChild("McastSocket.ASenderOnAnInterfaceThatDoesntExistStopsTheProgram",
                                                 "LOWLATENCY_NO_SUCH_INTERFACE=1");
  ASSERT_NE(status, -1) << "the child couldn't be started";
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), EXIT_FAILURE) << "a sender on an interface that doesn't exist was made";
}
