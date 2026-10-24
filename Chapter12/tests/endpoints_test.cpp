// Where the exchange and its clients meet (common/endpoints.h): the book's addresses by default, and
// the options that move them, which exchange_main and trading_main both read. That the programs use
// them is checked by Fillscope's system tests, which run them on ports of their own.
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "common/endpoints.h"
#include "test_support.h"

using Common::Endpoints;

namespace {
  // Reads `args` as a program's command line from argv[1], as the programs do; the result of each
  // call, and the endpoints read.
  auto read(std::vector<std::string> args) -> std::tuple<std::vector<int>, Endpoints> {
    args.insert(args.begin(), "program");
    std::vector<char *> argv;
    for (auto &arg : args) {
      argv.push_back(arg.data());
    }
    Endpoints endpoints;
    std::vector<int> took;
    for (int i = 1; i < static_cast<int>(argv.size());) {
      const int n = Common::parseEndpointOption(static_cast<int>(argv.size()), argv.data(), i, endpoints);
      took.push_back(n);
      i += n > 0 ? n : 1;
    }
    return {took, endpoints};
  }

  auto fields(const Endpoints &e) {
    return std::make_tuple(e.order_port, e.snapshot.group, e.snapshot.port, e.incremental.group, e.incremental.port);
  }
}

// The tests' own ports (test_support.h): this process holds its slot's file locked, so no other
// process can claim the slot while it runs. A second lock on the file, from another open of it, is
// refused.
TEST(TestPort, ThisProcessHoldsItsSlot) {
  const int port = LowLatencyTests::testPort(0);
  ASSERT_EQ((port - 22000) % 10, 0);
  const int slot = (port - 22000) / 10;
  ASSERT_GE(slot, 0);
  ASSERT_LT(slot, 1000);
  const auto path = std::filesystem::temp_directory_path() / "fillscope-test-ports" / ("slot-" + std::to_string(slot));
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0) << path;
  EXPECT_NE(::flock(fd, LOCK_EX | LOCK_NB), 0) << "slot " << slot << " could be claimed again";
  ::close(fd);
}

TEST(Endpoints, AreTheBooksByDefault) {
  EXPECT_EQ(fields(Endpoints{}), std::make_tuple(12345, std::string("233.252.14.1"), 20000, std::string("233.252.14.3"), 20001));
}

TEST(Endpoints, EachOptionMovesItsOwn) {
  const auto [took, endpoints] = read({"--order-port", "22005", "--snapshot", "233.252.20.1:22001",
                                       "--incremental", "233.252.21.1:22002"});
  EXPECT_EQ(took, (std::vector<int>{2, 2, 2}));
  EXPECT_EQ(fields(endpoints), std::make_tuple(22005, std::string("233.252.20.1"), 22001, std::string("233.252.21.1"), 22002));
}

TEST(Endpoints, OtherArgumentsArentTaken) {
  const auto [took, endpoints] = read({"5", "RANDOM", "--record"});
  EXPECT_EQ(took, (std::vector<int>{0, 0, 0}));
  EXPECT_EQ(fields(endpoints), fields(Endpoints{}));
}

TEST(Endpoints, RefusesAValueThatIsntOne) {
  for (const std::vector<std::string> &args : {
           std::vector<std::string>{"--order-port"},  // no value
           {"--order-port", "0"},
           {"--order-port", "65536"},
           {"--order-port", "-5"},
           {"--order-port", "+5"},
           {"--order-port", " 5"},
           {"--order-port", "5x"},
           {"--order-port", "99999999999999999999"},
           {"--snapshot", "233.252.20.1"},       // no port
           {"--snapshot", "233.252.20.1:"},
           {"--snapshot", "233.252.20.1:0"},
           {"--incremental", "10.0.0.1:22001"},  // not a multicast group
           {"--incremental", "not.an.address:22001"},
           {"--incremental", ":22001"},
       }) {
    const auto [took, endpoints] = read(args);
    EXPECT_EQ(took.front(), -1) << args.front() << " " << (args.size() > 1 ? args[1] : "(none)");
  }
}
