#include "logger.h"

#include <chrono>
#include <ctime>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>
#include <string_view>

namespace rime::aipara {

namespace {
constexpr std::string_view kDebug = "DEBUG";
constexpr std::string_view kInfo = "INFO";
constexpr std::string_view kWarn = "WARN";
constexpr std::string_view kError = "ERROR";

Logger::Level ParseLevelFromName(const std::string& level_name) {
  std::string upper = level_name;
  for (char& c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper == kDebug) {
    return Logger::Level::kDebug;
  }
  if (upper == kInfo) {
    return Logger::Level::kInfo;
  }
  if (upper == kWarn) {
    return Logger::Level::kWarn;
  }
  if (upper == kError) {
    return Logger::Level::kError;
  }
  return Logger::Level::kInfo;
}
}  // namespace

std::mutex Logger::config_mutex_;
std::mutex Logger::io_mutex_;
Logger::EffectiveConfig Logger::default_config_;
std::optional<bool> Logger::global_enabled_;
std::optional<bool> Logger::global_unique_file_log_;
std::string Logger::global_unique_filename_;

Logger Logger::Create(const std::string& module_name,
                      const Options& options) {
  std::scoped_lock lock(config_mutex_);

  EffectiveConfig config = ResolveConfig(options);

  if (global_enabled_.has_value()) {
    config.enabled = *global_enabled_;
  }
  if (global_unique_file_log_.has_value()) {
    config.unique_file_log = *global_unique_file_log_;
  }
  if (!global_unique_filename_.empty()) {
    config.unique_file_log_file = global_unique_filename_;
  }

  std::filesystem::path log_file = ResolveLogFilePath(module_name, config);
  return Logger(module_name, config, log_file);
}

void Logger::SetDefaultOptions(const Options& options) {
  std::scoped_lock lock(config_mutex_);
  EffectiveConfig resolved = ResolveConfig(options);
  default_config_ = resolved;
}

Logger::Options Logger::GetDefaultOptions() {
  std::scoped_lock lock(config_mutex_);
  Options opts;
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
  if (!config_.enabled) {
    return;
  }
  std::scoped_lock lock(io_mutex_);
  std::filesystem::create_directories(log_file_path_.parent_path());
  std::ofstream file(log_file_path_, std::ios::trunc);
  if (!file) {
    std::cerr << "Failed to clear log file: " << log_file_path_ << std::endl;
    return;
  }
  std::cout << "日志文件已清空: " << log_file_path_ << std::endl;
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
  if (!config_.enabled || level < config_.min_level) {
    return;
  }

  std::string location_suffix;
  std::string display_module =
      ModuleNameFromSource(module_name_, source_file, source_line,
                           config_.show_line_info, &location_suffix);

  std::time_t now_c =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm_snapshot;
#ifdef _WIN32
  localtime_s(&tm_snapshot, &now_c);
#else
  localtime_r(&now_c, &tm_snapshot);
#endif
  std::ostringstream timestamp_stream;
  timestamp_stream << std::put_time(&tm_snapshot, config_.timestamp_format.c_str());
  const std::string timestamp = timestamp_stream.str();

  std::ostringstream line;
  line << "[" << timestamp << "] "
       << "[" << LevelToString(level) << "] "
       << "[" << display_module << location_suffix << "] "
       << message << '\n';

  std::string serialized = line.str();

  std::scoped_lock lock(io_mutex_);
  std::filesystem::create_directories(log_file_path_.parent_path());

  if (config_.console_output) {
    std::cout << serialized;
  }

  std::ofstream file(log_file_path_, std::ios::app);
  if (!file) {
    std::cerr << "Failed to open log file: " << log_file_path_ << std::endl;
    return;
  }
  file << serialized;
}

Logger::Level Logger::ParseLevel(const std::string& level_name) {
  return ParseLevelFromName(level_name);
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
  if (!show_line_info || source_file == nullptr) {
    if (location_suffix) {
      *location_suffix = "";
    }
    return module_name;
  }

  std::filesystem::path file_path(source_file);
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
