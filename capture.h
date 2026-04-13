// stdout/stderr 捕获模块
#pragma once

// 开始捕获，所有 stdout/stderr 输出会被转发到 log 文件
// 同时保持屏幕输出不变
int capture_start(const char *log_path);

// 停止捕获，恢复原始 stdout/stderr
void capture_stop(void);
