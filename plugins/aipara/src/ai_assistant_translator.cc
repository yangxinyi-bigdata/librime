// 实现 AiAssistantTranslator：把 Rime 的查询请求（Query）根据标签路由到
// 不同的处理逻辑（清空历史/AI 对话/AI 回复），并通过 TcpZmq 轮询
// 外部服务的增量消息，解析 JSON，更新上下文，生成候选。
#include "ai_assistant_translator.h"

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/translation.h>

#include <algorithm>
#include <set>
#include <utility>
#include <string_view>

// 处理可能的宏污染：有些平台/头文件可能把 Bool/True/False 定义为宏，
// 这里统一取消定义，避免与 C++ 关键字/标识符冲突。
#ifdef Bool
#undef Bool
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
// 可以考虑先搞一个简单白本,不用tcp协议,就可以更加简单的测试了
#include "common/tcp_zmq.h"

namespace rime::aipara {

namespace {
// 常量：
// - kAiSocketTimeoutSeconds：读取 AI socket 的超时时间（秒）。
// - kDefaultWaitingMessage：展示给用户的“等待中”提示。
// 这利只是设置两个常量,应该没什么影响
constexpr double kAiSocketTimeoutSeconds = 0.1;
constexpr std::string_view kDefaultWaitingMessage = "等待回复...";

// 从字符串末尾移除指定后缀（若存在）。按值传参（value）允许在函数内就地修改副本，
// 避免修改调用者的原始字符串。
std::string RemoveSuffix(std::string value, const std::string& suffix) {
  // 如果 suffix 比 value 长，直接返回 value。
  if (suffix.size() > value.size()) {
    return value;
  }
  // 如果 value 以 suffix 结尾，移除 suffix。
  if (value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
    value.resize(value.size() - suffix.size());
  }
  return value;
}

// 把标签集合 join 成逗号分隔字符串，方便日志打印。
std::string TagsToString(const std::set<std::string>& tags) {
  std::string joined;
  for (const auto& tag : tags) {
    if (!joined.empty()) {
      joined.append(",");
    }
    joined.append(tag);
  }
  return joined;
}
}  // namespace

// 描述一次 AI 流消息的核心字段：消息类型、文本、响应 key、错误及是否最终片段。
struct AiAssistantTranslator::AiStreamData {
  std::string message_type;
  std::string content;
  std::string response_key;
  std::string error_message;
  bool is_final = false;
  bool has_error = false;
};

// 读取 AI 流的结果：包含状态（无数据/超时/成功/错误）、解析后的数据、原始报文等。
struct AiAssistantTranslator::AiStreamResult {
  enum class Status {
    kNoData,
    kTimeout,
    kSuccess,
    kError,
  };

  Status status = Status::kNoData;
  AiStreamData data;
  std::string raw_message;
  std::string error_message;

  bool IsSuccess() const { return status == Status::kSuccess; }
  bool IsTimeout() const { return status == Status::kTimeout; }
  bool IsError() const { return status == Status::kError; }
};

AiAssistantTranslator::AiAssistantTranslator(const Ticket& ticket)
    : Translator(ticket),
      logger_(MakeLogger("ai_assistant_translator")) {
  AIPARA_LOG_INFO(logger_, "AiAssistantTranslator initialized.");
}

// Query：Rime 会针对每个 segment 调用本函数询问“你能提供候选吗”。
// 根据 segment.tags 决定走哪个子处理函数；若不处理返回 nullptr。
an<Translation> AiAssistantTranslator::Query(const string& input,
                                             const Segment& segment) {
// Query是默认的调用函数, 这里确认已经正确的获取到input和segment
  AIPARA_LOG_INFO(logger_, "Translator query invoked. input='" + input +
                                "' tags=" + TagsToString(segment.tags));

  if (!engine_) {
    AIPARA_LOG_WARN(logger_, "Translator has no active engine.");
    return nullptr;
  }

  // 获取 Rime 上下文，用于访问全局状态（如选项、选项值、选项变更记录等）。
  Context* context = engine_->context();
  if (!context) {
    AIPARA_LOG_WARN(logger_, "Engine context unavailable.");
    return nullptr;
  }

  // 如果 segment 包含 clear_chat_history 标签，调用 HandleClearHistorySegment函数
  // 清空历史并返回 nullptr。
  if (segment.HasTag("clear_chat_history")) {
    return HandleClearHistorySegment(segment);
  }
  // 如果 segment 包含 ai_talk 标签，调用 HandleAiTalkSegment 处理 AI 对话。
  if (segment.HasTag("ai_talk")) {
    AIPARA_LOG_WARN(logger_, "进入ai_talk标签分支");
    return HandleAiTalkSegment(input, segment, context);
  }
  // 如果 segment 包含 ai_reply 标签，发起 AI 回复并返回 nullptr。
  if (segment.HasTag("ai_reply")) {
    return HandleAiReplySegment(input, segment, context);
  }

  return nullptr;
}

// 从 Rime 配置加载 ai_assistant/ai_prompts 下的各项设置：
// - chat_triggers：触发词；reply_messages_preedits：预编辑展示；chat_names：显示名称。
// 读取后填充到本类的哈希表缓存。
void AiAssistantTranslator::UpdateCurrentConfig(Config* config) {
  chat_triggers_.clear();
  reply_messages_preedits_.clear();
  chat_names_.clear();
  reply_input_to_trigger_.clear();

  if (!config) {
    AIPARA_LOG_WARN(logger_, "UpdateCurrentConfig called with null config.");
    return;
  }

  if (an<ConfigMap> prompts = config->GetMap("ai_assistant/ai_prompts")) {
    int entry_count = 0;
    for (auto it = prompts->begin(); it != prompts->end(); ++it) {
      ++entry_count;
    }
    AIPARA_LOG_INFO(
        logger_, "Loading ai_assistant/ai_prompts, entries=" +
                     std::to_string(entry_count));
    for (auto it = prompts->begin(); it != prompts->end(); ++it) {
      const std::string& trigger_name = it->first;
      const std::string base_path =
          "ai_assistant/ai_prompts/" + trigger_name;

      std::string trigger_value;
      if (config->GetString(base_path + "/chat_triggers", &trigger_value) &&
          !trigger_value.empty()) {
        chat_triggers_[trigger_name] = trigger_value;
        AIPARA_LOG_INFO(logger_, "Configured chat trigger '" + trigger_name +
                                     "' -> '" + trigger_value + "'");
      }

      std::string reply_message_preedit;
      if (config->GetString(base_path + "/reply_messages_preedits",
                            &reply_message_preedit) &&
          !reply_message_preedit.empty()) {
        reply_messages_preedits_[trigger_name] = reply_message_preedit;
        AIPARA_LOG_INFO(logger_,
                        "Configured reply preedit for '" + trigger_name + "'");
      }

      std::string chat_name;
      if (config->GetString(base_path + "/chat_names", &chat_name) &&
          !chat_name.empty()) {
        chat_names_[trigger_name] = chat_name;
        AIPARA_LOG_INFO(logger_,
                        "Configured chat name for '" + trigger_name + "'");
      }
    }
  } else {
    AIPARA_LOG_WARN(logger_, "No ai_assistant/ai_prompts map available.");
  }

  reply_input_to_trigger_.clear();
  for (const auto& entry : reply_messages_preedits_) {
    reply_input_to_trigger_[entry.second] = entry.first;
  }
}

// 绑定/解绑 TCP 同步器。
void AiAssistantTranslator::AttachTcpZmq(TcpZmq* client) {
  tcp_zmq_ = client;
  AIPARA_LOG_INFO(logger_, client ? "TcpZmq attached."
                                  : "TcpZmq detached.");
}

// 处理带有 ai_talk 标签的分段：根据当前上下文中设置的 current_ai_context
// 决定触发器，并生成一个展示候选（一般用于“进入某聊天上下文”的提示）。
an<Translation> AiAssistantTranslator::HandleAiTalkSegment(
    const string& /*input*/,
    const Segment& segment,
    Context* context) {
  if (!context) {
    return nullptr;
  }
  const std::string trigger_name =
      context->get_property("current_ai_context");
  AIPARA_LOG_INFO(logger_, "获取到 current_ai_context: trigger_name='" +
                               trigger_name + "'");

  if (trigger_name.empty()) {
    AIPARA_LOG_WARN(logger_, "current_ai_context is empty.");
    return nullptr;
  }

  auto trigger_it = chat_triggers_.find(trigger_name);
  if (trigger_it == chat_triggers_.end()) {
    AIPARA_LOG_WARN(logger_,
                    "trigger_name 在配置中没有找到: " + trigger_name);
    return nullptr;
  }

  const std::string display_text = [this, &trigger_name, &trigger_it]() {
    auto name_it = chat_names_.find(trigger_name);
    if (name_it != chat_names_.end() && !name_it->second.empty()) {
      return name_it->second;
    }
    return trigger_it->second + " AI助手";
  }();

  auto candidate =
      MakeCandidate(trigger_name, segment.start, segment.end, display_text);
  if (!candidate) {
    return nullptr;
  }

  AIPARA_LOG_INFO(logger_,
                  "生成 ai_talk 候选词: " + display_text);
  return MakeSingleCandidateTranslation(candidate);
}

// 生成“清空对话记录”的候选，不涉及 socket 通信。
an<Translation> AiAssistantTranslator::HandleClearHistorySegment(
    const Segment& segment) {
  auto candidate = MakeCandidate("clear_chat_history", segment.start,
                                 segment.end, "清空对话记录");
  if (!candidate) {
    return nullptr;
  }
  AIPARA_LOG_INFO(logger_, "Generated clear_chat_history candidate.");
  return MakeSingleCandidateTranslation(candidate);
}

// 处理 ai_reply 标签：轮询 socket 读增量消息，解析 JSON，把内容写入上下文属性
// （ai_replay_stream），控制 get_ai_stream 的状态机（start/stop/idle），
// 并以当前缓存的内容构造候选显示给用户。
an<Translation> AiAssistantTranslator::HandleAiReplySegment(
    const string& /*input*/,
    const Segment& segment,
    Context* context) {
  if (!context) {
    return nullptr;
  }

  std::string reply_tag;
  for (const auto& tag : segment.tags) {
    if (tag == "ai_reply") {
      continue;
    }
    reply_tag = tag;
    break;
  }

  if (reply_tag.empty()) {
    AIPARA_LOG_WARN(
        logger_,
        "ai_reply segment missing specific reply tag. tags=" +
            TagsToString(segment.tags));
    return nullptr;
  }

  const std::string trigger_name =
      RemoveSuffix(reply_tag, "_reply");
  const auto preedit_it =
      reply_messages_preedits_.find(trigger_name);
  const std::string preedit =
      preedit_it != reply_messages_preedits_.end() ? preedit_it->second : "";

  const std::string stream_state =
      context->get_property("get_ai_stream");
  if (stream_state == "stop") {
    std::string current_content =
        context->get_property("ai_replay_stream");
    if (current_content.empty()) {
      current_content = std::string(kDefaultWaitingMessage);
    }
    auto candidate =
        MakeCandidate(reply_tag, segment.start, segment.end,
                      current_content, preedit);
    if (!candidate) {
      return nullptr;
    }
    AIPARA_LOG_INFO(logger_,
                    "Stream stopped, returning cached AI reply text.");
    return MakeSingleCandidateTranslation(candidate);
  }

  const AiStreamResult stream_result = ReadLatestAiStream();
  if (stream_result.IsError()) {
    AIPARA_LOG_ERROR(
        logger_, "Stream error: " + stream_result.error_message);
    context->set_property("get_ai_stream", "idle");
    if (!stream_result.error_message.empty()) {
      context->set_property("ai_replay_stream",
                            stream_result.error_message);
    }
  } else if (stream_result.IsSuccess()) {
    if (stream_result.data.has_error) {
      context->set_property("get_ai_stream", "idle");
      if (!stream_result.data.error_message.empty()) {
        context->set_property("ai_replay_stream",
                              stream_result.data.error_message);
      }
      AIPARA_LOG_WARN(logger_, "AI stream reported error: " +
                                   stream_result.data.error_message);
    } else if (stream_result.data.is_final) {
      context->set_property("get_ai_stream", "stop");
      context->set_property("intercept_select_key", "1");
      AIPARA_LOG_INFO(logger_, "AI stream final message received.");
    } else {
      context->set_property("get_ai_stream", "start");
    }

    if (!stream_result.data.content.empty()) {
      context->set_property("ai_replay_stream",
                            stream_result.data.content);
      AIPARA_LOG_DEBUG(logger_,
                       "Updated ai_replay_stream content: " +
                           stream_result.data.content);
    }
  } else if (stream_result.IsTimeout()) {
    context->set_property("get_ai_stream", "start");
    AIPARA_LOG_DEBUG(logger_, "AI stream timeout, continue polling.");
  } else {
    context->set_property("get_ai_stream", "start");
    AIPARA_LOG_DEBUG(logger_, "No AI stream data available.");
  }

  std::string current_content =
      context->get_property("ai_replay_stream");
  if (current_content.empty()) {
    current_content = std::string(kDefaultWaitingMessage);
  }

  auto candidate =
      MakeCandidate(reply_tag, segment.start, segment.end,
                    current_content, preedit);
  if (!candidate) {
    return nullptr;
  }

  AIPARA_LOG_INFO(logger_, "Generated ai_reply candidate text length=" +
                               std::to_string(current_content.size()));
  return MakeSingleCandidateTranslation(candidate);
}

// 从 TcpZmq 非阻塞读取最近一条 AI 消息，设置超时；
// 使用 RapidJSON 解析，兼容不同字段位置（data 包裹或直接在根对象）。
AiAssistantTranslator::AiStreamResult
AiAssistantTranslator::ReadLatestAiStream() {
  AiStreamResult result;
  if (!tcp_zmq_) {
    result.status = AiStreamResult::Status::kError;
    result.error_message = "TcpZmq not attached.";
    return result;
  }

  const TcpZmq::LatestAiMessage latest =
      tcp_zmq_->ReadLatestFromAiSocket(kAiSocketTimeoutSeconds);

  switch (latest.status) {
    case TcpZmq::LatestStatus::kSuccess:
      break;
    case TcpZmq::LatestStatus::kTimeout:
      result.status = AiStreamResult::Status::kTimeout;
      return result;
    case TcpZmq::LatestStatus::kNoData:
      result.status = AiStreamResult::Status::kNoData;
      return result;
    case TcpZmq::LatestStatus::kError:
      result.status = AiStreamResult::Status::kError;
      result.error_message =
          latest.error_msg.value_or("TcpZmq read error.");
      return result;
  }

  result.raw_message = latest.raw_message;
  if (result.raw_message.empty()) {
    result.status = AiStreamResult::Status::kNoData;
    return result;
  }

  AIPARA_LOG_DEBUG(logger_, "AI stream raw message: " + result.raw_message);

  rapidjson::Document doc;
  if (doc.Parse(result.raw_message.c_str()).HasParseError()) {
    result.status = AiStreamResult::Status::kError;
    result.error_message = std::string("JSON parse error: ") +
                           rapidjson::GetParseError_En(
                               doc.GetParseError()) +
                           " at offset " +
                           std::to_string(doc.GetErrorOffset());
    return result;
  }

  result.status = AiStreamResult::Status::kSuccess;

  const rapidjson::Value* payload = &doc;
  if (doc.HasMember("data") && doc["data"].IsObject()) {
    payload = &doc["data"];
  }

  if (doc.HasMember("messege_type") && doc["messege_type"].IsString()) {
    result.data.message_type = doc["messege_type"].GetString();
  }

  if (payload->HasMember("content") && (*payload)["content"].IsString()) {
    result.data.content = (*payload)["content"].GetString();
  }
  if (payload->HasMember("response_key") &&
      (*payload)["response_key"].IsString()) {
    result.data.response_key = (*payload)["response_key"].GetString();
  }
  if (payload->HasMember("is_final") && (*payload)["is_final"].IsBool()) {
    result.data.is_final = (*payload)["is_final"].GetBool();
  }
  if (payload->HasMember("error")) {
    const auto& error_value = (*payload)["error"];
    if (error_value.IsString()) {
      result.data.has_error = true;
      result.data.error_message = error_value.GetString();
    } else if (error_value.IsBool()) {
      result.data.has_error = error_value.GetBool();
    }
  }
  if (payload->HasMember("error_msg") &&
      (*payload)["error_msg"].IsString()) {
    result.data.has_error = true;
    result.data.error_message = (*payload)["error_msg"].GetString();
  }
  if (doc.HasMember("status") && doc["status"].IsString()) {
    const std::string status_value = doc["status"].GetString();
    if (status_value == "error") {
      result.data.has_error = true;
      if (payload->HasMember("message") &&
          (*payload)["message"].IsString()) {
        result.data.error_message = (*payload)["message"].GetString();
      }
    }
  }

  if (result.data.has_error && result.error_message.empty() &&
      !result.data.error_message.empty()) {
    result.error_message = result.data.error_message;
  }

  return result;
}

// 构建 Rime 的 SimpleCandidate，并设置质量与预编辑文本。
an<Candidate> AiAssistantTranslator::MakeCandidate(
    const std::string& type,
    size_t start,
    size_t end,
    const std::string& text,
    const std::string& preedit,
    double quality) const {
  auto candidate =
      New<SimpleCandidate>(type, start, end, text);
  if (!candidate) {
    return nullptr;
  }
  candidate->set_quality(quality);
  if (!preedit.empty()) {
    candidate->set_preedit(preedit);
  }
  return candidate;
}

// 把单个候选写入一个 FIFO 翻译流中返回。
an<Translation> AiAssistantTranslator::MakeSingleCandidateTranslation(
    an<Candidate> candidate) const {
  if (!candidate) {
    return nullptr;
  }
  auto translation = New<FifoTranslation>();
  if (!translation) {
    return nullptr;
  }
  translation->Append(candidate);
  return translation;
}

}  // namespace rime::aipara
