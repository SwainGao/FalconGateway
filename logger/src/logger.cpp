#include "logger.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/backend/SignalHandler.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <iostream>

namespace logger {
quill::Logger *g_logger = nullptr;

void init() {
  // ====================================================================
  // 1. 配置 BackendOptions（后端线程行为）
  // ====================================================================
  quill::BackendOptions backend_options;

  // ---- CPU 亲和性 ----
  // 将后端线程绑定到指定物理核心，避免与交易线程竞争 L1/L2 缓存。
  // 使用前务必通过 isolcpus 隔离该核心。
  // 默认值: numeric_limits<uint16_t>::max() = 不绑定
  // backend_options.cpu_affinity = 2;

  // ---- 字符过滤 ----
  // 禁用 ASCII 检查，允许 UTF-8（如中文行情备注）直接通过。
  backend_options.check_printable_char = {};

  // ---- Transit Event Buffer ----
  // 后端每次从前端队列消费日志后暂存在此缓冲区中。
  // 单位: 事件条目数（非字节）。必须为 2 的幂。
  // 默认 256 太小 → 多线程高吞吐下会频繁触发 soft_limit。
  // 设为 8192: 足以缓冲 16 线程 × 各 512 条日志的突发量。
  backend_options.transit_event_buffer_initial_capacity = 8192;

  // ---- 反压触发阈值 ----
  // 积压事件数超过此值时后端放弃休眠全力消费。
  // 默认 8192 在 HFT 场景下偏高（已足够安全），保持默认。
  // backend_options.transit_events_soft_limit = 8192;

  // ---- 反压硬上限 ----
  // 单线程队列暂存事件数的绝对上限。默认 65536。
  // 对于 UnboundedBlocking 队列，到达硬上限后调用线程阻塞。
  // backend_options.transit_events_hard_limit = 65536;

  // ---- 后端线程休眠 ----
  // HFT 优化: sleep_duration = 0 + enable_yield_when_idle = true
  // 无日志时后端让出 CPU (yield)，有日志时立即消费。
  // 注意: 配合 cpu_affinity 绑核后，yield 开销可控。
  backend_options.sleep_duration = std::chrono::microseconds{0};
  backend_options.enable_yield_when_idle = true;

  // ---- 时间戳排序宽限期 ----
  // 默认 5μs: 后端等待 5μs 再消费，减少跨核心 TSC 偏差导致的时间戳乱序。
  // HFT 优化: 设为 0μs，优先消费速度 > 微秒级时间戳顺序。
  // 如需严格审计的时间戳顺序，可恢复为 5μs 或更高。
  backend_options.log_timestamp_ordering_grace_period =
      std::chrono::microseconds{0};

  // ---- TSC 重同步间隔 ----
  // RdtscClock 与系统墙上时间重新校准的间隔。默认 500ms。
  // 减小此值可提高时间戳精度，但增加系统时钟调用开销（仅影响后端）。
  // backend_options.rdtsc_resync_interval =
  // std::chrono::milliseconds{500};

  // ---- 错误通知器 ----
  // 队列满阻塞、格式错误等事件通过此回调报告。
  // 注意: 在后端线程执行，不可调用 LOG_* 宏（Quill 内部有保护）。
  backend_options.error_notifier = [](std::string const &error_msg) {
    // 过滤掉队列扩容的 INFO 通知（不是错误，Benchmark 场景下尤其嘈杂）
    // 如需观察队列扩容行为，注释掉这个 if 即可
    if (error_msg.find("Allocated a new SPSC queue") == std::string::npos) {
      std::cerr << "[Quill Error]: " << error_msg << std::endl;
    }
  };

  // ---- Sink 最小刷新间隔 ----
  // 默认 200ms: 后端至少每隔 200ms 调用一次 sink->flush_sink()。
  // HFT 优化: 减小到 50ms，更快地将数据推向磁盘 Page Cache。
  backend_options.sink_min_flush_interval = std::chrono::milliseconds{50};

  // ---- 信号处理器选项 ----
  quill::SignalHandlerOptions signal_options;
  // 显式指定崩溃日志使用的 logger。不指定时 Quill 自动选择第一个
  // 有效 logger，但显式指定更可预测（尤其后续添加多个 logger 时）。
  signal_options.logger_name = "Falcon";

  // 信号处理超时（秒）。如果 signal handler 内部卡死（极端情况），
  // 超时后 alarm 会强制终止进程。默认 20s 对 HFT 偏长：
  // 缩短为 5s，让 failover 更快接管。
  signal_options.timeout_seconds = 5;

  // ====================================================================
  // 2. 启动后端
  // ====================================================================
  quill::Backend::start<quill::FrontendOptions>(backend_options,
                                                signal_options);

  // ====================================================================
  // 3. 创建 Sink 链
  // ====================================================================
  std::vector<std::shared_ptr<quill::Sink>> sinks;

  // ---- 3a. 滚动文件 Sink（始终启用） ----
  auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
      "./log/Falcon", []() {
        quill::RotatingFileSinkConfig config;

        // 追加模式: 进程重启不覆盖历史日志
        config.set_open_mode('a');

        // 文件名: Falcon_2026-06-13_21-35.log
        // 使用 小时-分钟 粒度: 同小时内 size-based 轮转不会产生文件名冲突。
        // strftime 格式: %H_%M = 小时_分钟，用 '-' 连接日期和时间。
        config.set_filename_append_option(
            quill::FilenameAppendOption::StartCustomTimestampFormat,
            "_%Y-%m-%d_%H-%M.log");

        // 每小时轮转一次
        config.set_rotation_frequency_and_interval('H', 1);

        // 单文件 1 GB 上限（防止异常日志洪峰写满磁盘）
        // HFT 正常负载下 ~100MB/小时，1GB 留有充足余量。
        config.set_rotation_max_file_size(1024 * 1024 * 1024);

        // 保留最近 48 个文件（48 个连续分钟段或小时段的历史）
        // 分钟粒度下同小时可能有多个文件，增大保留数。
        config.set_max_backup_files(24);

        // 轮转时覆盖旧文件（保持文件数恒定）
        config.set_overwrite_rolled_files(true);

        // 不依赖 fsync: 操作系统 Page Cache 批量刷盘。
        // 崩溃安全由信号处理器提供（flush_log 显式刷盘）。
        config.set_fsync_enabled(false);

        return config;
      }());

  sinks.push_back(file_sink);
#ifdef NDEBUG
  // ---- 3b. 发布版：控制台 Sink（仅 ERROR 及以上） ----
  {
    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
        "console_sink", []() {
          quill::ConsoleSinkConfig config;
          config.set_colour_mode(
              quill::ConsoleSinkConfig::ColourMode::Automatic);
          return config;
        }());
    // Sink 级别过滤: 仅 ERROR/CRITICAL 输出到控制台
    console_sink->set_log_level_filter(quill::LogLevel::Error);
    sinks.push_back(console_sink);
  }
#else
  // ---- 3b. 调试版：控制台 Sink（全级别输出） ----
  {
    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
        "console_sink", []() {
          quill::ConsoleSinkConfig config;
          config.set_colour_mode(
              quill::ConsoleSinkConfig::ColourMode::Automatic);
          return config;
        }());
    sinks.push_back(console_sink);
  }
#endif

  // ====================================================================
  // 4. 创建 Logger 实例
  // ====================================================================
  // Pattern 字段说明:
  //   %(time)              — 时间戳（TSC 转换）
  //   [%(thread_id)]       — 线程 ID
  //   %(short_source_location:<28) — 文件名:行号（右对齐 28 字符）
  //   LOG_%(log_level:<9)  — 日志级别（左对齐 9 字符）
  //   %(logger:<12)        — Logger 名称
  //   %(message)           — 日志正文
  g_logger = quill::Frontend::create_or_get_logger(
      "Falcon", std::move(sinks),
      quill::PatternFormatterOptions{
          "%(time) [%(thread_id)] %(short_source_location:<28) "
          "LOG_%(log_level:<9) %(logger:<12) %(message)",
          "%Y-%m-%d %H:%M:%S.%Qus", quill::Timezone::LocalTime});

  // ====================================================================
  // 5. 运行时日志级别
  // ====================================================================
#ifdef NDEBUG
  g_logger->set_log_level(quill::LogLevel::Info);
#else
  g_logger->set_log_level(quill::LogLevel::Debug);
#endif
}

void preallocate() {
  // 预分配当前线程的 SPSC 队列。
  // UnboundedBlocking 队列默认从 256KB 开始，按需扩展到
  // unbounded_queue_max_capacity（默认 ~1GB）。
  // 预分配可消除首次 LOG 调用时的 ~100-500ns malloc 延迟。
  quill::Frontend::preallocate();
}

void shrink_queue(size_t target_capacity) {
  // 将当前线程的 SPSC 队列收缩到指定容量
  // 适用场景: 某一线程经历日志突发后不再需要大容量
  // 主动回收内存 (例如线程池 Worker 处理完一批消息后)
  // 注意: 仅对 Unbounded 队列类型生效，Bounded 队列无效果。
  quill::Frontend::shrink_thread_local_queue(target_capacity);
}
} // namespace logger