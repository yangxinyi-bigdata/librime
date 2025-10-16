#ifndef PLUGINS_AIPARA_SRC_COMMON_LOGGER_H_
#define PLUGINS_AIPARA_SRC_COMMON_LOGGER_H_

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace rime::aipara {

class Logger {
 public:
  struct Options {
    std::optional<bool> enabled;
    std::optional<std::string> log_dir;
    std::optional<std::string> timestamp_format;
    std::optional<bool> unique_file_log;
    std::optional<std::string> unique_file_log_file;
    std::optional<bool> console_output;
    std::optional<std::string> log_level;
    std::optional<bool> show_line_info;
  };

  static Logger Create(const std::string& module_name,
                       const Options& options = Options{});

  static void SetDefaultOptions(const Options& options);
  static Options GetDefaultOptions();

  static void SetGlobalEnabled(std::optional<bool> enabled);
  static void SetGlobalUniqueFileLog(std::optional<bool> enabled,
                                     const std::string& filename = "");
  static void SetConsoleOutput(bool enabled);
  static void SetLogLevel(const std::string& level);
  static void SetShowLineInfo(bool enabled);
  static void SetLogDir(const std::string& log_dir);

  void Clear() const;

  void Debug(const std::string& message,
             const char* source_file = nullptr,
             int source_line = 0) const;
  void Info(const std::string& message,
            const char* source_file = nullptr,
            int source_line = 0) const;
  void Warn(const std::string& message,
            const char* source_file = nullptr,
            int source_line = 0) const;
  void Error(const std::string& message,
             const char* source_file = nullptr,
             int source_line = 0) const;

  bool enabled() const { return config_.enabled; }

 private:
  enum class Level {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
  };

  struct EffectiveConfig {
    bool enabled = true;
    std::filesystem::path log_dir = "/Users/yangxinyi/Library/Rime/log/";
    std::string timestamp_format = "%Y-%m-%d %H:%M:%S";
    bool unique_file_log = false;
    std::string unique_file_log_file = "all_modules.log";
    bool console_output = false;
    Level min_level = Level::kDebug;
    bool show_line_info = true;
  };

  Logger(std::string module_name,
         EffectiveConfig config,
         std::filesystem::path log_file_path);

  void Write(Level level,
             const std::string& message,
             const char* source_file,
             int source_line) const;

  static Level ParseLevel(const std::string& level_name);
  static std::string LevelToString(Level level);
  static EffectiveConfig ResolveConfig(const Options& options);
  static std::filesystem::path ResolveLogFilePath(const std::string& module_name,
                                                  const EffectiveConfig& config);
  static std::string ModuleNameFromSource(const std::string& module_name,
                                          const char* source_file,
                                          int source_line,
                                          bool show_line_info,
                                          std::string* location_suffix);

  std::string module_name_;
  EffectiveConfig config_;
  std::filesystem::path log_file_path_;

  static std::mutex config_mutex_;
  static std::mutex io_mutex_;
  static EffectiveConfig default_config_;
  static std::optional<bool> global_enabled_;
  static std::optional<bool> global_unique_file_log_;
  static std::string global_unique_filename_;
};

Logger MakeLogger(const std::string& module_name,
                  const Logger::Options& options = Logger::Options{});

#define AIPARA_LOG_DEBUG(logger, message) \
  (logger).Debug((message), __FILE__, __LINE__)
#define AIPARA_LOG_INFO(logger, message) \
  (logger).Info((message), __FILE__, __LINE__)
#define AIPARA_LOG_WARN(logger, message) \
  (logger).Warn((message), __FILE__, __LINE__)
#define AIPARA_LOG_ERROR(logger, message) \
  (logger).Error((message), __FILE__, __LINE__)

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_COMMON_LOGGER_H_
