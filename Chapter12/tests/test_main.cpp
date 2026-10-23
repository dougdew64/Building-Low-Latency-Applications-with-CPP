// The tests' main: they run in a directory of their own, since the exchange's objects write their
// logs into the working directory, and it is removed afterwards.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

// Read by LeakSanitizer, when it is linked in (the sanitizer build): leaks it doesn't report. The
// book's createAndStartThread() allocates each std::thread with new, and the components' start()
// functions drop the pointer, so those threads can never be joined; a known issue of the book's
// design (the README's section on this fork), not something these tests can avoid.
extern "C" const char *__lsan_default_suppressions() {
  return "leak:createAndStartThread\n";
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  std::string directory = (std::filesystem::temp_directory_path() / "lowlatency-tests-XXXXXX").string();
  if (mkdtemp(directory.data()) == nullptr || chdir(directory.c_str()) != 0) {
    std::cerr << "lowlatency_tests: can't make and enter a directory for the logs in " << directory << '\n';
    return 1;
  }
  const int result = RUN_ALL_TESTS();
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  return result;
}
