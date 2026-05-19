# log_capture — stdout/stderr 捕获组件

把进程及其所有 fork 子孙进程的 stdout/stderr 输出原样归并到一个 log 文件，
同时保留 shell 重定向（`1>` / `2>`）行为。基于 dup2 + pipe + reader thread，
无需修改被捕获代码。

**实现语言：C++17/23（capture.cpp / capture.hh），但所有公开符号走 `extern "C"` 暴露 C ABI——C 代码可以无缝 `#include "capture.hh"` 并链接 `capture.o`，链接时用 `g++` 作 driver 拉入 C++ runtime（见 Makefile）。**

详细设计见 `docs/superpowers/specs/2026-05-19-log-capture-productionize-design.md`。
架构原理见 `docs/ARCHITECTURE.md`。

## API

```c
#include "capture.hh"

int    capture_start(const char *log_path);
void   capture_stop(void);
size_t capture_io_error_count(void);
```

详见 `capture.hh` 内 docstring。

## 集成模板（推荐）

```c
#include <sys/wait.h>
#include "capture.hh"

int main(int argc, char **argv) {
    if (capture_start("xxlink.log") < 0) { perror("capture_start"); return 1; }

    /* … 主流程：fork 子进程、dlopen 插件 … */

    /* 必须先回收所有 fork child，否则 capture_stop 的 pthread_join 死锁。
       组件不提供这个 helper——`waitpid` 是 POSIX，等子进程本来就是调用方
       自己的职责，xxlink 可按需扩展（按类型分类、记录退出码等）。 */
    while (waitpid(-1, NULL, 0) > 0) { /* drain */ }

    capture_stop();
    if (capture_io_error_count() > 0)
        fprintf(stderr, "warning: %zu log write errors\n", capture_io_error_count());
    return 0;
}
```

## 契约要点

- `capture_start` 必须在主线程、fork 任何子进程 / dlopen 任何 .so **之前**调用
- `capture_stop` 调用前必须先 `waitpid()` 回收所有 fork child，否则 pthread_join 死锁；最简写法：`while (waitpid(-1, NULL, 0) > 0) {}`
- `capture_stop` **只能在主进程主线程调用**——child 进程不应调（cap.thread 悬空 → pthread_join UB）
- **child 进程退出方式**：推荐 `_exit()`；如果某些代码路径无法保证（dlopen 的 customer .so / 第三方库），用 `exit()` 也不会污染 log——组件已用 `setvbuf(log_fp, _IONBF)` 防御 child libc cleanup 的脏 buffer 写
- 同进程同时只允许一次 active 捕获；`start → stop → start` 可重入
- `capture_stop` 幂等（多次调安全）
- 子进程通过 fd 继承自动捕获，不需要额外编码

## 生产部署 — Hang 风险与缓解

### 唯一已知 hang 风险

**`capture_stop()` 在还有 fork child 存活时调用** → `pthread_join` 永久阻塞。

机制：reader 的 `read()` 要返回 0 必须等 pipe write 端引用全部归零。`capture_stop` 只关闭父进程那一份；只要任何 child 还活着，它继承的 fd 1/fd 2 仍指向 pipe write 端，refcount > 0 → reader 永远不退出 → join 卡死。

进程将不响应 SIGTERM，只能 SIGKILL。

### 不会 hang 的路径（已防御）

| 场景 | 行为 |
|------|------|
| log 磁盘满 / EIO / 短写 | `io_err_count++` + 诊断到 stderr，**继续 drain pipe**（reader 不退出，writer 不阻塞） |
| log 在 NFS 慢 | 影响吞吐不死锁；最差 gcc 编译被节流 |
| 子进程自己 dup2 / freopen | child 自己的 fd 表修改，不影响父 pipe_w refcount |
| 多 child 同时 burst > pipe buffer | child write() 暂时阻塞 → reader 持续 drain → 自动恢复 |
| `capture_start` 部分失败 | 已 `stop_one` 回滚已建立的 cap_out，无线程残留 |
| 同一 test 内 ASSERT 失败 early-return | `RUN_TEST` 宏末尾无条件 `capture_stop()` 兜底（review F2） |
| reader `dprintf` 诊断写失败 | 返回值显式 `(void)`，reader 继续主循环 |

### 部署建议（按优先级递减）

**MUST**：

1. **xxlink 主循环结束、`capture_stop()` 之前必须 `waitpid()` 回收所有 child**（最简：`while (waitpid(-1, NULL, 0) > 0) {}`）。如果用 job pool / 异步队列 fork gcc，确保 pool 已完全 drain 再 stop。这是组件外的纪律——**没有任何代码层强制约束**，只有契约
2. **child 进程不要调 `capture_stop`**（cap.thread 悬空 → pthread_join UB）。child 推荐用 `_exit()` 退出；若 dlopen 的 customer .so 内部 fork 后用了 `exit()`，组件已用 `setvbuf(log_fp, _IONBF)` 防御 libc cleanup 时 fclose 陈旧 buffer 污染 log——你不必为不可控的 child 代码担心 log 数据完整性
3. **log 文件放本地 SSD**，避免 NFS / 网络盘上的慢写传染到 gcc 编译速度

**SHOULD**：

4. 监控体系里把 xxlink 的 SIGTERM 不响应当作告警信号——pthread_join 卡死时进程必须 SIGKILL 才能退出

**不建议**：

5. **不要** `atexit(capture_stop)`。理由：atexit 会被 fork 整表继承到 child，child 退出时跑这个 handler 会污染 log file（fclose 陈旧 FILE* 缓冲）。正常路径显式 `capture_stop()` 已够，异常 `exit()` 路径丢的几行 trailing 输出价值不大
6. 不要在信号处理函数里调 `capture_stop`（`pthread_join` / `fflush` / `fclose` 非 async-signal-safe，会 deadlock 或 UB）

### 一句话总结

只要 xxlink 主进程**在 `capture_stop` 前确实 wait 完所有 fork 出的子进程**，这个组件在生产里不会 hang。其它已知 backpressure / IO 错误路径都做过非阻塞处理。

## 平台支持

- **Linux**：完整支持，自动 `F_SETPIPE_SZ` 扩容 pipe 到 1MB
- **macOS / Darwin**：完整支持，默认 pipe 大小（16KB）足够低-中等并发 workload
- **其它 POSIX**：未交付承诺，理论可移植

编译时需 `-pthread`。

## Build & Test

```bash
make all       # 编译 demo（main.c + customer.so，作为 sanity check）
make run       # 跑 demo，看 output.log
make test      # 跑回归测试（macOS 22 个，Linux 23 个）
make clean
```

Soak 测试默认跳过，需要时：
```bash
LOG_CAPTURE_RUN_SOAK=1 ./test_capture
```

## 文件清单

```
capture.hh / capture.cpp  # 组件本体（vendor 这两个文件 + Makefile 片段到你的仓库）
Makefile                  # 自测用 + 演示 g++ 编译/链接方式
tests/                    # 回归测试（23 个，含 C 端 + C++ iostream 单元）
main.c / customer.c       # demo（C 调用者验证 C ABI），做 sanity check
docs/ARCHITECTURE.md      # 架构原理（中文）
docs/superpowers/specs/   # design spec 与 review/revise 历史
```

### vendor 到 xxlink 仓库时的 build 片段

```makefile
# xxlink 的 Makefile 里加上：
xxlink_capture.o: third_party/log_capture/capture.cpp third_party/log_capture/capture.hh
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -Ithird_party/log_capture -c -o $@ $<

# 链接 xxlink 时用 $(CXX) 作 driver（即使你的 xxlink 主体是 C）：
xxlink: <你的 .c 文件> xxlink_capture.o
	$(CXX) ... -o $@ -x c <你的 .c 文件> -x none xxlink_capture.o -lpthread
```

## License

MIT
