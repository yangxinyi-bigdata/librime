#ifndef PLUGINS_AIPARA_SRC_CLOUD_INPUT_PROCESSOR_H_
#define PLUGINS_AIPARA_SRC_CLOUD_INPUT_PROCESSOR_H_

#include <rime/common.h>
#include <rime/processor.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/logger.h"

namespace rime {
class Config;
class Context;
class KeyEvent;
class Schema;
}  // namespace rime

namespace rime::aipara {

class TcpZmq;

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
  int page_size = 5;
  std::string alternative_select_keys = "1234567890";
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

  void UpdateProperty(const std::string& property_name,
                      const std::string& property_value);

  void AttachTcpZmq(TcpZmq* client);

 private:
  void ApplyPendingProperties(rime::Context* context);
  bool HandleShiftReleaseInterception(const std::string& key_repr,
                                      rime::Context* context);
  bool HandleControlF14(const std::string& key_repr,
                        rime::Context* context,
                        Config* config);
  bool HandleControlF13(const std::string& key_repr, rime::Context* context);
  ProcessResult HandleInterceptSelectKey(const std::string& key_repr,
                                         rime::Context* context,
                                         Config* config);
  ProcessResult HandleAiTalkSelection(const std::string& key_repr,
                                      rime::Context* context,
                                      Config* config);
  ProcessResult HandleRawEnglishInput(const KeyEvent& key_event,
                                      const std::string& key_repr,
                                      rime::Context* context);
  ProcessResult HandleCloudConvertTrigger(const KeyEvent& key_event,
                                          const std::string& key_repr,
                                          rime::Context* context,
                                          Config* config);
  void SetCloudConvertFlag(rime::Context* context,
                           Config* config) const;
  ProcessResult HandleAiCandidateCommit(const std::string& key_repr,
                                        const std::string& chat_trigger,
                                        rime::Context* context,
                                        Config* config);

  Logger logger_;
  std::unordered_map<std::string, std::string> pending_property_updates_;

  TcpZmq* tcp_zmq_ = nullptr;

  connection unhandled_key_connection_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_CLOUD_INPUT_PROCESSOR_H_
