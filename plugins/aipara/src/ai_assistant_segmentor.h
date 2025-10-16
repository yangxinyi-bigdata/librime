#ifndef PLUGINS_AIPARA_SRC_AI_ASSISTANT_SEGMENTOR_H_
#define PLUGINS_AIPARA_SRC_AI_ASSISTANT_SEGMENTOR_H_

#include <rime/common.h>
#include <rime/segmentor.h>

#include <string>
#include <unordered_map>

namespace rime {
class Config;
class Segmentation;
}  // namespace rime

namespace rime::aipara {

struct AiAssistantSegmentorBehavior {
  bool commit_question = false;
  bool auto_commit_reply = false;
  bool clipboard_mode = false;
  std::string prompt_chat;
};

class AiAssistantSegmentor : public Segmentor {
 public:
  explicit AiAssistantSegmentor(const Ticket& ticket);

  bool Proceed(Segmentation* segmentation) override;

  void UpdateCurrentConfig(Config* config);

 private:
  bool enabled_ = false;
  bool keep_input_uncommit_ = false;
  AiAssistantSegmentorBehavior behavior_;

  std::unordered_map<std::string, std::string> chat_triggers_;
  std::unordered_map<std::string, std::string> reply_messages_preedits_;
  std::unordered_map<std::string, std::string> reply_tags_;
  std::unordered_map<std::string, std::string> chat_names_;
  std::unordered_map<std::string, std::string> clean_prefix_to_trigger_;
  std::unordered_map<std::string, std::string> reply_inputs_to_trigger_;
  std::unordered_map<std::string, std::string> chat_triggers_reverse_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AI_ASSISTANT_SEGMENTOR_H_
