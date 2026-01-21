#include "ai_assistant_segmentor.h"

// <algorithm> 提供 STL 算法，比如 std::equal。
// <utility> 主要提供 std::move/std::pair 等工具。
#include <algorithm>
#include <utility>

// 引入和 Rime 运行时交互所需的头文件，分别负责配置、上下文、引擎对象等。
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>

namespace rime::aipara {

AiAssistantSegmentor::AiAssistantSegmentor(const Ticket& ticket)
    : Segmentor(ticket), logger_(MakeLogger("ai_assistant_segmentor")) {}
// 上面是构造函数，冒号后的是“成员初始化列表”。
// 先调用基类构造函数 Segmentor(ticket)，再初始化 logger_。
// MakeLogger 返回一个 Logger 对象，用来记录调试信息。

bool AiAssistantSegmentor::Proceed(Segmentation* segmentation) {
  // Proceed 是 Segmentor 的核心回调。每当用户输入变化时，Rime 会调用它。
  // segmentation 指向当前的分段状态，engine_ 是基类提供的成员指针。
  if (!segmentation || !engine_) {
    return true;
  }

  Context* context = engine_->context();
  if (!context) {
    return true;
  }

  Config* config = ResolveConfig();

  bool keep_input_uncommit = false;
  bool enabled = false;
  AiAssistantSegmentorBehavior behavior;
  if (config) {
    config->GetBool("translator/keep_input_uncommit",
                    &keep_input_uncommit);
    config->GetBool("ai_assistant/enabled", &enabled);
    behavior = ReadBehavior(config);
  }

  // 根据配置决定是否保留用户输入（类似自动保存草稿）。
  UpdateKeepInputProperty(context, keep_input_uncommit);

  // 功能总开关，如果没启用就直接返回 true，表示“继续走默认流程”。
  if (!enabled) {
    return true;
  }

  // 从 segmentation 和 context 中提取当前输入的状态。
  const std::string& segmentation_input = segmentation->input();
  const size_t confirmed_pos = segmentation->GetConfirmedPosition();
  const size_t current_start = segmentation->GetCurrentStartPosition();
  const size_t current_end = segmentation->GetCurrentEndPosition();
  const std::string ai_context = context->get_property("current_ai_context");
  AIPARA_LOG_DEBUG(logger_,
                   "Segmentor Proceed input='" + segmentation_input +
                       "' confirmed_pos=" + std::to_string(confirmed_pos) +
                       " current_start=" + std::to_string(current_start) +
                       " current_end=" + std::to_string(current_end) +
                       " current_ai_context=" + ai_context);

  // 检查是否触发“清空历史”快捷键。
  if (HandleClearHistoryShortcut(segmentation, ai_context, segmentation_input,
                                 current_start, current_end)) {
    return false;
  }

  // 如果用户已经确认过前缀，就不再处理，避免破坏正常输入流程。
  if (confirmed_pos != 0 || current_start != 0) {
    AIPARA_LOG_DEBUG(logger_,
                     "Segmentor non-zero positions, try speech trigger after "
                     "AI prefix");
    bool should_stop = false;
    if (HandleSpeechTriggerAfterAiPrefix(segmentation, context,
                                         segmentation_input, current_start,
                                         config, &should_stop)) {
      AIPARA_LOG_INFO(
          logger_,
          "Segmentor added speech segment after AI prefix, should_stop=" +
              std::string(should_stop ? "true" : "false"));
      return !should_stop;
    }
    AIPARA_LOG_DEBUG(logger_,
                     "Segmentor speech-after-AI not handled");
    return true;
  }

  // 检查是否输入了 AI 回复的触发指令。
  if (HandleReplyInput(segmentation, segmentation_input, config)) {
    return false;
  }

  // 检查是否输入了特定的 prompt（提示词）。
  if (HandlePromptSegment(segmentation, segmentation_input, behavior)) {
    return false;
  }

  // 处理聊天触发器，should_stop 用来告诉调用者是否需要停止后续分段。
  bool should_stop = false;
  if (HandleChatTrigger(segmentation, context, segmentation_input, config,
                        &should_stop)) {
    return !should_stop;
  }

  return true;
}

Config* AiAssistantSegmentor::ResolveConfig() const {
  if (!engine_) {
    return nullptr;
  }
  if (auto* schema = engine_->schema()) {
    return schema->config();
  }
  return nullptr;
}

AiAssistantSegmentorBehavior AiAssistantSegmentor::ReadBehavior(
    Config* config) const {
  AiAssistantSegmentorBehavior behavior;
  if (!config) {
    return behavior;
  }
  config->GetBool("ai_assistant/behavior/commit_question",
                  &behavior.commit_question);
  config->GetBool("ai_assistant/behavior/auto_commit_reply",
                  &behavior.auto_commit_reply);
  config->GetBool("ai_assistant/behavior/clipboard_mode",
                  &behavior.clipboard_mode);
  config->GetString("ai_assistant/behavior/prompt_chat",
                    &behavior.prompt_chat);
  return behavior;
}

void AiAssistantSegmentor::UpdateKeepInputProperty(Context* context,
                                                   bool keep_input_uncommit) const {
  // context->input() 返回当前输入串。这里的逻辑是：当长度>8时缓存，当正好=8时根据缓存情况清理。
  if (!context || !keep_input_uncommit) {
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
  // segmentation 为空或当前上下文没有 AI 会话，就不处理。
  if (!segmentation || ai_context.empty()) {
    return false;
  }

  // 条件判断解释：
  // segmentation->size() == 2 说明当前有两段输入；
  // current_start/current_end == 3 表示光标位置；
  // EndsWith(segmentation_input, ":c") 检查输入是否以 ":c" 结尾。
  // 满足这些条件就把最后一个 Segment 标记成清空历史的指令。
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
    const std::string& segmentation_input,
    Config* config) const {
  if (!segmentation || !config) {
    return false;
  }

  if (segmentation_input.empty()) {
    return false;
  }

  if (an<ConfigMap> prompts = config->GetMap("ai_assistant/ai_prompts")) {
    for (auto it = prompts->begin(); it != prompts->end(); ++it) {
      const std::string& trigger_name = it->first;
      std::string reply_message;
      if (!config->GetString(
              "ai_assistant/ai_prompts/" + trigger_name +
                  "/reply_messages_preedits",
              &reply_message) ||
          reply_message.empty()) {
        continue;
      }
      const std::string reply_input_key = trigger_name + "_reply:";
      if (segmentation_input != reply_input_key) {
        continue;
      }

      Segment reply_segment(0, static_cast<int>(segmentation_input.length()));
      reply_segment.tags.insert(trigger_name + "_reply");
      reply_segment.tags.insert("ai_reply");

      segmentation->Reset(0);
      if (!segmentation->AddSegment(reply_segment)) {
        return false;
      }
      return true;
    }
  }

  return false;
}

bool AiAssistantSegmentor::HandlePromptSegment(
    Segmentation* segmentation,
    const std::string& segmentation_input,
    const AiAssistantSegmentorBehavior& behavior) const {
  // 如果没有配置 prompt_chat 或输入不匹配，则直接返回。
  if (behavior.prompt_chat.empty() ||
      segmentation_input != behavior.prompt_chat) {
    return false;
  }

  Segment prompt_segment(0, static_cast<int>(behavior.prompt_chat.size()));
  prompt_segment.tags.insert("ai_prompt");
  prompt_segment.tags.insert("abc");

  segmentation->Reset(0);
  if (!segmentation->AddSegment(prompt_segment)) {
    return false;
  }

  return true;
}

bool AiAssistantSegmentor::HandleSpeechTriggerAfterAiPrefix(
    Segmentation* segmentation,
    Context* context,
    const std::string& segmentation_input,
    size_t current_start,
    Config* config,
    bool* should_stop) const {
  if (!segmentation || !context || !config) {
    return false;
  }
  if (current_start == 0 || segmentation_input.empty()) {
    return false;
  }

  const std::string ai_context = context->get_property("current_ai_context");
  if (ai_context.empty()) {
    AIPARA_LOG_DEBUG(logger_, "SpeechAfterAi: empty current_ai_context");
    return false;
  }

  std::string ai_prefix;
  if (!config->GetString(
          "ai_assistant/ai_prompts/" + ai_context + "/chat_triggers",
          &ai_prefix) ||
      ai_prefix.empty()) {
    AIPARA_LOG_DEBUG(logger_, "SpeechAfterAi: missing ai_prefix for " +
                                  ai_context);
    return false;
  }

  std::string speech_trigger;
  if (!config->GetString("ai_assistant/speech_recognition/chat_triggers",
                        &speech_trigger) ||
      speech_trigger.empty()) {
    AIPARA_LOG_DEBUG(logger_, "SpeechAfterAi: missing speech_trigger");
    return false;
  }

  AIPARA_LOG_DEBUG(
      logger_,
      "SpeechAfterAi input='" + segmentation_input + "' ai_prefix='" +
          ai_prefix + "' speech_trigger='" + speech_trigger +
          "' current_start=" + std::to_string(current_start));

  if (segmentation_input.size() <
      current_start + speech_trigger.size()) {
    return false;
  }

  if (segmentation_input.compare(0, ai_prefix.size(), ai_prefix) != 0) {
    AIPARA_LOG_DEBUG(logger_, "SpeechAfterAi: prefix mismatch");
    return false;
  }

  if (segmentation_input.compare(current_start, speech_trigger.size(),
                                 speech_trigger) != 0) {
    AIPARA_LOG_DEBUG(logger_, "SpeechAfterAi: speech trigger mismatch");
    return false;
  }

  Segment ai_segment(0, static_cast<int>(ai_prefix.size()));
  ai_segment.tags.insert(ai_context);
  ai_segment.tags.insert("ai_talk");

  Segment speech_segment(static_cast<int>(current_start),
                         static_cast<int>(current_start +
                                          speech_trigger.size()));
  speech_segment.tags.insert("speech_recognition");

  segmentation->Reset(0);
  if (!segmentation->AddSegment(ai_segment)) {
    AIPARA_LOG_WARN(logger_, "SpeechAfterAi: failed to add ai_segment");
    return false;
  }
  if (!segmentation->Forward()) {
    AIPARA_LOG_WARN(logger_, "SpeechAfterAi: failed to forward after ai_segment");
    return false;
  }
  if (!segmentation->AddSegment(speech_segment)) {
    AIPARA_LOG_WARN(logger_, "SpeechAfterAi: failed to add speech_segment");
    return false;
  }
  AIPARA_LOG_INFO(logger_,
                  "SpeechAfterAi: added segments ai[0," +
                      std::to_string(ai_prefix.size()) + ") speech[" +
                      std::to_string(current_start) + "," +
                      std::to_string(current_start + speech_trigger.size()) +
                      ") total_segments=" +
                      std::to_string(segmentation->size()));

  if (segmentation_input.size() ==
      current_start + speech_trigger.size()) {
    if (should_stop) {
      *should_stop = true;
    }
  }
  return true;
}

bool AiAssistantSegmentor::HandleChatTrigger(
    Segmentation* segmentation,
    Context* context,
    const std::string& segmentation_input,
    Config* config,
    bool* should_stop) const {
  // segmentation->size() >= 2 表示已经存在多个片段，就不重复处理。
  if (!segmentation || !context || segmentation->size() >= 2 || !config) {
    return false;
  }

  std::string matched_prefix;
  std::string matched_trigger_name;
  bool full_match = false;

  size_t longest_prefix = 0;
  bool matched_speech = false;
  if (an<ConfigMap> prompts = config->GetMap("ai_assistant/ai_prompts")) {
    for (auto it = prompts->begin(); it != prompts->end(); ++it) {
      const std::string& trigger_name = it->first;
      std::string trigger_prefix;
      if (!config->GetString(
              "ai_assistant/ai_prompts/" + trigger_name + "/chat_triggers",
              &trigger_prefix) ||
          trigger_prefix.empty()) {
        continue;
      }
      if (segmentation_input.size() < trigger_prefix.size()) {
        continue;
      }
      if (segmentation_input.compare(0, trigger_prefix.size(),
                                     trigger_prefix) == 0 &&
          trigger_prefix.size() > longest_prefix) {
        matched_prefix = trigger_prefix;
        matched_trigger_name = trigger_name;
        longest_prefix = trigger_prefix.size();
        matched_speech = false;
      }
    }
  }

  std::string speech_trigger;
  if (config->GetString("ai_assistant/speech_recognition/chat_triggers",
                        &speech_trigger) &&
      !speech_trigger.empty()) {
    if (segmentation_input.size() >= speech_trigger.size() &&
        segmentation_input.compare(0, speech_trigger.size(),
                                   speech_trigger) == 0 &&
        speech_trigger.size() > longest_prefix) {
      matched_prefix = speech_trigger;
      matched_trigger_name = "speech_recognition";
      longest_prefix = speech_trigger.size();
      matched_speech = true;
    }
  }

  if (matched_trigger_name.empty()) {
    return false;
  }

  full_match = segmentation_input.size() == matched_prefix.size();
  AIPARA_LOG_DEBUG(
      logger_,
      "HandleChatTrigger input='" + segmentation_input +
          "' matched_prefix='" + matched_prefix +
          "' matched_trigger_name='" + matched_trigger_name +
          "' full_match=" + std::string(full_match ? "true" : "false") +
          " matched_speech=" + std::string(matched_speech ? "true" : "false"));

  // 构造新的 Segment，标记对应的触发器名称和 AI 对话标签。
  Segment ai_segment(0, static_cast<int>(matched_prefix.size()));
  if (matched_speech) {
    ai_segment.tags.insert("speech_recognition");
  } else {
    ai_segment.tags.insert(matched_trigger_name);
    ai_segment.tags.insert("ai_talk");
  }

  segmentation->Reset(0);
  if (!segmentation->AddSegment(ai_segment)) {
    return false;
  }

  if (!matched_speech) {
    context->set_property("current_ai_context", matched_trigger_name);
  }

  if (!matched_speech && full_match == false) {
    std::string speech_trigger;
    if (config->GetString("ai_assistant/speech_recognition/chat_triggers",
                          &speech_trigger) &&
        !speech_trigger.empty()) {
      const std::size_t speech_start = matched_prefix.size();
      if (segmentation_input.size() >=
              speech_start + speech_trigger.size() &&
          segmentation_input.compare(speech_start, speech_trigger.size(),
                                     speech_trigger) == 0) {
        if (!segmentation->Forward()) {
          AIPARA_LOG_WARN(logger_,
                          "HandleChatTrigger: failed to forward before speech");
          return true;
        }
        Segment speech_segment(static_cast<int>(speech_start),
                               static_cast<int>(speech_start +
                                                speech_trigger.size()));
        speech_segment.tags.insert("speech_recognition");
        if (!segmentation->AddSegment(speech_segment)) {
          AIPARA_LOG_WARN(logger_,
                          "HandleChatTrigger: failed to add speech segment");
          return false;
        }
        if (segmentation_input.size() ==
            speech_start + speech_trigger.size()) {
          if (should_stop) {
            *should_stop = true;
          }
        }
        return true;
      }
    }
  }

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
  // 在 AI 触发器匹配成功后，顺便检查有没有紧接着输入清空历史的指令。
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
  // std::equal 搭配反向迭代器（rbegin/rend）可以从尾部开始比较。
  // 注意：反向迭代器是 C++ 的常见难点，这里等价于 Python 的 value.endswith(suffix)。
  return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

}  // namespace rime::aipara
