#ifndef PLUGINS_AIPARA_SRC_AIPARA_LOGGER_H_
#define PLUGINS_AIPARA_SRC_AIPARA_LOGGER_H_

#include <rime/common.h>
#include <string>

namespace rime::aipara {

// Lightweight logger facade that mirrors the Lua logger module interface.
class AiparaLogger {
 public:
  explicit AiparaLogger(string name);

  void Enable(bool enabled);
  void SetUniqueFileLog(bool enabled);
  void SetLogLevel(string level);

  void Clear();

  void Debug(const string& message);
  void Info(const string& message);
  void Warn(const string& message);
  void Error(const string& message);

 private:
  string name_;
  bool enabled_ = true;
  bool unique_file_log_ = false;
  string log_level_ = "INFO";
};

AiparaLogger MakeLogger(const string& name);

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AIPARA_LOGGER_H_
