#include "cloud_input_processor.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/config/config_types.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/text_formatting.h"
#include "common/tcp_zmq.h"

namespace rime::aipara {
namespace {

constexpr std::string_view kLoggerName = "cloud_input_processor";
constexpr std::string_view kWaitingMessage = "等待回复...";
constexpr std::string_view kCursorMarker = u8"‸";

struct Utf8Char {
  std::string text;
  uint32_t codepoint = 0;
};
// 将UTF-8编码的字符串解码为Unicode字符序列
// 参数: text - 要解码的UTF-8编码字符串
// 返回: 包含解码后字符的向量，每个字符包含原始UTF-8子串和对应的Unicode码点
std::vector<Utf8Char> DecodeUtf8(const std::string& text) {
  // 存储解码后的字符序列
  std::vector<Utf8Char> chars;
  // 当前处理位置索引
  std::size_t i = 0;

  // 遍历整个字符串
  while (i < text.size()) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t len = 1;
    if ((c & 0x80u) == 0) {
      len = 1;
    } else if ((c & 0xE0u) == 0xC0u) {
      len = 2;
    } else if ((c & 0xF0u) == 0xE0u) {
      len = 3;
    } else {
      len = 4;
    }
    if (i + len > text.size()) {
      len = text.size() - i;
    }
    uint32_t codepoint = 0;
    if (len == 1) {
      codepoint = c;
    } else if (len == 2) {
      codepoint = ((c & 0x1Fu) << 6) |
                  (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
    } else if (len == 3) {
      codepoint = ((c & 0x0Fu) << 12) |
                  ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                  (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
    } else {
      codepoint = ((c & 0x07u) << 18) |
                  ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
                  ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) |
                  (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
    }
    chars.push_back({text.substr(i, len), codepoint});
    i += len;
  }
  return chars;
}

bool IsChinese(uint32_t codepoint) {
  return (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
         (codepoint >= 0x3400 && codepoint <= 0x4DBF);
}

bool IsChinesePunctuation(uint32_t codepoint) {
  return (codepoint >= 0x3000 && codepoint <= 0x303F) ||
         (codepoint >= 0xFF00 && codepoint <= 0xFFEF);
}

std::string TrimRight(const std::string& text) {
  std::string result = text;
  while (!result.empty() &&
         std::isspace(static_cast<unsigned char>(result.back())) != 0) {
    result.pop_back();
  }
  return result;
}

std::string RemoveOneCharacterFromEnd(const std::string& text) {
  const auto chars = DecodeUtf8(text);
  if (chars.empty()) {
    return std::string();
  }
  std::size_t bytes = 0;
  for (std::size_t i = 0; i + 1 < chars.size(); ++i) {
    bytes += chars[i].text.size();
  }
  return text.substr(0, bytes);
}

std::string RemoveLastSyllableKeepDelimiter(const std::string& text,
                                            const std::string& delimiter) {
  if (delimiter.empty()) {
    return std::string();
  }
  const std::string trimmed = TrimRight(text);
  const std::size_t pos = trimmed.rfind(delimiter);
  if (pos == std::string::npos) {
    return std::string();
  }
  return text.substr(0, pos + delimiter.size());
}

std::string RemoveLastSyllableNoDelimiter(const std::string& text,
                                          const std::string& delimiter,
                                          const std::string& english_marker) {
  if (delimiter.empty()) {
    return std::string();
  }
  std::string trimmed = TrimRight(text);
  const std::size_t pos = trimmed.rfind(delimiter);
  if (pos != std::string::npos) {
    return text.substr(0, pos + delimiter.size());
  }
  if (!english_marker.empty()) {
    const std::size_t marker_pos = text.rfind(english_marker + english_marker);
    if (marker_pos != std::string::npos) {
      return text.substr(0, marker_pos + english_marker.size());
    }
  }
  return std::string();
}

std::string NormalizeChatPrefix(const std::string& value) {
  if (value.empty()) {
    return value;
  }
  if (value.back() == ':') {
    return value.substr(0, value.size() - 1);
  }
  return value;
}

bool ContainsNewline(const std::string& text) {
  return text.find('\n') != std::string::npos;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), text.begin());
}

bool IsShiftRelease(const std::string& key_repr) {
  return key_repr == "Release+Shift_L" || key_repr == "Release+Shift_R";
}

bool IsSelectKey(const std::string& key_repr,
                 const AiAssistantConfig& config,
                 std::size_t* index) {
  if (key_repr == "space") {
    if (index) {
      *index = 0;
    }
    return true;
  }
  const std::size_t pos = config.alternative_select_keys.find(key_repr);
  if (pos == std::string::npos) {
    return false;
  }
  if (index) {
    *index = pos;
  }
  return true;
}

std::string MaybeStripPrefix(const std::string& text,
                             const std::string& prefix) {
  if (prefix.empty()) {
    return text;
  }
  if (StartsWith(text, prefix)) {
    return text.substr(prefix.size());
  }
  return text;
}

AiAssistantConfig LoadAiAssistantConfig(Config* config) {
  AiAssistantConfig result;
  if (!config) {
    result.page_size = 5;
    result.alternative_select_keys = "123456789";
    return result;
  }

  result.enabled = false;
  config->GetBool("ai_assistant/enabled", &result.enabled);

  AiAssistantBehavior behavior;
  config->GetBool("ai_assistant/behavior/commit_question",
                  &behavior.commit_question);
  config->GetBool("ai_assistant/behavior/strip_chat_prefix",
                  &behavior.strip_chat_prefix);
  config->GetBool("ai_assistant/behavior/add_reply_prefix",
                  &behavior.add_reply_prefix);
  config->GetBool("ai_assistant/behavior/auto_commit_reply",
                  &behavior.auto_commit_reply);
  config->GetBool("ai_assistant/behavior/clipboard_mode",
                  &behavior.clipboard_mode);
  config->GetString("ai_assistant/behavior/prompt_chat", &behavior.prompt_chat);
  config->GetString("ai_assistant/behavior/auto_commit_reply_send_key",
                    &behavior.auto_commit_reply_send_key);
  config->GetString("ai_assistant/behavior/after_question_send_key",
                    &behavior.after_question_send_key);
  result.behavior = behavior;

  if (auto prompts = config->GetMap("ai_assistant/ai_prompts")) {
    for (const auto& entry : *prompts) {
      const std::string& trigger_name = entry.first;

      std::string trigger_value;
      if (config->GetString(
              "ai_assistant/ai_prompts/" + trigger_name + "/chat_triggers",
              &trigger_value) &&
          !trigger_value.empty()) {
        result.chat_triggers[trigger_name] = trigger_value;
      }

      std::string chat_name;
      if (config->GetString(
              "ai_assistant/ai_prompts/" + trigger_name + "/chat_names",
              &chat_name) &&
          !chat_name.empty()) {
        result.chat_names[trigger_name] = chat_name;
      }

      std::string reply_preedit;
      if (config->GetString("ai_assistant/ai_prompts/" + trigger_name +
                                "/reply_messages_preedits",
                            &reply_preedit) &&
          !reply_preedit.empty()) {
        result.reply_messages_preedits[trigger_name] = reply_preedit;
      }
    }
  }

  for (const auto& [trigger_name, trigger_value] : result.chat_triggers) {
    auto it = result.reply_messages_preedits.find(trigger_name);
    if (it != result.reply_messages_preedits.end()) {
      result.prefix_to_reply[trigger_value] = it->second;
    }
  }

  result.page_size = 5;
  config->GetInt("menu/page_size", &result.page_size);
  result.alternative_select_keys = "123456789";
  config->GetString("menu/alternative_select_keys",
                    &result.alternative_select_keys);
  if (!result.alternative_select_keys.empty() && result.page_size > 0 &&
      static_cast<std::size_t>(result.page_size) <
          result.alternative_select_keys.size()) {
    result.alternative_select_keys = result.alternative_select_keys.substr(
        0, static_cast<std::size_t>(result.page_size));
  }

  return result;
}

}  // namespace

/**
 * @brief CloudInputProcessor 构造函数
 * 
 * 初始化云输入处理器，设置日志记录器并确保TCP客户端连接。
 * 
 * @param ticket Rime 引擎传递的票据，包含引擎和配置信息
 */
CloudInputProcessor::CloudInputProcessor(const Ticket& ticket)
    : Processor(ticket), logger_(MakeLogger(std::string(kLoggerName))) {
  // 清空日志记录器，确保没有残留的日志信息
  logger_.Clear();
  // 复用插件级共享的 TcpZmq 客户端，用于与云服务通信
  AttachTcpZmq(AcquireGlobalTcpZmq());
}

/**
 * @brief CloudInputProcessor 析构函数
 * 
 * 使用默认析构函数，由编译器自动生成。
 * 负责清理 CloudInputProcessor 实例占用的资源。
 */
CloudInputProcessor::~CloudInputProcessor() = default;

/**
 * @brief 处理键盘输入事件
 * 
 * 这是 CloudInputProcessor 的核心函数，负责处理各种键盘事件，
 * 包括特殊按键处理、云输入转换触发、AI 助手交互等功能。
 * 
 * @param key_event 键盘事件对象，包含按键信息和状态
 * @return ProcessResult 处理结果，表示事件是否被接受或忽略
 */
ProcessResult CloudInputProcessor::ProcessKeyEvent(const KeyEvent& key_event) {
  // 检查引擎是否已初始化，未初始化则无法处理事件
  if (!engine_) {
    return kNoop;
  }

  // 获取输入上下文，用于访问和修改输入状态
  Context* context = engine_->context();
  if (!context) {
    return kNoop;
  }

  // 获取配置对象，用于读取输入法配置
  Config* config = nullptr;
  if (const Schema* schema = engine_->schema()) {
    config = schema->config();
  }
  if (tcp_zmq_ && config) {
    tcp_zmq_->RefreshCurveConfig(config);
  }
  // 更新文本格式化模块的当前配置
  text_formatting::UpdateCurrentConfig(config);

  // 获取按键的字符串表示形式，用于后续处理
  const std::string key_repr = key_event.repr();

  // 处理按键释放事件
  if (key_event.release()) {
    // 检查是否需要拦截 Shift 键释放事件
    if (HandleShiftReleaseInterception(key_repr, context)) {
      return kAccepted;
    }
    return kNoop;
  }

  // 处理 Alt+F14 特殊组合键（AI 助手相关功能）
  if (HandleAltF14(key_repr, context, config)) {
    return kNoop;
  }

  // 应用待处理的属性更新
  ApplyPendingProperties(context);

  // 再次检查是否需要拦截 Shift 键释放事件（可能在属性更新后状态改变）
  if (HandleShiftReleaseInterception(key_repr, context)) {
    return kAccepted;
  }

  // 处理 Alt+F13 特殊组合键（云输入流相关功能）
  if (HandleAltF13(key_repr, context)) {
    return kAccepted;
  }

  // 如果当前不在编辑状态（没有正在输入的文本），则不处理
  if (!context->IsComposing()) {
    return kNoop;
  }

  // 处理选择键拦截（如空格键或数字键选择候选词）
  if (auto intercept = HandleInterceptSelectKey(key_repr, context, config);
      intercept != kNoop) {
    return intercept;
  }

  // 处理 AI 助手选择相关操作
  if (auto ai_result = HandleAiTalkSelection(key_repr, context, config);
      ai_result != kNoop) {
    return ai_result;
  }

  // 处理原始英文输入（直接输入英文而不转换）
  if (auto raw = HandleRawEnglishInput(key_event, key_repr, context);
      raw != kNoop) {
    return raw;
  }

  // 设置云转换标志，标记可能需要进行云转换
  SetCloudConvertFlag(context, config);

  // 处理云转换触发事件（如特定组合键触发云输入）
  if (auto convert =
          HandleCloudConvertTrigger(key_event, key_repr, context, config);
      convert != kNoop) {
    return convert;
  }

  // 默认情况下不处理该事件
  return kNoop;
}

void CloudInputProcessor::UpdateProperty(const std::string& property_name,
                                         const std::string& property_value) {
  pending_property_updates_[property_name] = property_value;
}

void CloudInputProcessor::AttachTcpZmq(TcpZmq* client) {
  tcp_zmq_ = client;
}

void CloudInputProcessor::ApplyPendingProperties(Context* context) {
  if (!context || pending_property_updates_.empty()) {
    return;
  }
  for (const auto& [name, value] : pending_property_updates_) {
    context->set_property(name, value);
  }
  pending_property_updates_.clear();
}

bool CloudInputProcessor::HandleShiftReleaseInterception(
    const std::string& key_repr,
    Context* context) {
  if (!context) {
    return false;
  }
  if (context->get_property("should_intercept_key_release") != "1") {
    return false;
  }
  if (!IsShiftRelease(key_repr)) {
    return false;
  }
  context->set_property("should_intercept_key_release", "0");
  return true;
}

bool CloudInputProcessor::HandleAltF14(const std::string& key_repr,
                                       Context* context,
                                       Config* config) {
  if (key_repr != "Alt+F14" || !context) {
    return false;
  }
  const std::string state = context->get_property("get_ai_stream");
  if (state == "start") {
    if (context->input().empty()) {
      const std::string current_context =
          context->get_property("current_ai_context");
      if (!current_context.empty()) {
        context->set_input(current_context + "_reply:");
      }
    }
    context->RefreshNonConfirmedComposition();
    if (context->get_property("get_ai_stream") == "stop") {
      bool auto_commit_reply = false;
      if (config) {
        config->GetBool("ai_assistant/behavior/auto_commit_reply",
                        &auto_commit_reply);
      }
      if (auto_commit_reply) {
        AIPARA_LOG_DEBUG(logger_, "get_ai_stream==stop, auto commit reply");
        context->set_property("get_ai_stream", "idle");
        KeyEvent space("space");
        engine_->ProcessKey(space);
      }
    }
    return true;
  }
  if (state == "stop") {
    context->set_property("get_ai_stream", "idle");
    bool auto_commit_reply = false;
    if (config) {
      config->GetBool("ai_assistant/behavior/auto_commit_reply",
                      &auto_commit_reply);
    }
    if (auto_commit_reply) {
      KeyEvent space("space");
      engine_->ProcessKey(space);
    }
    return true;
  }
  return true;
}

bool CloudInputProcessor::HandleAltF13(const std::string& key_repr,
                                       Context* context) {
  if (key_repr != "Alt+F13" || !context) {
    return false;
  }
  if (context->get_property("get_cloud_stream") == "starting") {
    context->RefreshNonConfirmedComposition();
  }
  return true;
}

ProcessResult CloudInputProcessor::HandleInterceptSelectKey(
    const std::string& key_repr,
    Context* context,
    Config* config) {
  if (!context || context->get_property("intercept_select_key") != "1") {
    return kNoop;
  }

  if (!(key_repr == "space" || key_repr == "1")) {
    return kNoop;
  }

  context->set_property("intercept_select_key", "0");

  if (!context->get_property("input_string").empty()) {
    context->set_property("input_string", "");
  }

  std::string commit_text = context->GetCommitText();
  if (commit_text.empty()) {
    commit_text = context->get_property("ai_replay_stream");
  }
  if (commit_text.empty()) {
    commit_text = context->input();
  }

  AiAssistantConfig ai_config = LoadAiAssistantConfig(config);
  if (!ai_config.behavior.auto_commit_reply_send_key.empty() &&
      ai_config.behavior.auto_commit_reply_send_key != "none") {
    context->set_property("send_key",
                          ai_config.behavior.auto_commit_reply_send_key);
  }

  if (ContainsNewline(commit_text)) {
    context->Clear();
    if (ai_config.behavior.add_reply_prefix) {
      const std::string script_text = context->GetScriptText();
      if (!script_text.empty()) {
        engine_->CommitText(script_text);
      }
      context->Clear();
    }
    bool success = false;
    if (tcp_zmq_) {
      std::string send_key = context->get_property("send_key");
      if (!send_key.empty()) {
        success = tcp_zmq_->SyncWithServer(engine_, true, true, "button",
                                           "paste_then_" + send_key);
        context->set_property("send_key", "");
      } else {
        success =
            tcp_zmq_->SyncWithServer(engine_, false, false, "button", "paste");
      }
    }
    return success ? kAccepted : kNoop;
  }

  if (ai_config.behavior.add_reply_prefix) {
    const std::string script_text = context->GetScriptText();
    engine_->CommitText(script_text + commit_text);
    context->Clear();
  } else {
    engine_->CommitText(commit_text);
    context->Clear();
  }

  if (tcp_zmq_) {
    std::string send_key = context->get_property("send_key");
    if (!send_key.empty()) {
      tcp_zmq_->SyncWithServer(engine_, true, true, "button", send_key);
      context->set_property("send_key", "");
    } else {
      tcp_zmq_->SyncWithServer(engine_, true, true);
    }
  }

  return kAccepted;
}

ProcessResult CloudInputProcessor::HandleAiTalkSelection(
    const std::string& key_repr,
    Context* context,
    Config* config) {
  if (!context) {
    return kNoop;
  }

  AiAssistantConfig ai_config = LoadAiAssistantConfig(config);

  std::size_t select_index = 0;
  if (!IsSelectKey(key_repr, ai_config, &select_index)) {
    return kNoop;
  }

  Composition& composition = context->composition();
  if (composition.empty()) {
    return kNoop;
  }

  Segment& first_segment = composition.front();
  if (!first_segment.HasTag("ai_talk")) {
    return kNoop;
  }

  if (context->get_property("rawenglish_prompt") == "1") {
    return kNoop;
  }

  std::string chat_trigger;
  for (const auto& tag : first_segment.tags) {
    if (tag != "ai_talk") {
      chat_trigger = tag;
      break;
    }
  }
  if (chat_trigger.empty()) {
    chat_trigger = context->get_property("current_ai_context");
  }

  Segment& last_segment = composition.back();
  if (!last_segment.menu) {
    return kNoop;
  }

  if (last_segment.menu->empty()) {
    return kNoop;
  }

  if (select_index >= last_segment.menu->candidate_count()) {
    return kNoop;
  }

  an<Candidate> candidate = last_segment.menu->GetCandidateAt(select_index);
  if (!candidate) {
    return kNoop;
  }

  const std::string& input = context->input();
  const bool is_last_candidate = candidate->end() == input.size();
  if (!is_last_candidate) {
    return kNoop;
  }

  if (candidate->type() == "clear_chat_history") {
    if (tcp_zmq_) {
      tcp_zmq_->SyncWithServer(engine_, false, false, "clear_chat_history",
                               chat_trigger);
    }
    context->Clear();
    return kAccepted;
  }

  std::string prefix_with_first;
  std::string prefix_without_first;
  if (composition.size() > 1) {
    for (std::size_t i = 0; i + 1 < composition.size(); ++i) {
      Segment& seg = composition[i];
      if (auto selected = seg.GetSelectedCandidate()) {
        const std::string& seg_text = selected->text();
        prefix_with_first += seg_text;
        if (i > 0) {
          prefix_without_first += seg_text;
        }
      }
    }
  }

  const std::string candidate_text = candidate->text();
  std::string commit_text = prefix_with_first;
  commit_text += candidate_text;
  std::string send_text = prefix_without_first;
  send_text += candidate_text;

  if (commit_text.empty()) {
    commit_text = candidate_text;
  }
  if (send_text.empty()) {
    send_text = candidate_text;
  }

  auto it_name = ai_config.chat_names.find(chat_trigger);
  if (it_name != ai_config.chat_names.end()) {
    const std::string chat_name = NormalizeChatPrefix(it_name->second);
    const std::string stripped = MaybeStripPrefix(send_text, chat_name);
    if (!stripped.empty()) {
      send_text = stripped;
    }
  }

  if (tcp_zmq_) {
    tcp_zmq_->ReadAllFromAiSocket();
    context->set_property("ai_replay_stream", std::string(kWaitingMessage));
    context->set_property("start_ai_question", "1");
    context->set_property("get_ai_stream", "start");

    if (ai_config.behavior.commit_question) {
      std::optional<std::string> response_key;
      if (!ai_config.behavior.after_question_send_key.empty()) {
        response_key = ai_config.behavior.after_question_send_key;
      }
      tcp_zmq_->SendChatMessage(send_text, chat_trigger, response_key);
    } else {
      tcp_zmq_->SendChatMessage(send_text, chat_trigger, std::nullopt);
    }
  }

  if (ai_config.behavior.commit_question) {
    context->Clear();
    std::string final_commit = commit_text;
    if (ai_config.behavior.strip_chat_prefix) {
      final_commit = send_text;
    }
    engine_->CommitText(final_commit);
  } else {
    context->Clear();
  }

  return kAccepted;
}

ProcessResult CloudInputProcessor::HandleRawEnglishInput(
    const KeyEvent& key_event,
    const std::string& key_repr,
    Context* context) {
  if (!context || context->get_property("rawenglish_prompt") != "1") {
    return kNoop;
  }

  const auto& key_map = text_formatting::handle_keys();
  auto it = key_map.find(key_repr);
  if (it == key_map.end()) {
    return kNoop;
  }

  const std::string& input = context->input();
  if (input.size() <= 1) {
    AIPARA_LOG_DEBUG(logger_,
                     "Raw English input length <= 1, skip converting key '" +
                         key_repr + "'");
    return kNoop;
  }

  if (key_repr.rfind("Shift+", 0) == 0) {
    context->set_property("should_intercept_key_release", "1");
  }

  context->PushInput(it->second);
  return kAccepted;
}

ProcessResult CloudInputProcessor::HandleCloudConvertTrigger(
    const KeyEvent& key_event,
    const std::string& key_repr,
    Context* context,
    Config* config) {
  if (!context) {
    return kNoop;
  }
  std::string cloud_convert_symbol = "Return";
  if (config) {
    std::string value;
    if (config->GetString("translator/cloud_convert_symbol", &value) &&
        !value.empty()) {
      cloud_convert_symbol = value;
    }
  }
  if (key_repr != cloud_convert_symbol) {
    return kNoop;
  }
  if (context->get_property("cloud_convert_flag") != "1") {
    return kNoop;
  }
  context->set_property("cloud_convert", "1");
  context->RefreshNonConfirmedComposition();
  context->set_property("should_intercept_key_release", "1");
  return kAccepted;
}

void CloudInputProcessor::SetCloudConvertFlag(Context* context,
                                              Config* config) const {
  if (!context) {
    return;
  }
  std::string delimiter = " ";
  if (config) {
    std::string value;
    if (config->GetString("speller/delimiter", &value) && !value.empty()) {
      delimiter = value.substr(0, 1);
    }
  }
  if (delimiter.empty()) {
    return;
  }
  Preedit preedit = context->GetPreedit();
  std::string text = preedit.text;
  const std::size_t cursor = text.find(std::string(kCursorMarker));
  if (cursor != std::string::npos) {
    text.erase(cursor);
  }

  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(delimiter, pos)) != std::string::npos) {
    ++count;
    pos += delimiter.size();
  }

  const bool is_composing = context->IsComposing();
  const std::string flag = context->get_property("cloud_convert_flag");
  if (is_composing && count >= 3) {
    if (flag != "1") {
      context->set_property("cloud_convert_flag", "1");
    }
  } else {
    if (flag != "0") {
      context->set_property("cloud_convert_flag", "0");
    }
  }
}

ProcessResult CloudInputProcessor::HandleAiCandidateCommit(
    const std::string& key_repr,
    const std::string& chat_trigger,
    Context* context,
    Config* config) {
  // 检查上下文对象是否存在，如果不存在则不执行任何操作
  if (!context) {
    return kNoop;
  }

  // 加载AI助手配置
  AiAssistantConfig ai_config = LoadAiAssistantConfig(config);

  // 初始化选择索引，并检查按键是否为选择键
  std::size_t select_index = 0;
  if (!IsSelectKey(key_repr, ai_config, &select_index)) {
    return kNoop;
  }

  // 获取当前编辑组合
  Composition& composition = context->composition();
  // 如果编辑组合为空，则不执行任何操作
  if (composition.empty()) {
    return kNoop;
  }
  // 获取最后一个段落
  Segment& last_segment = composition.back();
  // 检查段落是否有菜单，如果没有则不执行任何操作
  if (!last_segment.menu) {
    return kNoop;
  }

  // 检查菜单是否为空，如果是则不执行任何操作
  if (last_segment.menu->empty()) {
    return kNoop;
  }

  // 检查选择索引是否超出候选词数量范围，如果是则不执行任何操作
  if (select_index >= last_segment.menu->candidate_count()) {
    return kNoop;
  }

  // 获取指定索引的候选词
  an<Candidate> candidate = last_segment.menu->GetCandidateAt(select_index);
  // 如果候选词不存在，则不执行任何操作
  if (!candidate) {
    return kNoop;
  }

  // 特殊处理：清空聊天历史记录
  if (candidate->type() == "clear_chat_history") {
    if (tcp_zmq_) {
      // 与服务器同步，清空聊天历史记录
      tcp_zmq_->SyncWithServer(engine_, false, false, "clear_chat_history",
                               chat_trigger);
    }
    // 清空上下文
    context->Clear();
    return kAccepted;
  }

  // 获取提交文本，如果为空则使用候选词文本
  std::string commit_text = context->GetCommitText();
  if (commit_text.empty()) {
    commit_text = candidate->text();
  }

  // 从配置中获取聊天名称
  std::string chat_name;
  auto it_name = ai_config.chat_names.find(chat_trigger);
  if (it_name != ai_config.chat_names.end()) {
    chat_name = NormalizeChatPrefix(it_name->second);
  }

  // 去除提交文本中的聊天前缀，如果结果为空则使用候选词文本
  std::string send_text = MaybeStripPrefix(commit_text, chat_name);
  if (send_text.empty()) {
    send_text = candidate->text();
  }

  // 如果TCP/ZMQ连接存在，则发送聊天消息
  if (tcp_zmq_) {
    // 读取AI套接字中的所有数据
    tcp_zmq_->ReadAllFromAiSocket();
    // 设置AI回复流为等待消息
    context->set_property("ai_replay_stream", std::string(kWaitingMessage));
    // 标记开始AI问题
    context->set_property("start_ai_question", "1");
    // 标记开始获取AI流
    context->set_property("get_ai_stream", "start");

    // 根据配置决定是否提交问题
    if (ai_config.behavior.commit_question) {
      std::optional<std::string> response_key;
      // 如果配置了问题发送后的按键，则设置响应键
      if (!ai_config.behavior.after_question_send_key.empty()) {
        response_key = ai_config.behavior.after_question_send_key;
      }
      // 发送聊天消息，包含响应键
      tcp_zmq_->SendChatMessage(send_text, chat_trigger, response_key);
    } else {
      // 发送聊天消息，不包含响应键
      tcp_zmq_->SendChatMessage(send_text, chat_trigger, std::nullopt);
    }
  }

  // 根据配置决定是否提交问题文本
  if (ai_config.behavior.commit_question) {
    // 清空上下文
    context->Clear();
    // 确定最终提交的文本
    std::string final_commit = commit_text;
    if (ai_config.behavior.strip_chat_prefix) {
      final_commit = send_text;
    }
    // 通过引擎提交文本
    engine_->CommitText(final_commit);
  } else {
    // 不提交问题文本，只清空上下文
    context->Clear();
  }

  // 返回已接受状态
  return kAccepted;
}

}  // namespace rime::aipara
