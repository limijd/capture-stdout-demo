#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

// 每个流（stdout/stderr）的捕获状态
typedef struct {
    int saved_fd;      // 保存的原始 fd
    int pipe_rd;       // pipe 读端
    int target_fd;     // 要捕获的 fd (STDOUT_FILENO 或 STDERR_FILENO)
    const char *label; // log 中的前缀标识
    pthread_t thread;
} stream_capture_t;

static stream_capture_t cap_out;
static stream_capture_t cap_err;
static FILE *log_fp;

// 读取线程：从 pipe 读数据，写入 log 文件 + 原始屏幕
static void *reader_thread(void *arg) {
    stream_capture_t *cap = (stream_capture_t *)arg;
    char buf[4096];
    ssize_t n;

    while ((n = read(cap->pipe_rd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        // 写到 log 文件
        fprintf(log_fp, "[%s] %s", cap->label, buf);
        fflush(log_fp);
        // 同时写到真正的屏幕
        write(cap->saved_fd, buf, n);
    }
    return NULL;
}

// 对单个 fd 启动捕获
static int start_one(stream_capture_t *cap, int target_fd, const char *label) {
    int pipe_fds[2];
    cap->target_fd = target_fd;
    cap->label = label;

    // 保存原始 fd
    cap->saved_fd = dup(target_fd);
    if (cap->saved_fd < 0) return -1;

    // 创建 pipe
    if (pipe(pipe_fds) < 0) return -1;
    cap->pipe_rd = pipe_fds[0];

    // 把目标 fd 指向 pipe 写端
    dup2(pipe_fds[1], target_fd);
    close(pipe_fds[1]);

    // 启动读取线程
    pthread_create(&cap->thread, NULL, reader_thread, cap);
    return 0;
}

// 停止单个 fd 的捕获
static void stop_one(stream_capture_t *cap) {
    // 恢复原始 fd
    dup2(cap->saved_fd, cap->target_fd);
    close(cap->saved_fd);
    // 关闭 pipe 读端，读取线程会因 read 返回 0 而退出
    close(cap->pipe_rd);
    pthread_join(cap->thread, NULL);
}

int capture_start(const char *log_path) {
    log_fp = fopen(log_path, "a");
    if (!log_fp) return -1;

    // 先 flush，避免缓冲区内容错乱
    fflush(stdout);
    fflush(stderr);

    if (start_one(&cap_out, STDOUT_FILENO, "STDOUT") < 0) return -1;
    if (start_one(&cap_err, STDERR_FILENO, "STDERR") < 0) return -1;
    return 0;
}

void capture_stop(void) {
    // 先 flush，确保缓冲区数据进入 pipe
    fflush(stdout);
    fflush(stderr);

    stop_one(&cap_out);
    stop_one(&cap_err);

    fclose(log_fp);
    log_fp = NULL;
}
