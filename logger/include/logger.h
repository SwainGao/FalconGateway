#pragma once

// ============================================================================
// Falcon HFT Logger — 高频交易系统日志库
// ============================================================================
// 基于 Quill v11.1.0 异步日志库，针对 HFT 场景优化配置。
//
// 核心优化:
//   - 编译期日志级别裁剪: 发布版可剥离 DEBUG/TRACE，零开销
//   - TSC 时间戳: CPU 时间戳计数器，前端 ~20 cycles，无线程同步开销
//   - 崩溃安全: 内置信号处理器，SIGSEGV/SIGABRT 等触发时自动刷盘
//   - 无 fsync: 依赖操作系统 Page Cache，崩溃时信号处理器显式 flush
//   - 预分配队列: logger::preallocate() 消除首次 LOG 调用的 malloc 延迟
//
// 使用方式:
//   1. 启动时调用一次 logger::init()
//   2. 每个热路径线程启动时调用 logger::preallocate()
//   3. 使用 LOG_* 宏进行日志记录
// ============================================================================

// ---------------------------------------------------------------------------
// 编译期日志级别裁剪（发布版优化）
// ---------------------------------------------------------------------------
// 取消注释以在发布版中移除指定级别以下的日志代码:
//   QUILL_COMPILE_ACTIVE_LOG_LEVEL_INFO     — 剥离 DEBUG, TRACE_L1~L3
//   QUILL_COMPILE_ACTIVE_LOG_LEVEL_WARNING  — 剥离 INFO, DEBUG, TRACE
//   QUILL_COMPILE_ACTIVE_LOG_LEVEL_ERROR    — 仅保留 ERROR, CRITICAL (最激进)
//
// 默认不定义 = 所有级别编译进二进制（开发/调试模式适用）
// ---------------------------------------------------------------------------
// #define QUILL_COMPILE_ACTIVE_LOG_LEVEL QUILL_COMPILE_ACTIVE_LOG_LEVEL_INFO

// 禁止非前缀宏，避免与 <syslog.h> 的 LOG_INFO 等冲突
#define QUILL_DISABLE_NON_PREFIXED_MACROS

// 发布版移除函数名，减少二进制体积 & 防止源码路径信息泄露
#ifdef NDEBUG
#define QUILL_DISABLE_FUNCTION_NAME
// 如需完全移除文件路径和行号（最高安全级别），取消下面注释:
//   #define QUILL_DISABLE_FILE_INFO
#endif

#include <quill/LogMacros.h>
#include <quill/Logger.h>

namespace logger
{
    // 全局 Logger 实例指针 (在 logger.cpp 中定义)
    extern quill::Logger *g_logger;

    // 初始化日志库。启动后端线程、创建 Sink 和 Logger 实例。
    // 应在 main() 中最早调用，在任何日志语句之前。
    void init();

    // 预分配当前线程的 SPSC 队列（~256KB 固定）。
    // 消除首次 LOG 调用时的 malloc 延迟。
    // 建议在每个交易/策略线程启动时调用一次。
    void preallocate();

    // 将当前线程的 SPSC 队列收缩到指定容量(字节)。
    // 适用场景: 日志突发过后回收内存。仅对 Unbounded 队列有效。
    void shrink_queue(size_t target_capacity);
} // namespace logger

// ============================================================================
// 日志宏 — 兼容现有代码
// ============================================================================
#define LOG_DEBUG(fmt, ...) QUILL_LOG_DEBUG(logger::g_logger, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) QUILL_LOG_INFO(logger::g_logger, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) QUILL_LOG_WARNING(logger::g_logger, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) QUILL_LOG_ERROR(logger::g_logger, fmt, ##__VA_ARGS__)
#define LOG_CRITICAL(fmt, ...) QUILL_LOG_CRITICAL(logger::g_logger, fmt, ##__VA_ARGS__)
