# The fork's tests: what Fillscope relies on (the market data's layout, the matching, sequence
# numbers and snapshots), and every change made here. GoogleTest, the release Fillscope uses; when
# this is built inside Fillscope, Fillscope has fetched it already.
if(NOT TARGET GTest::gtest)
  include(FetchContent)
  FetchContent_Declare(googletest DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(lowlatency_tests
  test_main.cpp test_support.cpp market_update_test.cpp price_level_table_test.cpp order_book_test.cpp
  market_data_test.cpp replay_test.cpp lockstep_anchor_test.cpp order_server_test.cpp lf_queue_test.cpp
  mcast_socket_test.cpp endpoints_test.cpp)
target_link_libraries(lowlatency_tests PRIVATE libexchange libtrading libcommon pthread GTest::gtest)

# What must not allocate, in a program of its own: it replaces operator new with one that counts.
add_executable(lowlatency_allocation_tests test_main.cpp test_support.cpp allocation_test.cpp)
target_link_libraries(lowlatency_allocation_tests PRIVATE libexchange libcommon pthread GTest::gtest)

# One CTest test runs the whole program. The tests share one matching engine, which takes two
# seconds to make (the book's createAndStartThread sleeps a second after starting each thread, and
# it has two), and the exchange's objects preallocate hundreds of megabytes; a process per test
# would pay both every time.
# Built on its own, this project registers it; inside Fillscope, Fillscope does (its
# tests/backend/CMakeLists.txt), with its tiers and timeouts.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  add_test(NAME lowlatency_tests COMMAND lowlatency_tests)
  add_test(NAME lowlatency_allocation_tests COMMAND lowlatency_allocation_tests)
endif()
