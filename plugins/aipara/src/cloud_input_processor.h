#ifndef PLUGINS_AIPARA_SRC_CLOUD_INPUT_PROCESSOR_H_
#define PLUGINS_AIPARA_SRC_CLOUD_INPUT_PROCESSOR_H_

#include <rime/common.h>
#include <rime/processor.h>

#include <string>
#include <unordered_map>

namespace rime {
class Config;
class KeyEvent;
class Schema;
}  // namespace rime

namespace rime::aipara {

class TcpSocketSync;

struct AiAssistantBehavior {
  bool commit_question = false;
  bool strip_chat_prefix = false;
  bool add_reply_prefix = false;
  bool auto_commit_reply = false;
  bool clipboard_mode = false;
  std::string prompt_chat;
  std::string auto_commit_reply_send_key;
  std::string after_question_send_key;
};

struct AiAssistantConfig {
  bool enabled = false;
  AiAssistantBehavior behavior;
  std::unordered_map<std::string, std::string> chat_triggers;
  std::unordered_map<std::string, std::string> chat_names;
  std::unordered_map<std::string, std::string> reply_messages_preedits;
  std::unordered_map<std::string, std::string> prefix_to_reply;
};

class CloudInputProcessor : public Processor {
 public:
  explicit CloudInputProcessor(const Ticket& ticket);
  ~CloudInputProcessor() override;

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

  void UpdateCurrentConfig(Config* config);
  void UpdateProperty(const std::string& property_name,
                      const std::string& property_value);

  void AttachTcpSocketSync(TcpSocketSync* sync);

 private:
  void EnsureConfigLoaded(const Schema* schema);
  void RefreshAiPrompts(Config* config);

  std::string delimiter_;
  std::string cloud_convert_symbol_;
  std::string english_mode_symbol_;
  std::string rawenglish_delimiter_after_;
  std::string rawenglish_delimiter_before_;

  AiAssistantConfig ai_assistant_config_;

  std::string last_schema_id_;
  bool config_initialized_ = false;

  TcpSocketSync* tcp_socket_sync_ = nullptr;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_CLOUD_INPUT_PROCESSOR_H_
