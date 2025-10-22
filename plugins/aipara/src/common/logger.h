#ifndef PLUGINS_AIPARA_SRC_COMMON_LOGGER_H_
#define PLUGINS_AIPARA_SRC_COMMON_LOGGER_H_

// C++ 的头文件一般只负责声明（告诉编译器“有这些东西”），真正的实现放在 .cc/.cpp 文件里。
// 这里用的都是标准库提供的头文件，分别用于文件路径处理、线程锁、可选值和字符串。
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

// 命名空间 namespace 用来给代码分组，避免名字冲突。
// 语法 rime::aipara 表示“在 rime 命名空间里面的 aipara 子命名空间”。
namespace rime::aipara {

// Logger 是一个类（可以理解成“自定义的数据类型”），用来封装日志相关的功能。
// Python 里我们可能会写一个对象负责输出日志；C++ 用 class 实现类似的概念。
class Logger {
 public:
  // struct 和 class 类似，区别是 struct 默认成员是 public。
  // Options 用来收集从配置文件或外部传进来的“可选”设置，每个成员用 std::optional 包起来，
  // 表示“可能有值，也可能没值”。类似 Python 里的 None。
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

  // 静态成员函数（static）属于类本身，而不是某个具体对象。
  // 调用方式类似 Python 里的类方法：Logger::Create(...)
  static Logger Create(const std::string& module_name,
                       const Options& options = Options{});

  // 设置或获取全局默认配置。因为是 static，所以所有 Logger 实例共享同一份默认值。
  static void SetDefaultOptions(const Options& options);
  static Options GetDefaultOptions();

  // 一系列全局开关，会影响后续创建出的 Logger。
  // std::optional<bool> 在这里表示“三态逻辑”：true / false / 不覆盖（std::nullopt）。
  static void SetGlobalEnabled(std::optional<bool> enabled);
  static void SetGlobalUniqueFileLog(std::optional<bool> enabled,
                                     const std::string& filename = "");
  static void SetConsoleOutput(bool enabled);
  static void SetLogLevel(const std::string& level);
  static void SetShowLineInfo(bool enabled);
  static void SetLogDir(const std::string& log_dir);

  // 清空日志文件内容。注意这里是 const 成员函数，
  // 表示调用它不会修改对象的“可见状态”（类似 Python 成员方法里不写 self.x = ...）。
  void Clear() const;

  // 四个写日志的方法，对应不同级别。
  // const char* 表示 C 风格字符串指针，这里用于记录源文件名；int 记录行号。
  // 默认参数（= nullptr、= 0）表示调用者可以省略这些参数。
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
  // enum class 是“强类型枚举”，用于限定日志级别的取值范围。
  // 和 Python Enum 类似，不过在 C++ 中枚举值需要加作用域（比如 Level::kDebug）。
  enum class Level {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
  };

  // EffectiveConfig 存储最终生效的配置。
  // 默认值直接写在成员后面，相当于 Python dataclass 的默认字段。
  struct EffectiveConfig {
    bool enabled = true;
    std::filesystem::path log_dir = "/Users/yangxinyi/Library/Rime/log_cpp/";
    std::string timestamp_format = "%Y-%m-%d %H:%M:%S";
    bool unique_file_log = false;
    std::string unique_file_log_file = "all_modules.log";
    bool console_output = true;
    Level min_level = Level::kDebug;
    bool show_line_info = true;
  };

  // 构造函数，使用初始化列表（冒号后面那段）依次给成员赋值。
  // 这里参数是按值传进来，再用 std::move 转成右值，避免不必要的复制。
  Logger(std::string module_name,
         EffectiveConfig config,
         std::filesystem::path log_file_path);

  // 内部实际写日志的函数，被 Debug/Info/Warn/Error 调用。
  // const 成员函数说明不会修改对象成员（逻辑上是只读操作）。
  void Write(Level level,
             const std::string& message,
             const char* source_file,
             int source_line) const;

  // 以下都是辅助的静态工具函数，不依赖特定对象。
  // 例如 ParseLevel 把字符串转换成枚举值，类似 Python 里的 classmethod。
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

  // 以下是类的成员变量。命名末尾带下划线是一种常见约定。
  std::string module_name_;
  EffectiveConfig config_;
  std::filesystem::path log_file_path_;

  // static 成员在所有 Logger 实例间共享。
  // std::mutex 用于线程同步，防止多线程同时修改配置或写文件时出现数据竞争。
  static std::mutex config_mutex_;
  static std::mutex io_mutex_;
  static EffectiveConfig default_config_;
  static std::optional<bool> global_enabled_;
  static std::optional<bool> global_unique_file_log_;
  static std::string global_unique_filename_;
};

// 辅助函数，返回一个 Logger 对象。相当于给 Create 取了个更短的名字。
Logger MakeLogger(const std::string& module_name,
                  const Logger::Options& options = Logger::Options{});

// 下面定义了四个宏，用来自动把源文件名和行号传进日志函数。
// 宏在编译前进行文本替换，__FILE__/__LINE__ 是编译器内置宏，能自动展开成当前文件名和行号。
// 宏过长或跨行时使用反斜杠续行。
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
