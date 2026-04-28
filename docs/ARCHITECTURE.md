# stdout/stderr 捕获原理：基于 dup2 的文件描述符重定向

## 1. 问题背景

主程序通过 `dlopen` 加载客户提供的 `.so` 动态库。客户代码中会使用 `printf`、`fprintf`、`std::cout`、甚至 `write` 系统调用直接向 stdout/stderr 输出内容。主程序需要在不修改客户代码的前提下，将这些输出同时写入自己的 log 文件。

**约束条件：**
- 不能要求客户修改代码或使用特定的日志接口
- 需要捕获所有层级的输出（C stdio、C++ iostream、POSIX write）
- 捕获期间屏幕上的正常显示不能丢失

## 2. 核心原理

### 2.1 Unix 文件描述符模型

在 Unix/Linux 中，每个进程维护一个**文件描述符表（fd table）**，其中：

```
fd 0 → stdin
fd 1 → stdout  ← printf/cout 最终写到这里
fd 2 → stderr  ← fprintf(stderr,...) 最终写到这里
```

关键认识：`printf` 并不是"打印到屏幕"，而是"写入 fd 1 指向的目标"。如果我们改变 fd 1 的指向，所有写入 fd 1 的操作都会被重定向，无论调用者使用的是哪一层 API。

### 2.2 各层 API 最终都汇聚到 fd

```
应用层          C 库层            内核层
─────────     ──────────       ─────────
printf()  ──→ fwrite()  ──→  write(1, ...)  ──→  fd 1 指向的目标
std::cout ──→ streambuf ──→  write(1, ...)  ──→  fd 1 指向的目标
write(1,...)              (直接)             ──→  fd 1 指向的目标
```

因此，只要在 fd 层面做重定向，就能一次性捕获所有层级的输出。

### 2.3 dup / dup2 系统调用

```c
int dup(int oldfd);
// 复制 oldfd，返回一个新的 fd，指向同一个内核文件对象

int dup2(int oldfd, int newfd);
// 让 newfd 指向 oldfd 的目标。如果 newfd 已打开，先关闭它
```

`dup2` 是整个方案的核心：它能原子地替换一个 fd 的指向目标。

## 3. 实现流程

### 3.1 总体架构

```
                    ┌──────────────────────────────┐
                    │          主程序进程            │
                    │                              │
                    │  ┌────────┐    ┌───────────┐ │
                    │  │ 主线程  │    │ 读取线程   │ │
                    │  │        │    │           │ │
  ┌───────────┐     │  │ printf ─┼──→ pipe ──┬──→ log 文件  │
  │ 客户 .so  │────→│  │ write  │    │      │   │ │
  └───────────┘     │  │        │    │      └──→ 真实屏幕  │
                    │  └────────┘    └───────────┘ │
                    └──────────────────────────────┘
```

### 3.2 启动捕获（capture_start）

```
步骤 1: 保存原始 fd
    saved_fd = dup(STDOUT_FILENO)    // saved_fd 现在指向真实终端

    fd 表状态:
    ┌─────┬─────────────┐
    │ fd  │ 指向         │
    ├─────┼─────────────┤
    │  1  │ 终端 (/dev/tty) │
    │  5  │ 终端 (/dev/tty) │  ← saved_fd，同一个终端
    └─────┴─────────────┘

步骤 2: 创建 pipe
    pipe(pipe_fds)                   // pipe_fds[0]=读端, pipe_fds[1]=写端

    fd 表状态:
    ┌─────┬─────────────┐
    │  1  │ 终端         │
    │  5  │ 终端         │  ← saved_fd
    │  6  │ pipe 读端    │  ← pipe_fds[0]
    │  7  │ pipe 写端    │  ← pipe_fds[1]
    └─────┴─────────────┘

步骤 3: 重定向 stdout 到 pipe
    dup2(pipe_fds[1], STDOUT_FILENO) // fd 1 现在指向 pipe 写端
    close(pipe_fds[1])               // 关闭多余的写端引用

    fd 表状态:
    ┌─────┬─────────────┐
    │  1  │ pipe 写端    │  ← stdout 现在写入 pipe！
    │  5  │ 终端         │  ← saved_fd，保留真实终端的引用
    │  6  │ pipe 读端    │  ← 读取线程从这里读
    └─────┴─────────────┘

步骤 4: 启动读取线程
    读取线程循环执行:
    while (read(pipe_rd, buf, ...) > 0) {
        写入 log 文件;
        write(saved_fd, buf, n);  // 通过保存的 fd 写到真实屏幕
    }
```

### 3.3 捕获期间的数据流

```
客户代码 printf("hello")
    │
    ▼
C 库 fwrite → write(1, "hello", 5)
    │
    ▼
fd 1 → pipe 写端
    │
    ▼
pipe 内核缓冲区
    │
    ▼
读取线程 read(pipe_rd) → "hello"
    │
    ├──→ fprintf(log_fp, "[STDOUT] hello")   // 写入 log 文件
    │
    └──→ write(saved_fd, "hello", 5)          // 写到真实终端
```

### 3.4 停止捕获（capture_stop）

```
步骤 1: flush stdio 缓冲区
    fflush(stdout)    // 确保缓冲区中的数据进入 pipe

步骤 2: 恢复 fd
    dup2(saved_fd, STDOUT_FILENO)  // fd 1 重新指向终端
    close(saved_fd)

    fd 表状态:
    ┌─────┬─────────────┐
    │  1  │ 终端         │  ← 恢复正常
    │  6  │ pipe 读端    │
    └─────┴─────────────┘

步骤 3: 关闭 pipe 读端
    close(pipe_fds[0])
    // pipe 写端已无引用 → read() 返回 0 → 读取线程退出

步骤 4: 等待线程退出
    pthread_join(thread)
```

## 4. 关键设计考量

### 4.1 为什么用 pipe 而不是直接重定向到文件

如果 `dup2(log_fd, STDOUT_FILENO)` 直接把 stdout 指向 log 文件：
- **无法同时输出到屏幕**——内容只进 log 文件
- **无法添加前缀**（如 `[STDOUT]`）——直接写入，没有加工机会

用 pipe + 读取线程，数据经过线程中转，可以同时分发到多个目标并做格式化。

### 4.2 为什么需要 fflush

C 标准库的 `printf` 等函数有用户态缓冲区。在切换 fd 指向之前，必须 `fflush` 把缓冲区数据冲刷到当前 fd，否则：
- **启动时不 flush**：之前缓冲的内容可能在 fd 切换后才写出，导致混入 pipe
- **停止时不 flush**：缓冲区内容还没进入 pipe，恢复 fd 后直接写到终端，log 丢失数据

### 4.2.1 为什么需要 setvbuf 强制 line-buffered

仅做 `fflush` 还不够。glibc / macOS libc 在 stdio 流首次 I/O 时会用 `isatty()` 探测目标 fd：
- 目标是 tty → **line-buffered**（每行立即 flush）
- 目标不是 tty（pipe / 文件 / socket）→ **block-buffered**（默认 4KB 才 flush）

`dup2` 把 fd 1 从 tty 切到 pipe 后，stdio 仍然认为 stdout 是它最初判定的状态。如果 stdout 此前已经做过 I/O（比如 `capture_start` 之前已经有 `printf`），现状是 line-buffered，没问题；但**只要 stdio 在 fd 切换后第一次做 I/O 时重新做 isatty 判定（实现相关）**，就会切到 block-buffered。

实际后果：
- `printf("xxx\n")` 不再每行 flush，全部攒在 libc 用户态 buffer
- `fprintf(stderr, ...)` 默认 unbuffered，立即穿过 pipe
- 客户代码里直接调用的 `write(STDOUT_FILENO, ...)` 也是 syscall，立即穿过 pipe
- 直到 `capture_stop` 调用 `fflush(stdout)` 才把累积的 stdout buffer 一次性吐出来

**结果是输出顺序错乱、log 文件里多行 stdout 共用一个 `[STDOUT]` 前缀**（reader 一次 read 拿到了整块）。

修复方法：在 `capture_start` 启动重定向后，强制 stdout 为行缓冲：

```c
setvbuf(stdout, NULL, _IOLBF, 0);
```

stderr 默认就是 unbuffered，无需改动。

注：POSIX 严格规定 `setvbuf` 应在流的首次 I/O 之前调用，但 glibc / macOS libc 实践上允许中途调用并立即生效，本方案即依赖这一行为。如果担心严格可移植性，可以改为在每次写完关键节点后显式 `fflush`，但那需要侵入业务代码，不如 `setvbuf` 干净。

### 4.3 为什么需要单独的读取线程

`pipe` 有内核缓冲区（Linux 默认 64KB，macOS 类似）。如果主线程在写入 pipe 的同时没有人读取，缓冲区满后 `write` 会阻塞，导致主线程（和客户代码）死锁。读取线程持续消费 pipe 数据，保证不会阻塞。

### 4.4 线程安全

- `pipe` 的 `read`/`write` 本身是线程安全的（内核保证）
- `log_fp` 的写入需要保证线程安全：本 demo 中只有读取线程写 `log_fp`，所以没有竞争
- 如果主程序的 log 系统是多线程共享的，`your_log_write` 需要自行加锁

## 5. 方案对比

| 方案 | 能捕获 printf | 能捕获 write(2) | 能同时输出屏幕 | 侵入性 |
|------|:---:|:---:|:---:|:---:|
| **dup2 + pipe** | ✓ | ✓ | ✓ | 无 |
| freopen | ✓ | ✗ | ✗ | 无 |
| LD_PRELOAD hook | ✓ | 需额外 hook | ✓ | 高 |
| setvbuf 自定义缓冲 | ✓ | ✗ | ✗ | 中 |
| 提供回调接口给客户 | 需客户配合 | 需客户配合 | ✓ | 高 |

## 6. capture_stop 是否必须调用

### 6.1 结论

不调用不会 crash，但**可能丢失 log 尾部数据**。正式场景中必须调用。

### 6.2 不调用时的退出流程

当 `main()` 返回且未调用 `capture_stop()` 时，进程退出经过以下阶段：

```
main() 返回
    │
    ▼
exit()
    ├── 调用 atexit 处理函数
    ├── fflush 所有 stdio 流   ← 缓冲区数据写入 pipe
    ├── fclose 所有 stdio 流
    │
    ▼
_exit()
    ├── 关闭所有 fd            ← pipe 写端关闭，read() 将返回 0
    └── 终止所有线程            ← 读取线程被强制终止
```

**竞争条件在这里**：`_exit()` 关闭 fd 和终止线程几乎同时发生。读取线程可能还没来得及把 pipe 中最后一批数据 `read()` 出来并写入 log 文件，就已经被终止了。

### 6.3 时序图对比

**不调用 capture_stop（有竞争）：**

```
主线程          读取线程           内核
  │                │               │
  │ exit()         │               │
  │ fflush ────────────────────→ 数据进入 pipe 缓冲区
  │                │               │
  │ _exit() ───→ 关闭 fd ─────→ pipe 写端关闭
  │          ├──→ 终止线程 ──→ ✗ 读取线程被杀死
  │                               │
  │           ← 数据可能还在 pipe 缓冲区中，未被读取 →
```

**调用 capture_stop（安全）：**

```
主线程              读取线程           内核
  │                    │               │
  │ fflush(stdout) ──────────────→ 数据进入 pipe 缓冲区
  │                    │               │
  │ dup2(saved,1) ──→ fd 1 恢复为终端
  │ close(pipe_rd) ──→              pipe 读端关闭
  │                    │               │
  │                    │ read()=0 ← pipe 写端已无引用
  │                    │ 写入最后数据到 log
  │                    │ 线程正常退出
  │                    │               │
  │ pthread_join ──→ 确认线程已退出
  │                    │               │
  │ fclose(log_fp) ─→ log 文件正确关闭
  │                    │               │
  │ 继续执行 / 退出
```

### 6.4 影响汇总

| 方面 | 不调用 | 调用 |
|------|--------|------|
| 程序能否正常退出 | 能 | 能 |
| log 文件数据完整 | 不保证，尾部可能丢失 | 保证 |
| 读取线程安全退出 | 竞争条件，可能被强杀 | `pthread_join` 等待完成 |
| fd / 资源泄漏 | 进程退出时 OS 回收，不算真泄漏 | 显式释放，干净 |
| log 文件正确关闭 | 不保证 flush | `fclose` 保证 |

### 6.5 实际建议

- **Demo / 一次性脚本**：不调也行，丢一两行 log 无所谓
- **正式服务 / 需要 log 完整性**：必须调用。可配合 `atexit(capture_stop)` 作为兜底
- **信号处理场景**（如 `SIGTERM` 优雅退出）：在信号处理函数中也应调用 `capture_stop()`，但需注意信号处理函数中只能调用 async-signal-safe 的函数，`fflush` 和 `pthread_join` 并不在其中。实际做法是设置标志位，在主循环中检查并调用

## 7. 局限性

1. **多线程时序**：如果多个线程同时向 stdout 写入，pipe 中的数据可能交错。`[STDOUT]` 前缀标记的是读取线程的一次 `read` 调用结果，不一定对应一次完整的 `printf` —— 即使强制了 line-buffered，多次 `write` 在 pipe 中仍可能被一次 `read` 合并。如需严格按行打前缀，应在 reader_thread 中按 `\n` 切分后再加前缀。
2. **性能开销**：数据多了一次用户态拷贝（pipe → 读取线程 → log / 屏幕），对于极高频输出有可测量的开销。
3. **不能捕获直接写 `/dev/tty` 的行为**：如果客户代码 `open("/dev/tty", ...)` 直接写终端设备，绕过了 fd 1，无法捕获。但这种做法极为罕见。

## 8. 子进程行为（system / popen / fork+exec）

由 fd 表是**进程级**资源，且 `fork` 默认整表继承、`exec` 默认保留（除非设置了 `FD_CLOEXEC`），所以 `capture_start` 之后调用：

- `system("cmd")` 启动的 shell 及其子命令
- `popen("cmd", "r"/"w")` 启动的子进程
- 直接 `fork()` + `execvp()` 启动的子进程

它们继承到的 fd 1 / fd 2 都指向**父进程的 pipe 写端**，stdout / stderr 自然被同样重定向、同样被 reader 线程读到、同样写进 log 文件和真实终端。无需任何额外代码。

注意点：
- 子进程是另一个进程，有它自己独立的 stdio buffer。`setvbuf(stdout, NULL, _IOLBF, 0)` 只影响**当前进程**，对子进程无效。如果子命令把 stdout 输出当成 pipe 来 block-buffer，可能要等 cmd 退出（buffer flush）才能看到全部输出 —— 这是子命令自身行为，不是本捕获方案的问题。
- 如果子进程显式做了 `freopen` / `dup2` / shell 重定向（如 `cmd > /dev/null`），那这些输出绕过了继承的 fd，无法捕获。
- `fork` 之后父子进程都持有 pipe 写端的引用，子进程退出时其引用计数 -1，父进程仍持有，pipe 不会因此关闭。
