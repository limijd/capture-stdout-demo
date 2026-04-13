# stdout/stderr Capture via dup2

Capture output from dynamically loaded shared libraries (`.so`) into your own log file — without modifying the library's source code.

## How it works

Uses `dup2` to redirect file descriptors at the kernel level, combined with a `pipe` and a reader thread that forwards captured output to both a log file and the real terminal.

This captures **all** output methods: `printf`, `fprintf`, `std::cout`, and raw `write()` syscalls.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for a detailed technical explanation.

## Build & Run

```bash
make run
```

Output:

```
=== 我的程序启动 ===
我的程序: 准备调用客户代码
客户 printf: hello from customer
客户 fprintf: some data = 42
客户 stderr: warning something
客户 write: raw write to stdout
我的程序: 客户代码执行完毕
=== 捕获已停止，这行不会出现在 log 中 ===

--- log 文件内容 ---
[STDERR] 客户 stderr: warning something
[STDOUT] 客户 write: raw write to stdout
我的程序: 准备调用客户代码
客户 printf: hello from customer
客户 fprintf: some data = 42
我的程序: 客户代码执行完毕
```

## Files

```
├── capture.c / capture.h   # Capture module (integrate this into your project)
├── customer.c / customer.h # Simulated customer .so
├── main.c                  # Demo entry point
├── Makefile
└── docs/ARCHITECTURE.md    # Technical deep-dive (Chinese)
```

## License

MIT
