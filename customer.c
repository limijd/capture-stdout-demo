// 模拟客户 .so：直接用 printf/fprintf 打印
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void customer_work(void) {
    printf("客户 printf: hello from customer\n");
    fprintf(stdout, "客户 fprintf: some data = %d\n", 42);
    fprintf(stderr, "客户 stderr: warning something\n");
    // 甚至直接用 write 系统调用
    const char *msg = "客户 write: raw write to stdout\n";
    write(STDOUT_FILENO, msg, strlen(msg));
}
