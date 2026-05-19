// stdout/stderr 捕获组件（vendored source，纯 C）
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动捕获。
 *
 * 契约：
 *   - 必须由进程主线程在 fork 任何子进程 / dlopen 任何 .so 之前调用
 *   - 同进程同时只允许一次 active 捕获；start→stop→start 可重入
 *
 * 返回 0 表示成功；返回 -1 并设置 errno：
 *   EBUSY  已经在捕获中
 *   EINVAL log_path 为 NULL
 *   其它   透传自 fopen / dup / pipe / pthread_create
 */
int capture_start(const char *log_path);

/* 停止捕获并 drain reader、close log。
 *
 * 契约：
 *   - 调用前必须已经 waitpid() 回收所有 fork 出的子进程。否则 pipe write 端
 *     仍被 child 引用 → reader read() 永不返回 0 → pthread_join 死锁
 *   - 幂等：多次调用安全，第二次起 no-op，方便配合 atexit(capture_stop)
 *
 * Fork 安全：
 *   - 组件内置 pthread_atfork(child=...) handler，fork 后在 child 自动把
 *     capture 状态清零。因此 child 直接或间接（通过继承的 atexit）调
 *     capture_stop 是**安全 no-op**——不会 pthread_join 悬空 thread、
 *     不会 fclose 陈旧 FILE* 缓冲污染 log
 *   - 但这只是兜底；child 仍应优先用 _exit() 退出避免触发继承的 atexit
 */
void capture_stop(void);

/* 查询 reader 累计 log 写入错误次数（程序生命周期累计）。
 *
 * 任意时刻可调，包括 start 前（返回 0）、stop 后（返回累计值）。
 * 跨 start/stop 周期不重置；如需 per-session 计数，调用方在 capture_start
 * 前自行 snapshot 当前值，stop 后做差即可。
 * 线程安全（atomic load）。
 * 0 = 一切正常；非 0 时具体错误已通过原始 stderr 打印过诊断行。
 */
size_t capture_io_error_count(void);

#ifdef __cplusplus
}
#endif
