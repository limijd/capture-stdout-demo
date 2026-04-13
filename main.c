#include <stdio.h>
#include "capture.h"
#include "customer.h"

int main(void) {
    printf("=== 我的程序启动 ===\n");

    // 开始捕获，所有 stdout/stderr 都会同时写入 log 文件
    if (capture_start("output.log") < 0) {
        perror("capture_start failed");
        return 1;
    }

    printf("我的程序: 准备调用客户代码\n");

    // 调用客户 .so 中的函数
    customer_work();

    printf("我的程序: 客户代码执行完毕\n");

    // 停止捕获
    capture_stop();

    printf("=== 捕获已停止，这行不会出现在 log 中 ===\n");
    return 0;
}
