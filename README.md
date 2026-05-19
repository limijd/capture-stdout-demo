# log_capture — stdout/stderr 捕获组件

把进程及其所有 fork 子孙进程的 stdout/stderr 输出原样归并到一个 log 文件，
同时保留 shell 重定向（`1>` / `2>`）行为。基于 dup2 + pipe + reader thread，
无需修改被捕获代码。

详细设计见 `docs/superpowers/specs/2026-05-19-log-capture-productionize-design.md`。
架构原理见 `docs/ARCHITECTURE.md`。

## API

```c
#include "capture.h"

int    capture_start(const char *log_path);
void   capture_stop(void);
size_t capture_io_error_count(void);
```

详见 `capture.h` 内 docstring。

## 集成模板（推荐）

```c
int main(int argc, char **argv) {
    if (capture_start("xxlink.log") < 0) { perror("capture_start"); return 1; }
    atexit(capture_stop);

    /* … 主流程：fork 子进程、dlopen 插件 … */

    wait_all_children();   /* 必须先回收所有 fork child（capture_stop 契约） */
    capture_stop();
    if (capture_io_error_count() > 0)
        fprintf(stderr, "warning: %zu log write errors\n", capture_io_error_count());
    return 0;
}
```

## 契约要点

- `capture_start` 必须在主线程、fork 任何子进程 / dlopen 任何 .so **之前**调用
- `capture_stop` 调用前必须先 `waitpid()` 回收所有 fork child，否则 pthread_join 死锁
- 同进程同时只允许一次 active 捕获；`start → stop → start` 可重入
- `capture_stop` 幂等，可与 `atexit(capture_stop)` 配合做兜底
- 子进程通过 fd 继承自动捕获，不需要额外编码

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
capture.h / capture.c     # 组件本体（vendor 这两个文件进你的仓库即可）
Makefile                  # 自测用
tests/                    # 回归测试
main.c / customer.c       # demo，做 sanity check
docs/ARCHITECTURE.md      # 架构原理（中文）
docs/superpowers/specs/   # design spec 与 review/revise 历史
```

## License

MIT
