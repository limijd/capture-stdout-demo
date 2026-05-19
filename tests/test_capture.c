#include "framework.h"
#include "helpers.h"
#include "../capture.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int g_test_pass = 0;
int g_test_fail = 0;
const char *g_current_test = NULL;

extern void run_iostream_tests(void);

/* ========== A: 输出路径完整性 ========== */

TEST(A1_printf) {
    unlink("/tmp/lc_a1.log");
    ASSERT_EQ(capture_start("/tmp/lc_a1.log"), 0);
    printf("A1_TOKEN_PRINTF\n");
    capture_stop();
    ASSERT(file_contains("/tmp/lc_a1.log", "A1_TOKEN_PRINTF"));
    ASSERT_EQ(capture_io_error_count(), (size_t)0);
}

TEST(A2_fprintf) {
    unlink("/tmp/lc_a2.log");
    ASSERT_EQ(capture_start("/tmp/lc_a2.log"), 0);
    fprintf(stdout, "A2_TOKEN_FPRINTF_OUT\n");
    fprintf(stderr, "A2_TOKEN_FPRINTF_ERR\n");
    capture_stop();
    ASSERT(file_contains("/tmp/lc_a2.log", "A2_TOKEN_FPRINTF_OUT"));
    ASSERT(file_contains("/tmp/lc_a2.log", "A2_TOKEN_FPRINTF_ERR"));
}

TEST(A3_write) {
    unlink("/tmp/lc_a3.log");
    ASSERT_EQ(capture_start("/tmp/lc_a3.log"), 0);
    const char *m1 = "A3_TOKEN_WRITE_OUT\n";
    const char *m2 = "A3_TOKEN_WRITE_ERR\n";
    (void)!write(STDOUT_FILENO, m1, strlen(m1));
    (void)!write(STDERR_FILENO, m2, strlen(m2));
    capture_stop();
    ASSERT(file_contains("/tmp/lc_a3.log", "A3_TOKEN_WRITE_OUT"));
    ASSERT(file_contains("/tmp/lc_a3.log", "A3_TOKEN_WRITE_ERR"));
}

TEST(A5_big_write) {
    unlink("/tmp/lc_a5.log");
    ASSERT_EQ(capture_start("/tmp/lc_a5.log"), 0);
    /* 单次 write > PIPE_BUF (4096) 且 < 1MB */
    enum { SZ = 16 * 1024 };
    static char big[SZ + 64];
    memset(big, 'X', SZ);
    memcpy(big, "A5_TOKEN_BIG_HEAD", strlen("A5_TOKEN_BIG_HEAD"));
    memcpy(big + SZ - strlen("A5_TOKEN_BIG_TAIL\n"),
           "A5_TOKEN_BIG_TAIL\n", strlen("A5_TOKEN_BIG_TAIL\n"));
    (void)!write(STDOUT_FILENO, big, SZ);
    capture_stop();
    ASSERT(file_contains("/tmp/lc_a5.log", "A5_TOKEN_BIG_HEAD"));
    ASSERT(file_contains("/tmp/lc_a5.log", "A5_TOKEN_BIG_TAIL"));
}

int main(void) {
    RUN_TEST(A1_printf);
    RUN_TEST(A2_fprintf);
    RUN_TEST(A3_write);
    run_iostream_tests();   /* A4 (C++ iostream) */
    RUN_TEST(A5_big_write);

    fprintf(stderr, "\nTotal: %d passed, %d failed\n",
            g_test_pass, g_test_fail);
    return g_test_fail > 0 ? 1 : 0;
}
