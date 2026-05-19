#include "helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

int file_contains(const char *path, const char *needle) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return 0; }
    size_t r = fread(buf, 1, (size_t)sz, fp);
    buf[r] = '\0';
    fclose(fp);
    int hit = strstr(buf, needle) != NULL;
    free(buf);
    return hit;
}

size_t file_count_occurrences(const char *path, const char *needle) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return 0; }
    size_t r = fread(buf, 1, (size_t)sz, fp);
    buf[r] = '\0';
    fclose(fp);
    size_t count = 0;
    size_t nl = strlen(needle);
    if (nl == 0) { free(buf); return 0; }
    const char *p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nl;
    }
    free(buf);
    return count;
}

size_t count_unique_tokens(const char *path, const char *pattern,
                           int outer_n, int inner_n) {
    size_t hit = 0;
    char tok[64];
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return 0; }
    size_t r = fread(buf, 1, (size_t)sz, fp);
    buf[r] = '\0';
    fclose(fp);
    for (int c = 0; c < outer_n; c++) {
        for (int i = 0; i < inner_n; i++) {
            snprintf(tok, sizeof(tok), pattern, c, i);
            if (strstr(buf, tok)) hit++;
        }
    }
    free(buf);
    return hit;
}

size_t fork_n_children(int n, void (*child_fn)(int)) {
    size_t spawned = 0;
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) { child_fn(i); _exit(0); }
        if (pid > 0) spawned++;
    }
    return spawned;
}

size_t wait_all_children(void) {
    size_t n = 0;
    int status;
    while (waitpid(-1, &status, 0) > 0) n++;
    return n;
}

void get_fd_identity(int fd, unsigned long long *ino,
                     unsigned long long *dev) {
    struct stat st;
    if (fstat(fd, &st) == 0) {
        *ino = (unsigned long long)st.st_ino;
        *dev = (unsigned long long)st.st_dev;
    } else {
        *ino = 0; *dev = 0;
    }
}
