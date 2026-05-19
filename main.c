#include <stdio.h>
#include <sys/wait.h>
#include "capture.h"
#include "customer.h"

/* 回收所有 fork 出来的子进程（含 customer .so 内部可能偷偷 fork 的）。
   capture_stop 契约要求：调用前必须确保没有 child 还持有继承的 fd 1/2，
   否则 reader 永远等不到 pipe EOF → pthread_join 死锁。
   即使本 demo 当前 customer_work() 不 fork，这里仍调用以演示正确模板，
   且对 dlopen 的客户 .so 是真实兜底——客户可能在内部 fork 而你看不见。 */
static void wait_all_children(void) {
    while (waitpid(-1, NULL, 0) > 0) { /* drain */ }
}

int main(void) {
    printf("=== 我的程序启动 ===\n");

    // 开始捕获，所有 stdout/stderr 都会同时写入 log 文件
    if (capture_start("output.log") < 0) {
        perror("capture_start failed");
        return 1;
    }

    printf("我的程序: 准备调用客户代码\n");

    // 调用客户 .so 中的函数（客户可能在内部 fork，所以下面必须 wait_all_children）
    customer_work();

    printf("我的程序: 客户代码执行完毕\n");

    // 关键：先 wait 所有 child 再 capture_stop，否则 join 死锁
    wait_all_children();
    capture_stop();

    printf("=== 捕获已停止，这行不会出现在 log 中 ===\n");
    return 0;
}
