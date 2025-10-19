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

  // 确保配置加载，这会读取当前 schema（方案）对应的设置。
  EnsureConfigLoaded();

  Context* context = engine_->context();
  if (!context) {
    return true;
  }

  // 根据配置决定是否保留用户输入（类似自动保存草稿）。
  UpdateKeepInputProperty(context);

  // 功能总开关，如果没启用就直接返回 true，表示“继续走默认流程”。
  if (!enabled_) {
    return true;
  }

  // 从 segmentation 和 context 中提取当前输入的状态。
  const std::string& segmentation_input = segmentation->input();
  const size_t confirmed_pos = segmentation->GetConfirmedPosition();
  const size_t current_start = segmentation->GetCurrentStartPosition();
  const size_t current_end = segmentation->GetCurrentEndPosition();
  const std::string ai_context = context->get_property("current_ai_context");

  // 检查是否触发“清空历史”快捷键。
  if (HandleClearHistoryShortcut(segmentation, ai_context, segmentation_input,
                                 current_start, current_end)) {
    return false;
  }

  // 如果用户已经确认过前缀，就不再处理，避免破坏正常输入流程。
  if (confirmed_pos != 0 || current_start != 0) {
    return true;
  }

  // 检查是否输入了 AI 回复的触发指令。
  if (HandleReplyInput(segmentation, segmentation_input)) {
    return false;
  }

  // 检查是否输入了特定的 prompt（提示词）。
  if (HandlePromptSegment(segmentation, segmentation_input)) {
    return false;
  }

  // 处理聊天触发器，should_stop 用来告诉调用者是否需要停止后续分段。
  bool should_stop = false;
  if (HandleChatTrigger(segmentation, context, segmentation_input,
                        &should_stop)) {
    return !should_stop;
  }

  return true;
}

void AiAssistantSegmentor::EnsureConfigLoaded() {
  // engine_ 可能为空（比如 Segmentor 尚未绑定到 Engine），需要判空。
  if (!engine_) {
    ResetConfigCaches();
    config_loaded_ = false;
    last_schema_id_.clear();
    return;
  }

  Schema* schema = engine_->schema();
  if (!schema) {
    // 如果没有 schema，也要清空缓存，避免使用旧配置。
    ResetConfigCaches();
    config_loaded_ = false;
    last_schema_id_.clear();
    return;
  }

  Config* config = schema->config();
  const std::string schema_id = schema->schema_id();

  // 当第一次加载或 schema 切换时，重新读取配置。
  if (!config_loaded_ || schema_id != last_schema_id_) {
    last_schema_id_ = schema_id;
    LoadConfig(config);
    config_loaded_ = (config != nullptr);
  }
}

void AiAssistantSegmentor::UpdateCurrentConfig(Config* config) {
  // 手动更新配置时也重用 LoadConfig，保持逻辑一致。
  LoadConfig(config);
  config_loaded_ = (config != nullptr);
  last_schema_id_.clear();
}

void AiAssistantSegmentor::ResetConfigCaches() {
  // 恢复所有状态到默认值，避免旧数据干扰。
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
    // config 可能为空指针（类似 Python 的 None），直接返回。
    return;
  }

  // Rime 的 Config::GetBool/GetString 会把结果写入传入的指针。
  // 类比 Python 的 config.get("key", default)。
  bool enabled = false;
  if (config->GetBool("ai_assistant/enabled", &enabled)) {
    enabled_ = enabled;
  }

  bool keep_input_uncommit = false;
  if (config->GetBool("translator/keep_input_uncommit",
                      &keep_input_uncommit)) {
    keep_input_uncommit_ = keep_input_uncommit;
  }

  // behavior_ 包含多个布尔开关和一个字符串，全部从配置读取。
  config->GetBool("ai_assistant/behavior/commit_question",
                  &behavior_.commit_question);
  config->GetBool("ai_assistant/behavior/auto_commit_reply",
                  &behavior_.auto_commit_reply);
  config->GetBool("ai_assistant/behavior/clipboard_mode",
                  &behavior_.clipboard_mode);
  config->GetString("ai_assistant/behavior/prompt_chat",
                    &behavior_.prompt_chat);

  // an<ConfigMap> 是 Rime 的模板辅助，用来尝试把节点转成键值表。
  // for 循环遍历所有触发器配置，类似 Python for key, value in dict.items()。
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

        // clean_prefix 去掉末尾的冒号，方便后续匹配。
        std::string clean_prefix = trigger_value;
        if (!clean_prefix.empty() && clean_prefix.back() == ':') {
          clean_prefix.pop_back();
        }

        TriggerMetadata metadata;
        metadata.trigger_name = trigger_name;
        metadata.trigger_prefix = trigger_value;

        // 读取展示名称，若配置存在则保存。
        std::string chat_name;
        if (config->GetString(base_path + "/chat_names", &chat_name) &&
            !chat_name.empty()) {
          chat_names_[trigger_name] = chat_name;
          metadata.chat_name = chat_name;
        }

        // 使用 std::move 把 metadata 填进 map，避免额外复制。
        clean_prefix_to_trigger_[clean_prefix] = std::move(metadata);
      } else {
        // 如果没有 chat_triggers，但有 chat_name，同样记录下来。
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
        // reply_messages_preedits_ 用于显示预编辑内容，key 是 trigger_name。
        reply_messages_preedits_[trigger_name] = reply_message;
        // 构造一个特殊的输入 key，用来匹配“xx_reply:”。
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
  // context->input() 返回当前输入串。这里的逻辑是：当长度>8时缓存，当正好=8时根据缓存情况清理。
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
    const std::string& segmentation_input) const {
  // 在 reply_inputs_to_trigger_ 里查找是否存在匹配的“触发器回复”关键字。
  auto it = reply_inputs_to_trigger_.find(segmentation_input);
  if (it == reply_inputs_to_trigger_.end()) {
    return false;
  }

  // Segment 是 Rime 的数据结构，构造函数参数是 (start, end)。
  // static_cast<int> 强制类型转换，避免 size_t 转 int 的编译警告。
  Segment reply_segment(0, static_cast<int>(segmentation_input.length()));
  reply_segment.tags.insert(it->second + "_reply");
  reply_segment.tags.insert("ai_reply");

  // Reset(0) 清空所有已有分段，从头重新开始。
  segmentation->Reset(0);
  if (!segmentation->AddSegment(reply_segment)) {
    return false;
  }

  return true;
}

bool AiAssistantSegmentor::HandlePromptSegment(
    Segmentation* segmentation,
    const std::string& segmentation_input) const {
  // 如果没有配置 prompt_chat 或输入不匹配，则直接返回。
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
  // segmentation->size() >= 2 表示已经存在多个片段，就不重复处理。
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
    // compare(0, prefix.size(), prefix) 判断 segmentation_input 是否以 prefix 开头。
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

  // 构造新的 Segment，标记对应的触发器名称和 AI 对话标签。
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
