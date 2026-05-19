#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读取 path，若内容含 needle 子串返回 1，否则 0；找不到文件返回 0。 */
int file_contains(const char *path, const char *needle);

/* 数 path 中 needle 子串的出现次数（非重叠扫描）；找不到文件返回 0。 */
size_t file_count_occurrences(const char *path, const char *needle);

/* 统计 path 中匹配 pattern (sprintf 模板含 %02d-%04d 类格式) 的独一无二行数。 */
size_t count_unique_tokens(const char *path, const char *pattern,
                           int outer_n, int inner_n);

/* fork N 个 child，每个 child 调 child_fn(idx)，返回成功 fork 数量。 */
size_t fork_n_children(int n, void (*child_fn)(int));

/* waitpid 所有未回收的 child；返回回收数量。 */
size_t wait_all_children(void);

/* 对 fd 做 fstat，把 st_ino 和 st_dev 写入输出参数。 */
void get_fd_identity(int fd, unsigned long long *ino, unsigned long long *dev);

#ifdef __cplusplus
}
#endif
