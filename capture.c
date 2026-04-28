#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

// 每个流（stdout/stderr）的捕获状态
typedef struct {
    int saved_fd;      // 保存的原始 fd
    int pipe_rd;       // pipe 读端
    int target_fd;     // 要捕获的 fd (STDOUT_FILENO 或 STDERR_FILENO)
    const char *label; // log 中的前缀标识
    pthread_t thread;
    int thread_started;
    int active;
} stream_capture_t;

static stream_capture_t cap_out;
static stream_capture_t cap_err;
static FILE *log_fp;
static int capture_active;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void reset_cap(stream_capture_t *cap) {
    cap->saved_fd = -1;
    cap->pipe_rd = -1;
    cap->target_fd = -1;
    cap->label = NULL;
    cap->thread_started = 0;
    cap->active = 0;
}

static void write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        off += (size_t)n;
    }
}

// 读取线程：从 pipe 读数据，写入 log 文件 + 原始屏幕
static void *reader_thread(void *arg) {
    stream_capture_t *cap = (stream_capture_t *)arg;
    char buf[4096];
    ssize_t n;

    for (;;) {
        n = read(cap->pipe_rd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        // 写到 log 文件
        pthread_mutex_lock(&log_lock);
        fprintf(log_fp, "[%s] ", cap->label);
        fwrite(buf, 1, (size_t)n, log_fp);
        fflush(log_fp);
        pthread_mutex_unlock(&log_lock);
        // 同时写到真正的屏幕
        write_all(cap->saved_fd, buf, (size_t)n);
    }
    return NULL;
}

// 对单个 fd 启动捕获
static int start_one(stream_capture_t *cap, int target_fd, const char *label) {
    int pipe_fds[2] = {-1, -1};

    cap->target_fd = target_fd;
    cap->label = label;

    // 保存原始 fd
    cap->saved_fd = dup(target_fd);
    if (cap->saved_fd < 0) return -1;

    // 创建 pipe
    if (pipe(pipe_fds) < 0) {
        close(cap->saved_fd);
        reset_cap(cap);
        return -1;
    }
    cap->pipe_rd = pipe_fds[0];

    // 把目标 fd 指向 pipe 写端
    if (dup2(pipe_fds[1], target_fd) < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(cap->saved_fd);
        reset_cap(cap);
        return -1;
    }
    close(pipe_fds[1]);

    // 启动读取线程
    int err = pthread_create(&cap->thread, NULL, reader_thread, cap);
    if (err != 0) {
        errno = err;
        dup2(cap->saved_fd, target_fd);
        close(cap->pipe_rd);
        close(cap->saved_fd);
        reset_cap(cap);
        return -1;
    }
    cap->thread_started = 1;
    cap->active = 1;
    return 0;
}

// 停止单个 fd 的捕获
static void stop_one(stream_capture_t *cap) {
    if (!cap->active) return;

    // 恢复原始 fd
    dup2(cap->saved_fd, cap->target_fd);
    // 目标 fd 恢复会关闭 pipe 写端；等待读取线程 drain 完 pipe 中剩余数据。
    if (cap->thread_started) pthread_join(cap->thread, NULL);
    close(cap->pipe_rd);
    close(cap->saved_fd);
    reset_cap(cap);
}

int capture_start(const char *log_path) {
    if (capture_active) {
        errno = EBUSY;
        return -1;
    }
    if (!log_path) {
        errno = EINVAL;
        return -1;
    }

    reset_cap(&cap_out);
    reset_cap(&cap_err);

    log_fp = fopen(log_path, "a");
    if (!log_fp) return -1;

    // 先 flush，避免缓冲区内容错乱
    fflush(stdout);
    fflush(stderr);

    if (start_one(&cap_out, STDOUT_FILENO, "STDOUT") < 0) {
        fclose(log_fp);
        log_fp = NULL;
        return -1;
    }
    if (start_one(&cap_err, STDERR_FILENO, "STDERR") < 0) {
        stop_one(&cap_out);
        fclose(log_fp);
        log_fp = NULL;
        return -1;
    }

    // fd 1/2 现在指向 pipe（非 tty），libc 默认会把 stdout 切到 block-buffered，
    // 导致 printf 在 fflush 前一直攒在用户态 buffer 里，与 unbuffered 的
    // stderr/write(2) 输出乱序。强制 line-buffered 还原正常顺序。
    // stderr 默认 unbuffered，无需改动。
    setvbuf(stdout, NULL, _IOLBF, 0);

    capture_active = 1;
    return 0;
}

void capture_stop(void) {
    if (!capture_active) return;

    // 先 flush，确保缓冲区数据进入 pipe
    fflush(stdout);
    fflush(stderr);

    stop_one(&cap_out);
    stop_one(&cap_err);

    if (log_fp) fclose(log_fp);
    log_fp = NULL;
    capture_active = 0;
}
