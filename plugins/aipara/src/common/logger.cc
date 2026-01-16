#include "logger.h"

// 这里包含了很多 C++ 标准库头文件：
// <chrono>/<ctime> 用于时间处理，<fstream> 用于文件读写，
// <iomanip> 控制输出格式，<mutex> 在头文件里已经包含。
// 可以把它们理解成 Python 的 datetime、io 等模块。
#include <chrono>
#include <ctime>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>
#include <string_view>

namespace rime::aipara {

namespace {
// 匿名命名空间里的内容只在当前 .cc 文件可见，类似 Python 模块的私有变量。
// constexpr 表示编译期常量，std::string_view 是只读的字符串视图，避免复制整段字符串。
constexpr std::string_view kDebug = "DEBUG";
constexpr std::string_view kInfo = "INFO";
constexpr std::string_view kWarn = "WARN";
constexpr std::string_view kError = "ERROR";
constexpr std::string_view kLoggerVersion = "v12";
}  // namespace

// 静态成员在类外初始化。互斥量（std::mutex）用于保护共享状态。
std::mutex Logger::config_mutex_;
std::mutex Logger::io_mutex_;
Logger::EffectiveConfig Logger::default_config_;
std::optional<bool> Logger::global_enabled_;
std::optional<bool> Logger::global_unique_file_log_;
std::string Logger::global_unique_filename_;

std::filesystem::path Logger::GetDefaultLogDir() {
#ifdef _WIN32
  const char* appdata = std::getenv("APPDATA");
  if (appdata && *appdata) {
    return std::filesystem::path(appdata) / "Rime" / "log";
  }
  const char* userprofile = std::getenv("USERPROFILE");
  if (userprofile && *userprofile) {
    return std::filesystem::path(userprofile) / "AppData" / "Roaming" /
           "Rime" / "log";
  }
  return std::filesystem::path("Rime") / "log";
#else
  const char* home = std::getenv("HOME");
  std::filesystem::path base = (home && *home) ? std::filesystem::path(home)
                                               : std::filesystem::path(".");
  return base / "Library" / "Aipara" / "log";
#endif
}

Logger Logger::Create(const std::string& module_name,
                      const Options& options) {
  // std::scoped_lock 在构造时自动加锁，析构时自动解锁，
  // 相当于 Python 里的 with threading.Lock(): 保护代码块的线程安全。
  std::scoped_lock lock(config_mutex_);

  EffectiveConfig config = ResolveConfig(options);

  // 如果外部设置了全局开关，就覆盖当前配置。
  // std::optional::has_value() 判断里面是否存了值。
  if (global_enabled_.has_value()) {
    config.enabled = *global_enabled_;
  }
  if (global_unique_file_log_.has_value()) {
    config.unique_file_log = *global_unique_file_log_;
  }
  if (!global_unique_filename_.empty()) {
    config.unique_file_log_file = global_unique_filename_;
  }

  // Force console mirroring so every log line also appears in the terminal.
  config.console_output = true;

  std::filesystem::path log_file = ResolveLogFilePath(module_name, config);
  Logger logger(module_name, config, log_file);
  // 记录版本初始化信息，方便定位代码是否更新生效。
  logger.Info(std::string("logger_init ") + module_name + "_" +
              std::string(kLoggerVersion));
  return logger;
}

void Logger::SetDefaultOptions(const Options& options) {
  std::scoped_lock lock(config_mutex_);
  EffectiveConfig resolved = ResolveConfig(options);
  default_config_ = resolved;
}

Logger::Options Logger::GetDefaultOptions() {
  std::scoped_lock lock(config_mutex_);
  Options opts;
  // 将内部配置转换成外部 Options 结构。string() 用来把 path 转成普通字符串。
  opts.enabled = default_config_.enabled;
  opts.log_dir = default_config_.log_dir.string();
  opts.timestamp_format = default_config_.timestamp_format;
  opts.unique_file_log = default_config_.unique_file_log;
  opts.unique_file_log_file = default_config_.unique_file_log_file;
  opts.console_output = default_config_.console_output;
  opts.log_level = LevelToString(default_config_.min_level);
  opts.show_line_info = default_config_.show_line_info;
  return opts;
}

void Logger::SetGlobalEnabled(std::optional<bool> enabled) {
  std::scoped_lock lock(config_mutex_);
  global_enabled_ = enabled;
}

void Logger::SetGlobalUniqueFileLog(std::optional<bool> enabled,
                                    const std::string& filename) {
  std::scoped_lock lock(config_mutex_);
  global_unique_file_log_ = enabled;
  if (!filename.empty()) {
    global_unique_filename_ = filename;
    default_config_.unique_file_log_file = filename;
  }
}

void Logger::SetConsoleOutput(bool enabled) {
  std::scoped_lock lock(config_mutex_);
  default_config_.console_output = enabled;
}

void Logger::SetLogLevel(const std::string& level) {
  std::scoped_lock lock(config_mutex_);
  default_config_.min_level = ParseLevel(level);
}

void Logger::SetShowLineInfo(bool enabled) {
  std::scoped_lock lock(config_mutex_);
  default_config_.show_line_info = enabled;
}

void Logger::SetLogDir(const std::string& log_dir) {
  std::scoped_lock lock(config_mutex_);
  default_config_.log_dir = std::filesystem::path(log_dir);
}

void Logger::Clear() const {
  // 如果日志整体被禁用，就直接返回。const 函数中仍然可以读取成员。
  if (!config_.enabled) {
    return;
  }
  {
    std::scoped_lock lock(io_mutex_);
    // std::filesystem::create_directories 会按需创建目录，相当于 Python 的 os.makedirs(..., exist_ok=True)。
    std::filesystem::create_directories(log_file_path_.parent_path());
    // std::ofstream 的第二个参数 std::ios::trunc 表示“清空文件重新写”。
    std::ofstream file(log_file_path_, std::ios::trunc);
    if (!file) {
      std::cerr << "Failed to clear log file: " << log_file_path_ << std::endl;
      return;
    }
  }
  std::cout << "日志文件已清空: " << log_file_path_ << std::endl;
  // 在清空文件后写入版本标记，确保最新日志可见。
  Info(std::string("logger_init ") + module_name_ + "_" +
       std::string(kLoggerVersion));
}

void Logger::Debug(const std::string& message,
                   const char* source_file,
                   int source_line) const {
  Write(Level::kDebug, message, source_file, source_line);
}

void Logger::Info(const std::string& message,
                  const char* source_file,
                  int source_line) const {
  Write(Level::kInfo, message, source_file, source_line);
}

void Logger::Warn(const std::string& message,
                  const char* source_file,
                  int source_line) const {
  Write(Level::kWarn, message, source_file, source_line);
}

void Logger::Error(const std::string& message,
                   const char* source_file,
                   int source_line) const {
  Write(Level::kError, message, source_file, source_line);
}

Logger::Logger(std::string module_name,
               EffectiveConfig config,
               std::filesystem::path log_file_path)
    : module_name_(std::move(module_name)),
      config_(std::move(config)),
      log_file_path_(std::move(log_file_path)) {}

void Logger::Write(Level level,
                   const std::string& message,
                   const char* source_file,
                   int source_line) const {
  // 如果日志被禁用或者当前级别低于最小级别，就直接返回。
  // 枚举比较遵循定义时的整数顺序。
  if (!config_.enabled || level < config_.min_level) {
    return;
  }

  // location_suffix 用于保存额外的行号信息。
  std::string location_suffix;
  std::string display_module =
      ModuleNameFromSource(module_name_, source_file, source_line,
                           config_.show_line_info, &location_suffix);

  // 获取当前时间，chrono::system_clock::now() 返回高精度时间点，
  // to_time_t 转成 C 风格时间戳，再用 localtime_r/localtime_s 转换成本地时间。
  // 注意 #ifdef 的写法，用于区分 Windows 和其他平台的 API 差异，这是 C++ 常见坑。
  std::time_t now_c =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm_snapshot;
#ifdef _WIN32
  localtime_s(&tm_snapshot, &now_c);
#else
  localtime_r(&now_c, &tm_snapshot);
#endif
  // std::ostringstream 类似 Python 的 io.StringIO，用来拼接字符串。
  // std::put_time 可以按自定义格式输出时间。
  std::ostringstream timestamp_stream;
  timestamp_stream << std::put_time(&tm_snapshot, config_.timestamp_format.c_str());
  const std::string timestamp = timestamp_stream.str();

  // 逐段拼接最终的日志行文本。
  std::ostringstream line;
  line << "[" << timestamp << "] "
       << "[" << LevelToString(level) << "] "
       << "[" << display_module << location_suffix << "] "
       << message << '\n';

  std::string serialized = line.str();

  std::scoped_lock lock(io_mutex_);
  // 确保日志目录存在，否则写文件会失败。
  std::filesystem::create_directories(log_file_path_.parent_path());

  // 如果配置允许，也把日志同步打印到终端（std::cout）。
  if (config_.console_output) {
    std::cout << serialized;
  }

  // 以追加模式写入文件，相当于 Python open(..., "a")。
  std::ofstream file(log_file_path_, std::ios::app);
  if (!file) {
    std::cerr << "Failed to open log file: " << log_file_path_ << std::endl;
    return;
  }
  file << serialized;
}

Logger::Level Logger::ParseLevel(const std::string& level_name) {
  // 这里直接把字符串映射到枚举值。
  // 先复制一份字符串并转成大写，避免大小写差异导致匹配失败。
  std::string upper = level_name;
  for (char& c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper == kDebug) {
    return Level::kDebug;
  }
  if (upper == kInfo) {
    return Level::kInfo;
  }
  if (upper == kWarn) {
    return Level::kWarn;
  }
  if (upper == kError) {
    return Level::kError;
  }
  // 默认退回到 Info，防止无效字符串造成崩溃。
  return Level::kInfo;
}

std::string Logger::LevelToString(Level level) {
  switch (level) {
    case Level::kDebug:
      return std::string(kDebug);
    case Level::kInfo:
      return std::string(kInfo);
    case Level::kWarn:
      return std::string(kWarn);
    case Level::kError:
      return std::string(kError);
  }
  return std::string(kInfo);
}

Logger::EffectiveConfig Logger::ResolveConfig(const Options& options) {
  EffectiveConfig config = default_config_;

  // 依次检查每个 optional 是否有值，有的话就覆盖默认配置。
  if (options.enabled.has_value()) {
    config.enabled = *options.enabled;
  }
  if (options.log_dir.has_value()) {
    config.log_dir = std::filesystem::path(*options.log_dir);
  }
  if (options.timestamp_format.has_value()) {
    config.timestamp_format = *options.timestamp_format;
  }
  if (options.unique_file_log.has_value()) {
    config.unique_file_log = *options.unique_file_log;
  }
  if (options.unique_file_log_file.has_value()) {
    config.unique_file_log_file = *options.unique_file_log_file;
  }
  if (options.console_output.has_value()) {
    config.console_output = *options.console_output;
  }
  if (options.log_level.has_value()) {
    config.min_level = ParseLevel(*options.log_level);
  }
  if (options.show_line_info.has_value()) {
    config.show_line_info = *options.show_line_info;
  }

  return config;
}

std::filesystem::path Logger::ResolveLogFilePath(
    const std::string& module_name,
    const EffectiveConfig& config) {
  // std::filesystem::path 支持 / 运算符拼接路径，自动处理分隔符。
  std::filesystem::path dir = config.log_dir;
  std::filesystem::path filename;

  if (config.unique_file_log) {
    filename = config.unique_file_log_file;
  } else {
    filename = module_name + ".log";
  }

  return dir / filename;
}

std::string Logger::ModuleNameFromSource(const std::string& module_name,
                                         const char* source_file,
                                         int source_line,
                                         bool show_line_info,
                                         std::string* location_suffix) {
  // source_file 是 const char*，来自编译器的 __FILE__，可能为 nullptr（比如外部直接调用）。
  if (!show_line_info || source_file == nullptr) {
    if (location_suffix) {
      *location_suffix = "";
    }
    return module_name;
  }

  std::filesystem::path file_path(source_file);
  // stem() 返回文件名去掉扩展名，类似 Python pathlib.Path(source_file).stem。
  std::string stem = file_path.stem().string();

  if (location_suffix) {
    if (source_line > 0) {
      *location_suffix = ":" + std::to_string(source_line);
    } else {
      *location_suffix = "";
    }
  }

  return stem.empty() ? module_name : stem;
}

Logger MakeLogger(const std::string& module_name,
                  const Logger::Options& options) {
  return Logger::Create(module_name, options);
}

}  // namespace rime::aipara
