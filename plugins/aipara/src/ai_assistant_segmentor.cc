#include "ai_assistant_segmentor.h"

#include <algorithm>
#include <utility>

#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>

namespace rime::aipara {

AiAssistantSegmentor::AiAssistantSegmentor(const Ticket& ticket)
    : Segmentor(ticket), logger_(MakeLogger("ai_assistant_segmentor")) {}

bool AiAssistantSegmentor::Proceed(Segmentation* segmentation) {
  if (!segmentation || !engine_) {
    return true;
  }

  EnsureConfigLoaded();

  Context* context = engine_->context();
  if (!context) {
    return true;
  }

  UpdateKeepInputProperty(context);

  if (!enabled_) {
    return true;
  }

  const std::string& segmentation_input = segmentation->input();
  const size_t confirmed_pos = segmentation->GetConfirmedPosition();
  const size_t current_start = segmentation->GetCurrentStartPosition();
  const size_t current_end = segmentation->GetCurrentEndPosition();
  const std::string ai_context = context->get_property("current_ai_context");

  if (HandleClearHistoryShortcut(segmentation, ai_context, segmentation_input,
                                 current_start, current_end)) {
    return false;
  }

  if (confirmed_pos != 0 || current_start != 0) {
    return true;
  }

  if (HandleReplyInput(segmentation, segmentation_input)) {
    return false;
  }

  if (HandlePromptSegment(segmentation, segmentation_input)) {
    return false;
  }

  bool should_stop = false;
  if (HandleChatTrigger(segmentation, context, segmentation_input,
                        &should_stop)) {
    return !should_stop;
  }

  return true;
}

void AiAssistantSegmentor::EnsureConfigLoaded() {
  if (!engine_) {
    ResetConfigCaches();
    config_loaded_ = false;
    last_schema_id_.clear();
    return;
  }

  Schema* schema = engine_->schema();
  if (!schema) {
    ResetConfigCaches();
    config_loaded_ = false;
    last_schema_id_.clear();
    return;
  }

  Config* config = schema->config();
  const std::string schema_id = schema->schema_id();

  if (!config_loaded_ || schema_id != last_schema_id_) {
    last_schema_id_ = schema_id;
    LoadConfig(config);
    config_loaded_ = (config != nullptr);
  }
}

void AiAssistantSegmentor::UpdateCurrentConfig(Config* config) {
  LoadConfig(config);
  config_loaded_ = (config != nullptr);
  last_schema_id_.clear();
}

void AiAssistantSegmentor::ResetConfigCaches() {
  enabled_ = false;
  keep_input_uncommit_ = false;
  behavior_ = AiAssistantSegmentorBehavior{};
  chat_triggers_.clear();
  reply_messages_preedits_.clear();
  reply_tags_.clear();
  chat_names_.clear();
  clean_prefix_to_trigger_.clear();
  reply_inputs_to_trigger_.clear();
  chat_triggers_reverse_.clear();
}

void AiAssistantSegmentor::LoadConfig(Config* config) {
  ResetConfigCaches();
  if (!config) {
    return;
  }

  bool enabled = false;
  if (config->GetBool("ai_assistant/enabled", &enabled)) {
    enabled_ = enabled;
  }

  bool keep_input_uncommit = false;
  if (config->GetBool("translator/keep_input_uncommit",
                      &keep_input_uncommit)) {
    keep_input_uncommit_ = keep_input_uncommit;
  }

  config->GetBool("ai_assistant/behavior/commit_question",
                  &behavior_.commit_question);
  config->GetBool("ai_assistant/behavior/auto_commit_reply",
                  &behavior_.auto_commit_reply);
  config->GetBool("ai_assistant/behavior/clipboard_mode",
                  &behavior_.clipboard_mode);
  config->GetString("ai_assistant/behavior/prompt_chat",
                    &behavior_.prompt_chat);

  if (an<ConfigMap> prompts = config->GetMap("ai_assistant/ai_prompts")) {
    for (auto it = prompts->begin(); it != prompts->end(); ++it) {
      const std::string& trigger_name = it->first;
      const std::string base_path =
          "ai_assistant/ai_prompts/" + trigger_name;

      std::string trigger_value;
      if (config->GetString(base_path + "/chat_triggers", &trigger_value) &&
          !trigger_value.empty()) {
        chat_triggers_[trigger_name] = trigger_value;
        chat_triggers_reverse_[trigger_value] = trigger_name;

        std::string clean_prefix = trigger_value;
        if (!clean_prefix.empty() && clean_prefix.back() == ':') {
          clean_prefix.pop_back();
        }

        TriggerMetadata metadata;
        metadata.trigger_name = trigger_name;
        metadata.trigger_prefix = trigger_value;

        std::string chat_name;
        if (config->GetString(base_path + "/chat_names", &chat_name) &&
            !chat_name.empty()) {
          chat_names_[trigger_name] = chat_name;
          metadata.chat_name = chat_name;
        }

        clean_prefix_to_trigger_[clean_prefix] = std::move(metadata);
      } else {
        std::string chat_name;
        if (config->GetString(base_path + "/chat_names", &chat_name) &&
            !chat_name.empty()) {
          chat_names_[trigger_name] = chat_name;
        }
      }

      std::string reply_message;
      if (config->GetString(base_path + "/reply_messages_preedits",
                            &reply_message) &&
          !reply_message.empty()) {
        reply_messages_preedits_[trigger_name] = reply_message;
        const std::string reply_input_key = trigger_name + "_reply:";
        reply_inputs_to_trigger_[reply_input_key] = trigger_name;
      }
    }
  }

  AIPARA_LOG_INFO(logger_,
                  "AI assistant segmentor config loaded. enabled=" +
                      std::string(enabled_ ? "true" : "false") +
                      ", triggers=" +
                      std::to_string(chat_triggers_.size()));
}

void AiAssistantSegmentor::UpdateKeepInputProperty(Context* context) const {
  if (!context || !keep_input_uncommit_) {
    return;
  }

  const std::string& input = context->input();
  if (input.size() > 8) {
    context->set_property("input_string", input);
  } else if (input.size() == 8) {
    const std::string cached = context->get_property("input_string");
    if (cached.size() == 9) {
      context->set_property("input_string", "");
    }
  }
}

bool AiAssistantSegmentor::HandleClearHistoryShortcut(
    Segmentation* segmentation,
    const std::string& ai_context,
    const std::string& segmentation_input,
    size_t current_start,
    size_t current_end) const {
  if (!segmentation || ai_context.empty()) {
    return false;
  }

  if (segmentation->size() == 2 && current_start == 3 && current_end == 3 &&
      EndsWith(segmentation_input, ":c")) {
    Segment& last_segment = segmentation->back();
    last_segment.tags.clear();
    last_segment.tags.insert("clear_chat_history");
    last_segment.end += 1;
    last_segment.length = last_segment.end - last_segment.start;
    return true;
  }

  return false;
}

bool AiAssistantSegmentor::HandleReplyInput(
    Segmentation* segmentation,
    const std::string& segmentation_input) const {
  auto it = reply_inputs_to_trigger_.find(segmentation_input);
  if (it == reply_inputs_to_trigger_.end()) {
    return false;
  }

  Segment reply_segment(0, static_cast<int>(segmentation_input.length()));
  reply_segment.tags.insert(it->second + "_reply");
  reply_segment.tags.insert("ai_reply");

  segmentation->Reset(0);
  if (!segmentation->AddSegment(reply_segment)) {
    return false;
  }

  return true;
}

bool AiAssistantSegmentor::HandlePromptSegment(
    Segmentation* segmentation,
    const std::string& segmentation_input) const {
  if (behavior_.prompt_chat.empty() ||
      segmentation_input != behavior_.prompt_chat) {
    return false;
  }

  Segment prompt_segment(0, static_cast<int>(behavior_.prompt_chat.size()));
  prompt_segment.tags.insert("ai_prompt");
  prompt_segment.tags.insert("abc");

  segmentation->Reset(0);
  if (!segmentation->AddSegment(prompt_segment)) {
    return false;
  }

  return true;
}

bool AiAssistantSegmentor::HandleChatTrigger(
    Segmentation* segmentation,
    Context* context,
    const std::string& segmentation_input,
    bool* should_stop) const {
  if (!segmentation || !context || segmentation->size() >= 2) {
    return false;
  }

  std::string matched_prefix;
  std::string matched_trigger_name;
  bool full_match = false;

  for (const auto& entry : chat_triggers_reverse_) {
    const std::string& prefix = entry.first;
    if (segmentation_input.size() < prefix.size()) {
      continue;
    }
    if (segmentation_input.compare(0, prefix.size(), prefix) == 0) {
      matched_prefix = prefix;
      matched_trigger_name = entry.second;
      full_match = segmentation_input.size() == prefix.size();
      break;
    }
  }

  if (matched_trigger_name.empty()) {
    return false;
  }

  Segment ai_segment(0, static_cast<int>(matched_prefix.size()));
  ai_segment.tags.insert(matched_trigger_name);
  ai_segment.tags.insert("ai_talk");

  segmentation->Reset(0);
  if (!segmentation->AddSegment(ai_segment)) {
    return false;
  }

  context->set_property("current_ai_context", matched_trigger_name);

  if (full_match) {
    if (should_stop) {
      *should_stop = true;
    }
    return true;
  }

  if (!segmentation->Forward()) {
    return true;
  }

  const size_t next_start = segmentation->GetCurrentStartPosition();
  const size_t next_end = segmentation->GetCurrentEndPosition();
  if (HandleClearHistoryShortcut(segmentation, matched_trigger_name,
                                 segmentation_input, next_start, next_end)) {
    if (should_stop) {
      *should_stop = true;
    }
  }

  return true;
}

bool AiAssistantSegmentor::EndsWith(const std::string& value,
                                    const std::string& suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

}  // namespace rime::aipara
