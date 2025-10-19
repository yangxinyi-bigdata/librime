#include "ai_assistant_translator.h"

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/translation.h>

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <string_view>

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

#include "common/tcp_socket_sync.h"

namespace rime::aipara {

namespace {
constexpr double kAiSocketTimeoutSeconds = 0.1;
constexpr std::string_view kDefaultWaitingMessage = "等待回复...";

std::string RemoveSuffix(std::string value, const std::string& suffix) {
  if (suffix.size() > value.size()) {
    return value;
  }
  if (value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
    value.resize(value.size() - suffix.size());
  }
  return value;
}

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

struct AiAssistantTranslator::AiStreamData {
  std::string message_type;
  std::string content;
  std::string response_key;
  std::string error_message;
  bool is_final = false;
  bool has_error = false;
};

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
      logger_(MakeLogger("ai_assistant_translator_cpp")) {
  AIPARA_LOG_INFO(logger_, "AiAssistantTranslator initialized.");
}

an<Translation> AiAssistantTranslator::Query(const string& input,
                                             const Segment& segment) {
  AIPARA_LOG_INFO(logger_, "Translator query invoked. input='" + input +
                                "' tags=" + TagsToString(segment.tags));

  if (!engine_) {
    AIPARA_LOG_WARN(logger_, "Translator has no active engine.");
    return nullptr;
  }

  Context* context = engine_->context();
  if (!context) {
    AIPARA_LOG_WARN(logger_, "Engine context unavailable.");
    return nullptr;
  }

  if (segment.HasTag("clear_chat_history")) {
    return HandleClearHistorySegment(segment);
  }
  if (segment.HasTag("ai_talk")) {
    return HandleAiTalkSegment(input, segment, context);
  }
  if (segment.HasTag("ai_reply")) {
    return HandleAiReplySegment(input, segment, context);
  }

  return nullptr;
}

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

void AiAssistantTranslator::AttachTcpSocketSync(TcpSocketSync* sync) {
  tcp_socket_sync_ = sync;
  AIPARA_LOG_INFO(logger_, sync ? "TcpSocketSync attached."
                                : "TcpSocketSync detached.");
}

an<Translation> AiAssistantTranslator::HandleAiTalkSegment(
    const string& /*input*/,
    const Segment& segment,
    Context* context) {
  if (!context) {
    return nullptr;
  }
  const std::string trigger_name =
      context->get_property("current_ai_context");
  AIPARA_LOG_INFO(logger_, "Handling ai_talk segment. trigger='" +
                               trigger_name + "'");

  if (trigger_name.empty()) {
    AIPARA_LOG_WARN(logger_, "current_ai_context is empty.");
    return nullptr;
  }

  auto trigger_it = chat_triggers_.find(trigger_name);
  if (trigger_it == chat_triggers_.end()) {
    AIPARA_LOG_WARN(logger_,
                    "Trigger not found in configuration: " + trigger_name);
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
                  "Generated ai_talk candidate: " + display_text);
  return MakeSingleCandidateTranslation(candidate);
}

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

AiAssistantTranslator::AiStreamResult
AiAssistantTranslator::ReadLatestAiStream() {
  AiStreamResult result;
  if (!tcp_socket_sync_) {
    result.status = AiStreamResult::Status::kError;
    result.error_message = "TcpSocketSync not attached.";
    return result;
  }

  const std::optional<std::string> raw =
      tcp_socket_sync_->ReadLatestAiMessage(kAiSocketTimeoutSeconds);
  if (!raw.has_value()) {
    result.status = AiStreamResult::Status::kTimeout;
    return result;
  }

  result.raw_message = *raw;
  if (result.raw_message.empty()) {
    result.status = AiStreamResult::Status::kNoData;
    return result;
  }

  AIPARA_LOG_DEBUG(logger_, "AI stream raw message: " + result.raw_message);

  rapidjson::Document doc;
  if (doc.Parse(raw->c_str()).HasParseError()) {
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
