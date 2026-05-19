#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdatomic.h>

// 每个流（stdout/stderr）的捕获状态
typedef struct {
    int saved_fd;      // 保存的原始 fd
    int pipe_rd;       // pipe 读端
    int target_fd;     // 要捕获的 fd (STDOUT_FILENO 或 STDERR_FILENO)
    pthread_t thread;
    int thread_started;
    int active;
} stream_capture_t;

static stream_capture_t cap_out;
static stream_capture_t cap_err;
static FILE *log_fp;
static int capture_active;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_size_t io_err_count;  /* 累计 log 写错误，C11 atomic */
static int atfork_registered;       /* 确保 pthread_atfork 只注册一次 */

static void reset_cap(stream_capture_t *cap);   /* 前向声明，供 atfork handler 用 */

/* fork() 在 POSIX 下整表复制 atexit handler list、整片内存（含 cap_out/cap_err
   的 pthread_t、log_fp、capture_active 标记），但**只复制调用 fork 的那个线程**——
   reader 线程不存在于 child。所以 child 里的 cap.thread 是悬空 pthread_t、
   log_fp 是陈旧 FILE* 缓冲快照。

   如果 child 通过任何方式触发 capture_stop（atexit 继承、客户 .so 显式调、
   错抄模板），后果：
   - pthread_join(悬空 pthread_t) → glibc ESRCH 立即返回（survivable）
   - fclose(log_fp) → child 把陈旧 buffer 写进 log file → **重复/错位字节**污染 log

   防御：fork 后在 child 立刻把组件状态清零，让任何 child 路径的 capture_stop
   变成 no-op。注：故意不 close FILE*——child fclose 就是要避免的污染源；
   kernel 在 child 退出时回收 fd 与 FILE 内存即可。 */
static void capture_atfork_child(void) {
    capture_active = 0;
    log_fp = NULL;          /* 故意丢弃，不 fclose，避免脏缓冲写 log */
    reset_cap(&cap_out);
    reset_cap(&cap_err);
}

static void reset_cap(stream_capture_t *cap) {
    cap->saved_fd = -1;
    cap->pipe_rd = -1;
    cap->target_fd = -1;
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
    char buf[64 * 1024];   /* 栈上：Linux pthread 默认 8MB / macOS 默认 512KB */
    ssize_t n;

    for (;;) {
        n = read(cap->pipe_rd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        /* 1) 写 log（与另一 reader 加锁串行化） */
        pthread_mutex_lock(&log_lock);
        size_t w = fwrite(buf, 1, (size_t)n, log_fp);
        if (w != (size_t)n) {
            int err = errno;
            atomic_fetch_add_explicit(&io_err_count, 1, memory_order_relaxed);
            /* 诊断走 reader 自己的 saved_fd（reader_out → 原 fd 1 目标，
               reader_err → 原 fd 2 目标），不需要全局诊断 fd。
               dprintf 自身失败也无能为力，显式 (void) 标注 best-effort 语义 */
            (void)dprintf(cap->saved_fd,
                "log_capture: fwrite failed: %s\n", strerror(err));
            /* 不 break——继续 drain pipe，否则 writer 死锁 */
        }
        pthread_mutex_unlock(&log_lock);

        /* 2) echo 回原始终端（无锁，两个 reader 各写自己的 saved fd） */
        write_all(cap->saved_fd, buf, (size_t)n);
    }
    return NULL;
}

// 对单个 fd 启动捕获
static int start_one(stream_capture_t *cap, int target_fd) {
    int pipe_fds[2] = {-1, -1};

    cap->target_fd = target_fd;

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

    /* pipe_rd 设 close-on-exec：避免 child exec 后继承组件内部 read 端 fd。
       pipe_w 不设（后续 dup2 到 fd 1/2，需要被 fork+exec 子进程继承）。 */
    {
        int flags = fcntl(pipe_fds[0], F_GETFD);
        if (flags >= 0) (void)fcntl(pipe_fds[0], F_SETFD, flags | FD_CLOEXEC);
    }

#ifdef __linux__
    /* 扩容 pipe 到 1MB 防御 burst；失败静默 fallback 到默认 16-64KB */
    (void)fcntl(pipe_fds[0], F_SETPIPE_SZ, 1 << 20);
#endif

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

    /* 只注册一次：fork 后在 child 把组件状态清零，避免 atexit(capture_stop)
       继承到 child 后 fclose 陈旧 log_fp 缓冲污染 log。pthread_atfork 注册
       是进程级、不能撤销，所以用 atfork_registered 守卫避免多次 start 重复注册 */
    if (!atfork_registered) {
        if (pthread_atfork(NULL, NULL, capture_atfork_child) == 0) {
            atfork_registered = 1;
        }
        /* 注册失败不致命，让 capture 继续；只是失去 child 兜底 */
    }

    log_fp = fopen(log_path, "a");
    if (!log_fp) return -1;

    // 先 flush，避免缓冲区内容错乱
    fflush(stdout);
    fflush(stderr);

    if (start_one(&cap_out, STDOUT_FILENO) < 0) {
        fclose(log_fp);
        log_fp = NULL;
        return -1;
    }
    if (start_one(&cap_err, STDERR_FILENO) < 0) {
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

size_t capture_io_error_count(void) {
    return atomic_load_explicit(&io_err_count, memory_order_relaxed);
}
