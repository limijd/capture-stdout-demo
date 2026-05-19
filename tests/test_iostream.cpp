#include "framework.h"
#include "helpers.h"
#include "../capture.hh"
#include <iostream>
#include <unistd.h>

TEST(A4_cpp_iostream) {
    unlink("/tmp/lc_a4.log");
    ASSERT_EQ(capture_start("/tmp/lc_a4.log"), 0);
    std::cout << "A4_TOKEN_COUT" << std::endl;
    std::cerr << "A4_TOKEN_CERR" << std::endl;
    capture_stop();
    ASSERT(file_contains("/tmp/lc_a4.log", "A4_TOKEN_COUT"));
    ASSERT(file_contains("/tmp/lc_a4.log", "A4_TOKEN_CERR"));
}

extern "C" void run_iostream_tests(void) {
    RUN_TEST(A4_cpp_iostream);
}
