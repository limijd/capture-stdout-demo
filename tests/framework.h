#pragma once
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_test_pass;
extern int g_test_fail;
extern const char *g_current_test;

/* 应用 review F2: RUN_TEST 在 test 函数返回后无条件调 capture_stop，
   即使 test 因 ASSERT 失败 early-return，也不会留下 capture_active=1
   导致下个 test 的 capture_start 报 EBUSY 级联失败。
   capture_stop 文档化为幂等，未启动状态调用是 no-op。 */
void capture_stop(void);

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do {                                       \
    g_current_test = #name;                                       \
    int before = g_test_fail;                                     \
    fprintf(stderr, "[ RUN  ] %s\n", #name);                      \
    test_##name();                                                \
    capture_stop();   /* teardown：保证状态隔离 */                  \
    if (g_test_fail == before) {                                  \
        g_test_pass++;                                            \
        fprintf(stderr, "[  OK  ] %s\n", #name);                  \
    } else fprintf(stderr, "[ FAIL ] %s\n", #name);               \
} while (0)

#define ASSERT(cond) do {                                         \
    if (!(cond)) { g_test_fail++;                                 \
        fprintf(stderr, "  ASSERT failed: %s @ %s:%d\n",          \
                #cond, __FILE__, __LINE__); return; }             \
} while (0)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))

#ifdef __cplusplus
}
#endif
