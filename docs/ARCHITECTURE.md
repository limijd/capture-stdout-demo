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

## 6. 局限性

1. **多线程时序**：如果多个线程同时向 stdout 写入，pipe 中的数据可能交错。`[STDOUT]` 前缀标记的是读取线程的一次 `read` 调用结果，不一定对应一次完整的 `printf`。
2. **性能开销**：数据多了一次用户态拷贝（pipe → 读取线程 → log / 屏幕），对于极高频输出有可测量的开销。
3. **不能捕获直接写 `/dev/tty` 的行为**：如果客户代码 `open("/dev/tty", ...)` 直接写终端设备，绕过了 fd 1，无法捕获。但这种做法极为罕见。
