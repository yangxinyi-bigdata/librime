#include "aipara_logger.h"

#include <rime/common.h>
#include <rime/service.h>

namespace rime::aipara {

AiparaLogger::AiparaLogger(string name) : name_(std::move(name)) {}

void AiparaLogger::Enable(bool enabled) {
  enabled_ = enabled;
}

void AiparaLogger::SetUniqueFileLog(bool enabled) {
  unique_file_log_ = enabled;
}

void AiparaLogger::SetLogLevel(string level) {
  log_level_ = std::move(level);
}

void AiparaLogger::Clear() {
  // TODO: integrate with persistent logging backend.
}

void AiparaLogger::Debug(const string& message) {
  if (!enabled_) {
    return;
  }
  DLOG(INFO) << "[" << name_ << "] " << message;
}

void AiparaLogger::Info(const string& message) {
  if (!enabled_) {
    return;
  }
  LOG(INFO) << "[" << name_ << "] " << message;
}

void AiparaLogger::Warn(const string& message) {
  if (!enabled_) {
    return;
  }
  LOG(WARNING) << "[" << name_ << "] " << message;
}

void AiparaLogger::Error(const string& message) {
  if (!enabled_) {
    return;
  }
  LOG(ERROR) << "[" << name_ << "] " << message;
}

AiparaLogger MakeLogger(const string& name) {
  return AiparaLogger(name);
}

}  // namespace rime::aipara
