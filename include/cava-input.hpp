#pragma once

#include <stddef.h>
#include <stdint.h>

#define CAVA_BARS_NUMBER 128

enum cava_status {
    CAVA_OK = 0,
    CAVA_ERR = -1,
};

// 启动 cava 读取线程
// bit_format: "16bit" or "8bit"
// bars_number: number of bars (usually CAVA_BARS_NUMBER)
// ring_capacity: number of sample slots to keep (power of two recommended)
// Returns CAVA_OK on success, CAVA_ERR on failure.
int cava_reader_start(const char *bit_format, size_t bars_number, size_t ring_capacity);

// 停止并 join 读取线程（阻塞直到清理完成）
void cava_reader_stop(void);

// 非阻塞尝试读取一帧数据（长度 == bars_number passed to start）。
// out_buf must point to an array of at least bars_number floats.
// Returns 1 if a frame was read, 0 if no frame available, -1 on error.
int cava_reader_try_pop(float *out_buf, size_t max_len);

// 查询启动时使用的 bars_number（只读）
size_t cava_reader_bars_number(void);

// 查询当前运行状态：1=running, 0=stopped
int cava_reader_running(void);