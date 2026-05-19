// stdout/stderr 捕获组件 — C++23 实现（C ABI 接口）
//
// 设计要点：
//   1. extern "C" 暴露 capture_start / capture_stop / capture_io_error_count
//   2. 内部实现用 anonymous namespace + nullptr + std::atomic 等 C++ 特性
//   3. 全局状态**故意保留 POSIX 原语**（pthread_t / pthread_mutex_t / FILE*）
//      而不是 std::thread / std::mutex——避免 fork 之后 child 退出时
//      触发 C++ namespace-scope 析构（std::thread joinable → std::terminate；
//      std::mutex locked → UB）。POSIX 原语没有自动析构，fork 行为干净。
//   4. std::atomic<size_t> 例外（析构 trivial，fork-safe）
//   5. log_fp 用 setvbuf(_IONBF)，防御 child 用 exit() 退出时 libc cleanup
//      把陈旧 stdio buffer 又写一遍污染 log

#include "capture.hh"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace {

struct StreamCapture {
    int saved_fd = -1;
    int pipe_rd = -1;
    int target_fd = -1;
    pthread_t thread{};
    bool thread_started = false;
    bool active = false;

    void reset() {
        saved_fd = -1;
        pipe_rd = -1;
        target_fd = -1;
        thread_started = false;
        active = false;
    }
};

// ===== 全局状态 =====
// 故意全部用 POSIX 原语：fork 后 child 进程退出时不会触发 C++ 自动析构，
// 避免 std::thread / std::mutex 在 child 状态下的析构 UB
StreamCapture cap_out;
StreamCapture cap_err;
FILE* log_fp = nullptr;
bool capture_active = false;
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
// std::atomic 析构 trivial，fork-safe
std::atomic<size_t> io_err_count{0};

void write_all(int fd, const char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        off += static_cast<size_t>(n);
    }
}

// 读取线程：从 pipe 读数据，写入 log 文件 + 原始屏幕
void* reader_thread(void* arg) {
    auto* cap = static_cast<StreamCapture*>(arg);
    char buf[64 * 1024];   // 栈上：Linux pthread 默认 8MB / macOS 默认 512KB

    for (;;) {
        ssize_t n = ::read(cap->pipe_rd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        // 1) 写 log（与另一 reader 加锁串行化）
        pthread_mutex_lock(&log_lock);
        size_t w = std::fwrite(buf, 1, static_cast<size_t>(n), log_fp);
        if (w != static_cast<size_t>(n)) {
            int err = errno;
            io_err_count.fetch_add(1, std::memory_order_relaxed);
            // 诊断走 reader 自己的 saved_fd；显式 (void) 标注 best-effort
            (void)::dprintf(cap->saved_fd,
                "log_capture: fwrite failed: %s\n", std::strerror(err));
            // 不 break——继续 drain pipe，否则 writer 死锁
        }
        pthread_mutex_unlock(&log_lock);

        // 2) echo 回原始终端（无锁，两个 reader 各写自己的 saved fd）
        write_all(cap->saved_fd, buf, static_cast<size_t>(n));
    }
    return nullptr;
}

// 对单个 fd 启动捕获
int start_one(StreamCapture* cap, int target_fd) {
    int pipe_fds[2] = {-1, -1};

    cap->target_fd = target_fd;

    cap->saved_fd = ::dup(target_fd);
    if (cap->saved_fd < 0) return -1;

    if (::pipe(pipe_fds) < 0) {
        ::close(cap->saved_fd);
        cap->reset();
        return -1;
    }
    cap->pipe_rd = pipe_fds[0];

    // pipe_rd 设 close-on-exec：避免 child exec 后继承组件内部 read 端 fd
    // pipe_w 不设（后续 dup2 到 fd 1/2，需要被 fork+exec 子进程继承）
    {
        int flags = ::fcntl(pipe_fds[0], F_GETFD);
        if (flags >= 0) (void)::fcntl(pipe_fds[0], F_SETFD, flags | FD_CLOEXEC);
    }

#ifdef __linux__
    // 扩容 pipe 到 1MB 防御 burst；失败静默 fallback 到默认 16-64KB
    (void)::fcntl(pipe_fds[0], F_SETPIPE_SZ, 1 << 20);
#endif

    // 把目标 fd 指向 pipe 写端
    if (::dup2(pipe_fds[1], target_fd) < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        ::close(cap->saved_fd);
        cap->reset();
        return -1;
    }
    ::close(pipe_fds[1]);

    // 启动读取线程
    int err = pthread_create(&cap->thread, nullptr, reader_thread, cap);
    if (err != 0) {
        errno = err;
        ::dup2(cap->saved_fd, target_fd);
        ::close(cap->pipe_rd);
        ::close(cap->saved_fd);
        cap->reset();
        return -1;
    }
    cap->thread_started = true;
    cap->active = true;
    return 0;
}

// 停止单个 fd 的捕获
//
// 关键顺序：dup2 → join → close(saved_fd)。saved_fd 是 reader 的 echo 目标，
// 必须 join 之后才能 close，否则 reader 写到已关闭 fd 会 EBADF。
void stop_one(StreamCapture* cap) {
    if (!cap->active) return;

    // 恢复原始 fd —— pipe write 端引用归零，reader 的 read() 会返回 0
    ::dup2(cap->saved_fd, cap->target_fd);
    if (cap->thread_started) pthread_join(cap->thread, nullptr);
    ::close(cap->pipe_rd);
    ::close(cap->saved_fd);
    cap->reset();
}

}  // anonymous namespace

// ===== 公开 C ABI 接口 =====

extern "C" int capture_start(const char* log_path) {
    if (capture_active) {
        errno = EBUSY;
        return -1;
    }
    if (!log_path) {
        errno = EINVAL;
        return -1;
    }

    cap_out.reset();
    cap_err.reset();

    log_fp = std::fopen(log_path, "a");
    if (!log_fp) return -1;

    // 关键防御：log_fp 不缓冲。fork 后 child 用 exit() 退出时 libc cleanup
    // 会 fclose 继承的 log_fp，unbuffered 则没有陈旧 buffer 可 flush →
    // 不会污染 log file。reader 的 fwrite 通常 ≥ 1KB chunk（pipe batch
    // read 结果），libc 对 > buffer_size 的 fwrite 本就走 direct write
    // 路径，性能差异不可测。
    std::setvbuf(log_fp, nullptr, _IONBF, 0);

    // 先 flush 旧 stdio buffer，避免内容在 fd 切换之后才写出 → 跑到 pipe 里
    std::fflush(stdout);
    std::fflush(stderr);

    if (start_one(&cap_out, STDOUT_FILENO) < 0) {
        std::fclose(log_fp);
        log_fp = nullptr;
        return -1;
    }
    if (start_one(&cap_err, STDERR_FILENO) < 0) {
        stop_one(&cap_out);
        std::fclose(log_fp);
        log_fp = nullptr;
        return -1;
    }

    // fd 1/2 现在指向 pipe（非 tty），libc 默认会把 stdout 切到 block-buffered，
    // 导致 printf 与 unbuffered stderr / write(2) 输出乱序。强制 line-buffered
    // 还原正常顺序。stderr 默认 unbuffered，无需改动。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    capture_active = true;
    return 0;
}

extern "C" void capture_stop(void) {
    if (!capture_active) return;

    // 先 flush，确保缓冲区数据进入 pipe
    std::fflush(stdout);
    std::fflush(stderr);

    stop_one(&cap_out);
    stop_one(&cap_err);

    if (log_fp) {
        std::fclose(log_fp);
        log_fp = nullptr;
    }
    capture_active = false;
}

extern "C" size_t capture_io_error_count(void) {
    return io_err_count.load(std::memory_order_relaxed);
}
