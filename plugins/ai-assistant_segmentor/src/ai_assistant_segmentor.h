#ifndef RIME_PLUGINS_AI_ASSISTANT_SEGMENTOR_H_
#define RIME_PLUGINS_AI_ASSISTANT_SEGMENTOR_H_

#include <rime/segmentor.h>

#include <string>
#include <unordered_map>

namespace rime {

class Config;
class Context;
class Segmentation;

class AiAssistantSegmentor : public Segmentor {
 public:
  explicit AiAssistantSegmentor(const Ticket& ticket);

  bool Proceed(Segmentation* segmentation) override;

 private:
  struct BehaviorConfig {
    bool commit_question = false;
    bool auto_commit_reply = false;
    bool clipboard_mode = false;
    std::string prompt_chat;
  };

  struct TriggerMetadata {
    std::string trigger_name;
    std::string trigger_prefix;
    std::string chat_name;
  };

  void EnsureConfigLoaded();
  void LoadConfig(Config* config);
  void ResetConfigCaches();
  void UpdateKeepInputProperty(Context* context) const;
  bool HandleClearHistoryShortcut(Segmentation* segmentation,
                                  const std::string& ai_context,
                                  const std::string& segmentation_input,
                                  size_t current_start,
                                  size_t current_end) const;
  bool HandleReplyInput(Segmentation* segmentation,
                        const std::string& segmentation_input) const;
  bool HandlePromptSegment(Segmentation* segmentation,
                           const std::string& segmentation_input) const;
  bool HandleChatTrigger(Segmentation* segmentation,
                         Context* context,
                         const std::string& segmentation_input,
                         bool* should_stop) const;
  static bool EndsWith(const std::string& value, const std::string& suffix);

  bool config_loaded_ = false;
  std::string last_schema_id_;

  bool enabled_ = false;
  bool keep_input_uncommit_ = false;
  BehaviorConfig behavior_;

  std::unordered_map<std::string, std::string> chat_triggers_;
  std::unordered_map<std::string, std::string> chat_triggers_reverse_;
  std::unordered_map<std::string, std::string> reply_messages_preedits_;
  std::unordered_map<std::string, std::string> reply_tags_;
  std::unordered_map<std::string, std::string> chat_names_;
  std::unordered_map<std::string, TriggerMetadata> clean_prefix_to_trigger_;
  std::unordered_map<std::string, std::string> reply_inputs_to_trigger_;
};

}  // namespace rime

#endif  // RIME_PLUGINS_AI_ASSISTANT_SEGMENTOR_H_
