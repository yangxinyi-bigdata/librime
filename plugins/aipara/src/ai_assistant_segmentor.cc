#include "ai_assistant_segmentor.h"

#include <rime/config.h>
#include <rime/segmentation.h>

namespace rime::aipara {

AiAssistantSegmentor::AiAssistantSegmentor(const Ticket& ticket)
    : Segmentor(ticket) {}

bool AiAssistantSegmentor::Proceed(Segmentation*) {
  // TODO: implement AI assistant segmentation.
  return false;
}

void AiAssistantSegmentor::UpdateCurrentConfig(Config* config) {
  if (!config) {
    return;
  }

  bool enabled = false;
  if (config->GetBool("ai_assistant/enabled", &enabled)) {
    enabled_ = enabled;
  } else {
    enabled_ = false;
  }

  config->GetBool("translator/keep_input_uncommit", &keep_input_uncommit_);

  config->GetBool("ai_assistant/behavior/commit_question",
                  &behavior_.commit_question);
  config->GetBool("ai_assistant/behavior/auto_commit_reply",
                  &behavior_.auto_commit_reply);
  config->GetBool("ai_assistant/behavior/clipboard_mode",
                  &behavior_.clipboard_mode);

  config->GetString("ai_assistant/behavior/prompt_chat",
                    &behavior_.prompt_chat);

  chat_triggers_.clear();
  reply_messages_preedits_.clear();
  reply_tags_.clear();
  chat_names_.clear();
  clean_prefix_to_trigger_.clear();
  reply_inputs_to_trigger_.clear();
  chat_triggers_reverse_.clear();

  // TODO: populate prompt tables from configuration.
}

}  // namespace rime::aipara
