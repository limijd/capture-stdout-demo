#include "framework.h"
#include "helpers.h"
#include "../capture.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>

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
    ssize_t n1 = write(STDOUT_FILENO, m1, strlen(m1));
    ssize_t n2 = write(STDERR_FILENO, m2, strlen(m2));
    ASSERT_EQ(n1, (ssize_t)strlen(m1));
    ASSERT_EQ(n2, (ssize_t)strlen(m2));
    capture_stop();
    ASSERT(file_contains("/tmp/lc_a3.log", "A3_TOKEN_WRITE_OUT"));
    ASSERT(file_contains("/tmp/lc_a3.log", "A3_TOKEN_WRITE_ERR"));
}

/* ========== B: fork / exec / 子孙进程 ========== */

TEST(B1_fork_basic) {
    unlink("/tmp/lc_b1.log");
    ASSERT_EQ(capture_start("/tmp/lc_b1.log"), 0);
    printf("B1_PARENT_BEFORE\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { printf("B1_CHILD_TOKEN\n"); fflush(stdout); _exit(0); }
    ASSERT(pid > 0);
    int st;
    waitpid(pid, &st, 0);
    printf("B1_PARENT_AFTER\n");
    fflush(stdout);
    capture_stop();
    ASSERT(file_contains("/tmp/lc_b1.log", "B1_PARENT_BEFORE"));
    ASSERT(file_contains("/tmp/lc_b1.log", "B1_CHILD_TOKEN"));
    ASSERT(file_contains("/tmp/lc_b1.log", "B1_PARENT_AFTER"));
}

TEST(B2_fork_exec) {
    unlink("/tmp/lc_b2.log");
    ASSERT_EQ(capture_start("/tmp/lc_b2.log"), 0);
    pid_t pid = fork();
    if (pid == 0) {
        execlp("sh", "sh", "-c",
            "echo B2_EXEC_STDOUT; echo B2_EXEC_STDERR >&2", (char *)NULL);
        _exit(127);
    }
    ASSERT(pid > 0);
    int st;
    waitpid(pid, &st, 0);
    capture_stop();
    ASSERT(file_contains("/tmp/lc_b2.log", "B2_EXEC_STDOUT"));
    ASSERT(file_contains("/tmp/lc_b2.log", "B2_EXEC_STDERR"));
}

static void b3_child(int idx) {
    for (int i = 0; i < 1000; i++) {
        printf("B3-C%02d-L%04d\n", idx, i);
    }
    fflush(stdout);
}

TEST(B3_concurrent_children) {
    unlink("/tmp/lc_b3.log");
    ASSERT_EQ(capture_start("/tmp/lc_b3.log"), 0);
    size_t spawned = fork_n_children(16, b3_child);
    ASSERT_EQ(spawned, (size_t)16);
    wait_all_children();
    capture_stop();
    size_t hits = count_unique_tokens("/tmp/lc_b3.log", "B3-C%02d-L%04d", 16, 1000);
    ASSERT_EQ(hits, (size_t)(16 * 1000));
    ASSERT_EQ(capture_io_error_count(), (size_t)0);
}

/* ========== F: 压力 / 持久性 ========== */

TEST(F1_sustained_100k_lines) {
    unlink("/tmp/lc_f1.log");
    ASSERT_EQ(capture_start("/tmp/lc_f1.log"), 0);
    enum { N = 100 * 1000 };
    for (int i = 0; i < N; i++) {
        printf("F1-L%07d\n", i);
    }
    fflush(stdout);
    capture_stop();
    /* 抽样检查首/中/尾 token */
    ASSERT(file_contains("/tmp/lc_f1.log", "F1-L0000000"));
    ASSERT(file_contains("/tmp/lc_f1.log", "F1-L0050000"));
    ASSERT(file_contains("/tmp/lc_f1.log", "F1-L0099999"));
    ASSERT_EQ(capture_io_error_count(), (size_t)0);
}

#ifdef __linux__
static long read_rss_kb(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(fp);
    return rss;
}
#endif

static void f2_child(int idx) {
    time_t end = time(NULL) + 30;
    int i = 0;
    while (time(NULL) < end) {
        printf("F2-C%02d-L%07d\n", idx, i++);
        if ((i & 0xFFF) == 0) fflush(stdout);
    }
    fflush(stdout);
}

TEST(F2_soak_30s_16children) {
    if (!getenv("LOG_CAPTURE_RUN_SOAK")) {
        fprintf(stderr, "  (skipped: set LOG_CAPTURE_RUN_SOAK=1 to run)\n");
        return;
    }
    unlink("/tmp/lc_f2.log");
    ASSERT_EQ(capture_start("/tmp/lc_f2.log"), 0);
#ifdef __linux__
    long rss_before = read_rss_kb();
#endif
    fork_n_children(16, f2_child);
    wait_all_children();
    capture_stop();
#ifdef __linux__
    long rss_after = read_rss_kb();
    if (rss_before > 0 && rss_after > 0) {
        ASSERT((rss_after - rss_before) < 10 * 1024);   /* ≤ 10MB 增长 */
    }
#endif
    ASSERT_EQ(capture_io_error_count(), (size_t)0);
}

/* ========== E: shell 重定向兼容 ========== */

TEST(E1_stderr_redirect_preserved) {
    unlink("/tmp/lc_e1_err_simulated.txt");
    unlink("/tmp/lc_e1.log");
    int file_fd = open("/tmp/lc_e1_err_simulated.txt",
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT(file_fd > 0);
    int saved_stderr_orig = dup(STDERR_FILENO);
    ASSERT(saved_stderr_orig > 0);
    ASSERT(dup2(file_fd, STDERR_FILENO) == STDERR_FILENO);
    close(file_fd);

    ASSERT_EQ(capture_start("/tmp/lc_e1.log"), 0);
    fprintf(stderr, "E1_TOKEN_SHELL_2REDIR\n");
    fflush(stderr);
    capture_stop();

    dup2(saved_stderr_orig, STDERR_FILENO);
    close(saved_stderr_orig);

    ASSERT(file_contains("/tmp/lc_e1.log", "E1_TOKEN_SHELL_2REDIR"));
    ASSERT(file_contains("/tmp/lc_e1_err_simulated.txt",
                         "E1_TOKEN_SHELL_2REDIR"));
}

TEST(E2_stdout_redirect_preserved) {
    unlink("/tmp/lc_e2_out_simulated.txt");
    unlink("/tmp/lc_e2.log");
    int file_fd = open("/tmp/lc_e2_out_simulated.txt",
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT(file_fd > 0);
    int saved_stdout_orig = dup(STDOUT_FILENO);
    ASSERT(saved_stdout_orig > 0);
    ASSERT(dup2(file_fd, STDOUT_FILENO) == STDOUT_FILENO);
    close(file_fd);

    ASSERT_EQ(capture_start("/tmp/lc_e2.log"), 0);
    printf("E2_TOKEN_SHELL_1REDIR\n");
    fflush(stdout);
    capture_stop();

    dup2(saved_stdout_orig, STDOUT_FILENO);
    close(saved_stdout_orig);

    ASSERT(file_contains("/tmp/lc_e2.log", "E2_TOKEN_SHELL_1REDIR"));
    ASSERT(file_contains("/tmp/lc_e2_out_simulated.txt",
                         "E2_TOKEN_SHELL_1REDIR"));
}

/* ========== D: fd / 资源还原 ========== */

TEST(D1_fd1_restored) {
    unsigned long long ino_before, dev_before, ino_after, dev_after;
    get_fd_identity(STDOUT_FILENO, &ino_before, &dev_before);
    unlink("/tmp/lc_d1.log");
    ASSERT_EQ(capture_start("/tmp/lc_d1.log"), 0);
    unsigned long long ino_during, dev_during;
    get_fd_identity(STDOUT_FILENO, &ino_during, &dev_during);
    ASSERT(ino_during != ino_before || dev_during != dev_before);
    capture_stop();
    get_fd_identity(STDOUT_FILENO, &ino_after, &dev_after);
    ASSERT_EQ(ino_after, ino_before);
    ASSERT_EQ(dev_after, dev_before);
}

TEST(D2_fd2_restored) {
    unsigned long long ino_before, dev_before, ino_after, dev_after;
    get_fd_identity(STDERR_FILENO, &ino_before, &dev_before);
    unlink("/tmp/lc_d2.log");
    ASSERT_EQ(capture_start("/tmp/lc_d2.log"), 0);
    capture_stop();
    get_fd_identity(STDERR_FILENO, &ino_after, &dev_after);
    ASSERT_EQ(ino_after, ino_before);
    ASSERT_EQ(dev_after, dev_before);
}

#ifdef __linux__
static int count_proc_self_fd(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') n++;
    }
    closedir(d);
    return n;
}

TEST(D3_no_fd_leak_after_50_cycles) {
    int before = count_proc_self_fd();
    ASSERT(before > 0);
    for (int i = 0; i < 50; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/lc_d3_%02d.log", i);
        unlink(path);
        ASSERT_EQ(capture_start(path), 0);
        capture_stop();
    }
    int after = count_proc_self_fd();
    ASSERT(after <= before + 2);
}
#endif

/* ========== C: 生命周期与 API ========== */

TEST(C1_stop_without_start) {
    capture_stop();
    capture_stop();
    ASSERT(1);
}

TEST(C2_double_start_busy) {
    unlink("/tmp/lc_c2.log");
    ASSERT_EQ(capture_start("/tmp/lc_c2.log"), 0);
    errno = 0;
    int r = capture_start("/tmp/lc_c2_b.log");
    int saved_errno = errno;
    capture_stop();
    ASSERT_EQ(r, -1);
    ASSERT_EQ(saved_errno, EBUSY);
}

TEST(C3_null_path_einval) {
    errno = 0;
    int r = capture_start(NULL);
    ASSERT_EQ(r, -1);
    ASSERT_EQ(errno, EINVAL);
}

TEST(C4_bad_path_returns_error) {
    errno = 0;
    int r = capture_start("/no/such/dir/x.log");
    ASSERT_EQ(r, -1);
    ASSERT(errno != 0);
    /* 没残留：再 capture_start 一个好路径应成功 */
    unlink("/tmp/lc_c4.log");
    ASSERT_EQ(capture_start("/tmp/lc_c4.log"), 0);
    capture_stop();
}

TEST(C5_start_stop_re_entry) {
    unlink("/tmp/lc_c5a.log");
    unlink("/tmp/lc_c5b.log");
    ASSERT_EQ(capture_start("/tmp/lc_c5a.log"), 0);
    printf("C5_ROUND_1\n");
    capture_stop();
    ASSERT(file_contains("/tmp/lc_c5a.log", "C5_ROUND_1"));
    ASSERT_EQ(capture_start("/tmp/lc_c5b.log"), 0);
    printf("C5_ROUND_2\n");
    capture_stop();
    ASSERT(file_contains("/tmp/lc_c5b.log", "C5_ROUND_2"));
}

TEST(C6_stop_idempotent) {
    unlink("/tmp/lc_c6.log");
    ASSERT_EQ(capture_start("/tmp/lc_c6.log"), 0);
    printf("C6_TOKEN\n");
    capture_stop();
    capture_stop();
    capture_stop();
    ASSERT(file_contains("/tmp/lc_c6.log", "C6_TOKEN"));
}

/* C8: 验证 child 用 exit() 退出时 libc cleanup 不污染 log。
   核心防御：setvbuf(log_fp, _IONBF) 让 buffer 永远空，child 的 fclose
   没有陈旧数据可 flush → 不重复写 parent 已经写过的内容。
   适用场景：dlopen 的 customer .so 内部 fork 后用 exit() 而非 _exit()。 */
TEST(C8_child_exit_does_not_pollute_log) {
    unlink("/tmp/lc_c8.log");
    ASSERT_EQ(capture_start("/tmp/lc_c8.log"), 0);
    /* parent 先写一行，让 reader 把它 fwrite 进 log_fp */
    printf("C8_PARENT_UNIQUE_LINE\n");
    fflush(stdout);
    usleep(50 * 1000);   /* 给 reader 50ms 抓 + 写 log */

    pid_t pid = fork();
    if (pid == 0) {
        printf("C8_CHILD_LINE\n");
        fflush(stdout);
        exit(0);   /* 故意用 exit (非 _exit) 触发 libc cleanup */
    }
    ASSERT(pid > 0);
    int st;
    waitpid(pid, &st, 0);
    capture_stop();

    /* parent 的行不应被 child 的 fclose libc cleanup 重复写一遍 */
    size_t parent_line_count = file_count_occurrences("/tmp/lc_c8.log",
                                                     "C8_PARENT_UNIQUE_LINE");
    ASSERT_EQ(parent_line_count, (size_t)1);
    /* child 自己的输出仍正常进 log（通过 pipe → reader → log_fp） */
    ASSERT(file_contains("/tmp/lc_c8.log", "C8_CHILD_LINE"));
}

TEST(C7_io_error_count_anytime) {
    /* start 前 */
    size_t before = capture_io_error_count();
    unlink("/tmp/lc_c7.log");
    ASSERT_EQ(capture_start("/tmp/lc_c7.log"), 0);
    /* start 中 */
    printf("C7_TOKEN\n");
    /* 期望 happy path 不增加 */
    capture_stop();
    /* stop 后 */
    size_t after = capture_io_error_count();
    ASSERT_EQ(after, before);
}


TEST(B4_grandchild) {
    unlink("/tmp/lc_b4.log");
    ASSERT_EQ(capture_start("/tmp/lc_b4.log"), 0);
    pid_t pid = fork();
    if (pid == 0) {
        pid_t gpid = fork();
        if (gpid == 0) {
            printf("B4_GRANDCHILD_TOKEN\n");
            fflush(stdout);
            _exit(0);
        }
        int gst; waitpid(gpid, &gst, 0);
        _exit(0);
    }
    ASSERT(pid > 0);
    int st; waitpid(pid, &st, 0);
    capture_stop();
    ASSERT(file_contains("/tmp/lc_b4.log", "B4_GRANDCHILD_TOKEN"));
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
    ssize_t n = write(STDOUT_FILENO, big, SZ);
    ASSERT_EQ(n, (ssize_t)SZ);
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

    RUN_TEST(B1_fork_basic);
    RUN_TEST(B2_fork_exec);
    RUN_TEST(B3_concurrent_children);
    RUN_TEST(B4_grandchild);

    RUN_TEST(C1_stop_without_start);
    RUN_TEST(C2_double_start_busy);
    RUN_TEST(C3_null_path_einval);
    RUN_TEST(C4_bad_path_returns_error);
    RUN_TEST(C5_start_stop_re_entry);
    RUN_TEST(C6_stop_idempotent);
    RUN_TEST(C7_io_error_count_anytime);
    RUN_TEST(C8_child_exit_does_not_pollute_log);

    RUN_TEST(D1_fd1_restored);
    RUN_TEST(D2_fd2_restored);
#ifdef __linux__
    RUN_TEST(D3_no_fd_leak_after_50_cycles);
#endif

    RUN_TEST(E1_stderr_redirect_preserved);
    RUN_TEST(E2_stdout_redirect_preserved);

    RUN_TEST(F1_sustained_100k_lines);
    RUN_TEST(F2_soak_30s_16children);

    fprintf(stderr, "\nTotal: %d passed, %d failed\n",
            g_test_pass, g_test_fail);
    return g_test_fail > 0 ? 1 : 0;
}
