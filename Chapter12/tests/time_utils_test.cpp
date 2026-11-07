// The time every log line starts with (common/time_utils.h). The fork changed how it is made, from
// ctime(), whose one buffer the process shares, to localtime_r() into a buffer of the call's own;
// these check the text is what it was: the local time, HH:MM:SS.nnnnnnnnn. That threads no longer
// share a buffer is ThreadSanitizer's to see, which the tests don't run under (the fork's README).
#include <gtest/gtest.h>

#include <ctime>
#include <regex>
#include <string>

#include "common/time_utils.h"

TEST(TimeStr, IsTheLocalTimeToTheNanosecond) {
  std::string text;
  const auto before = std::time(nullptr);
  Common::getCurrentTimeStr(&text);
  const auto after = std::time(nullptr);
  ASSERT_TRUE(std::regex_match(text, std::regex(R"(\d\d:\d\d:\d\d\.\d{9})"))) << text;

  // The seconds are the local time's, as ctime() gave them, at one of the seconds the call spanned.
  const auto hhmmss = [](std::time_t t) {
    std::tm local{};
    localtime_r(&t, &local);
    char text[9];
    std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return std::string(text);
  };
  const auto seconds = text.substr(0, 8);
  EXPECT_TRUE(seconds == hhmmss(before) || seconds == hhmmss(after)) << text;
  EXPECT_EQ(hhmmss(before), std::string(std::ctime(&before) + 11, 8));
}

TEST(TimeStr, IsWrittenIntoTheStringGiven) {
  std::string text = "something much longer than a time, to be replaced whole";
  const auto &returned = Common::getCurrentTimeStr(&text);
  EXPECT_EQ(&returned, &text);
  EXPECT_EQ(text.size(), 18u);
}
