#include "cloud_ai_filter_v2.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/config/config_types.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef Bool
#pragma push_macro("Bool")
#undef Bool
#define RIME_CLOUD_FILTER_BOOL_RESTORE
#endif
#ifdef True
#pragma push_macro("True")
#undef True
#define RIME_CLOUD_FILTER_TRUE_RESTORE
#endif
#ifdef False
#pragma push_macro("False")
#undef False
#define RIME_CLOUD_FILTER_FALSE_RESTORE
#endif
#include <rapidjson/document.h>
#ifdef RIME_CLOUD_FILTER_FALSE_RESTORE
#pragma pop_macro("False")
#undef RIME_CLOUD_FILTER_FALSE_RESTORE
#endif
#ifdef RIME_CLOUD_FILTER_TRUE_RESTORE
#pragma pop_macro("True")
#undef RIME_CLOUD_FILTER_TRUE_RESTORE
#endif
#ifdef RIME_CLOUD_FILTER_BOOL_RESTORE
#pragma pop_macro("Bool")
#undef RIME_CLOUD_FILTER_BOOL_RESTORE
#endif

#include "common/spans_manager.h"
#include "common/tcp_zmq.h"

namespace rime::aipara {
namespace {

constexpr std::string_view kLoggerName = "cloud_ai_filter_v2";
constexpr std::string_view kCursorMarker = u8"‸";
constexpr std::string_view kCacheCloudComment = u8"☁📦";
constexpr std::string_view kCacheAiComment = u8"🤖📦";
constexpr double kDefaultCacheTimeoutSeconds = 60.0;

double NowSeconds() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          now.time_since_epoch());
  return seconds.count();
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  if (suffix.empty() || text.size() < suffix.size()) {
    return false;
  }
  return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin());
}

std::string StripTrailingColon(std::string text) {
  while (!text.empty() && text.back() == ':') {
    text.pop_back();
  }
  return text;
}

std::string RequireConfigString(Config* config,
                                const std::string& path) {
  if (!config) {
    throw std::runtime_error(
        "schema config unavailable while reading '" + path + "'");
  }
  std::string value;
  if (!config->GetString(path, &value) || value.empty()) {
    throw std::runtime_error("missing string config '" + path + "'");
  }
  return value;
}

int RequireConfigInt(Config* config, const std::string& path) {
  if (!config) {
    throw std::runtime_error(
        "schema config unavailable while reading '" + path + "'");
  }
  int value = 0;
  if (!config->GetInt(path, &value)) {
    throw std::runtime_error("missing integer config '" + path + "'");
  }
  return value;
}

std::unordered_map<std::string, std::string> RequirePromptValues(
    Config* config,
    const std::string& leaf) {
  if (!config) {
    throw std::runtime_error(
        "schema config unavailable while reading ai prompts");
  }
  an<ConfigMap> prompts = config->GetMap("ai_assistant/ai_prompts");
  if (!prompts) {
    throw std::runtime_error(
        "missing config 'ai_assistant/ai_prompts'");
  }

  std::unordered_map<std::string, std::string> values;
  for (auto it = prompts->begin(); it != prompts->end(); ++it) {
    const std::string& prompt_name = it->first;
    std::string path = "ai_assistant/ai_prompts/" + prompt_name;
    if (!leaf.empty()) {
      path.append("/");
      path.append(leaf);
    }
    std::string value;
    if (!config->GetString(path, &value) || value.empty()) {
      throw std::runtime_error(
          "missing config '" + path + "' for prompt '" +
          prompt_name + "'");
    }
    values.emplace(prompt_name, value);
  }
  return values;
}

an<FifoTranslation> MakeTranslation(
    const std::vector<an<Candidate>>& first,
    const CandidateList& originals) {
  auto fifo = New<FifoTranslation>();
  for (const auto& cand : first) {
    if (cand) {
      fifo->Append(cand);
    }
  }
  for (const auto& cand : originals) {
    if (cand) {
      fifo->Append(cand);
    }
  }
  return fifo;
}

an<FifoTranslation> MakeTranslationFromOriginals(
    const std::vector<an<Candidate>>& originals) {
  auto fifo = New<FifoTranslation>();
  for (const auto& cand : originals) {
    if (cand) {
      fifo->Append(cand);
    }
  }
  return fifo;
}

}  // namespace

CloudAiFilterV2::CloudAiFilterV2(const Ticket& ticket)
    : Filter(ticket),
      logger_(MakeLogger(std::string(kLoggerName))) {
  logger_.Clear();
  cache_.cache_timeout = kDefaultCacheTimeoutSeconds;
  AttachTcpZmq(AcquireGlobalTcpZmq());
}

an<Translation> CloudAiFilterV2::Apply(an<Translation> translation,
                                       CandidateList* /*candidates*/) {
  if (!translation || !engine_) {
    return translation;
  }

  Context* context = engine_->context();
  if (!context) {
    return translation;
  }

  Composition& composition = context->composition();
  if (composition.empty()) {
    return translation;
  }
  Segment& segment = composition.back();

  CandidateList originals;
  while (!translation->exhausted()) {
    an<Candidate> cand = translation->Peek();
    translation->Next();
    if (cand) {
      originals.push_back(cand);
    }
  }

  if (originals.empty()) {
    return translation;
  }

  const an<Candidate>& first = originals.front();
  const std::string schema_name =
      engine_->schema() ? engine_->schema()->schema_id()
                        : std::string();

  Config* config = ResolveConfig();
  std::string delimiter;
  if (config) {
    try {
      const std::string delimiter_raw =
          RequireConfigString(config, "speller/delimiter");
      delimiter = delimiter_raw.substr(0, 1);
    } catch (const std::exception& e) {
      delimiter.clear();
      context->set_property("cloud_convert_flag", "0");
      AIPARA_LOG_ERROR(logger_,
                       "Failed to read speller/delimiter: " +
                           std::string(e.what()));
    }
  } else {
    context->set_property("cloud_convert_flag", "0");
    AIPARA_LOG_ERROR(logger_,
                     "Schema config unavailable while resolving "
                     "speller/delimiter.");
  }
  SetCloudConvertFlag(first.get(), context, delimiter);

  if (segment.HasTag("ai_prompt")) {


    try {
      const std::string prompt_chat = RequireConfigString(
          config, "ai_assistant/behavior/prompt_chat");
      const auto chat_triggers =
          RequirePromptValues(config, "chat_triggers");
      const auto chat_names =
          RequirePromptValues(config, "chat_names");

      std::vector<std::string> prompt_triggers;
      if (!prompt_chat.empty()) {
        const char prefix_char = prompt_chat.front();
        for (const auto& [trigger_name, trigger_prefix] :
             chat_triggers) {
          if (trigger_prefix.empty() ||
              trigger_prefix.front() != prefix_char) {
            continue;
          }
          auto it_name = chat_names.find(trigger_name);
          if (it_name == chat_names.end()) {
            throw std::runtime_error(
                "missing chat_names entry for prompt '" +
                trigger_name + "'");
          }
          std::string chat_name =
              StripTrailingColon(it_name->second);
          if (chat_name.empty()) {
            continue;
          }
          prompt_triggers.push_back(trigger_prefix + chat_name);
        }
        std::sort(prompt_triggers.begin(), prompt_triggers.end());
      }

      const std::size_t max_rounds = prompt_triggers.size() / 2;
      std::size_t current_round = 0;
      std::vector<an<Candidate>> rewritten;
      rewritten.reserve(originals.size());

      for (std::size_t index = 0; index < originals.size();
           ++index) {
        const an<Candidate>& cand = originals[index];
        std::string comment;
        if (current_round < max_rounds) {
          const std::size_t base = current_round * 2;
          comment.assign(" ");
          comment.append(prompt_triggers[base]);
          if (base + 1 < prompt_triggers.size()) {
            comment.append("  ");
            comment.append(prompt_triggers[base + 1]);
          }
          ++current_round;
        }

        if (!comment.empty()) {
          auto shadow = New<ShadowCandidate>(
              cand, cand->type(), std::string(), comment);
          rewritten.push_back(shadow);
        } else {
          rewritten.push_back(cand);
        }
      }

      return MakeTranslationFromOriginals(rewritten);
    } catch (const std::exception& e) {
      AIPARA_LOG_ERROR(
          logger_,
          "Failed to construct ai_prompt candidates: " +
              std::string(e.what()));
      return MakeTranslationFromOriginals(originals);
    }
  }

  const std::string cand_type = first->type();
  if (cand_type == "punct" || EndsWith(cand_type, "ai_chat")) {
    return MakeTranslationFromOriginals(originals);
  }

  const std::string& cloud_convert =
      context->get_property("cloud_convert");
  const std::string& get_cloud_stream =
      context->get_property("get_cloud_stream");

  if (cloud_convert != "1" && get_cloud_stream != "starting") {
    return MakeTranslationFromOriginals(originals);
  }

  const std::string& input = context->input();
  const size_t seg_start = segment.start;
  const size_t seg_end = segment.end;
  if (seg_start >= input.size() || seg_end > input.size() ||
      seg_end <= seg_start) {
    return MakeTranslationFromOriginals(originals);
  }
  const std::string segment_input =
      input.substr(seg_start, seg_end - seg_start);

  std::optional<int> max_cloud_candidates;
  std::optional<int> max_ai_candidates;
  auto build_candidates_safe =
      [&](const ParsedResult& parsed, bool from_cache)
          -> std::vector<an<Candidate>> {
        if (!config) {
          throw std::runtime_error(
              "schema config unavailable while building candidates");
        }
        if (!max_cloud_candidates) {
          max_cloud_candidates = RequireConfigInt(
              config, "cloud_ai_filter/max_cloud_candidates");
        }
        if (!max_ai_candidates) {
          max_ai_candidates = RequireConfigInt(
              config, "cloud_ai_filter/max_ai_candidates");
        }
        return BuildCandidatesFromResult(
            parsed, first.get(), seg_start, seg_end,
            *max_cloud_candidates, *max_ai_candidates, from_cache);
      };

  if (cloud_convert == "1") {
    if (!tcp_zmq_) {
      AttachTcpZmq(AcquireGlobalTcpZmq());
    }
    std::vector<std::string> long_texts =
        CollectLongCandidateTexts(originals, seg_end);
    if (tcp_zmq_) {
      bool request_attempted = false;
      bool send_success = false;
      if (config) {
        try {
          tcp_zmq_->RefreshCurveConfig(config);
          const std::string shuru_schema = RequireConfigString(
              config, "schema/my_shuru_schema");
          request_attempted = true;
          send_success = tcp_zmq_->SendConvertRequest(
              schema_name, shuru_schema, segment_input,
              long_texts);
        } catch (const std::exception& e) {
          AIPARA_LOG_ERROR(
              logger_,
              "Failed to send cloud convert request: " +
                  std::string(e.what()));
          context->set_property("get_cloud_stream", "error");
        }
      } else {
        AIPARA_LOG_ERROR(
            logger_,
            "Schema config unavailable while sending cloud "
            "convert request.");
        context->set_property("get_cloud_stream", "error");
      }

      if (request_attempted) {
        if (send_success) {
          context->set_property("get_cloud_stream", "starting");
        } else {
          context->set_property("get_cloud_stream", "error");
        }
      }
    }
  }

  std::vector<an<Candidate>> cloud_candidates;

  if (context->get_property("get_cloud_stream") == "starting") {
    if (tcp_zmq_) {
      context->set_property("cloud_convert", "0");
      auto stream_result = tcp_zmq_->ReadConvertResult(0.01);
      if (stream_result.status == TcpZmq::LatestStatus::kSuccess &&
          stream_result.data) {
        const ParsedResult parsed =
            ParseConvertResult(*stream_result.data);
        SaveCache(segment_input, parsed);
        try {
          cloud_candidates =
              build_candidates_safe(parsed, false);
        } catch (const std::exception& e) {
          AIPARA_LOG_ERROR(
              logger_,
              "Failed to build candidates from stream: " +
                  std::string(e.what()));
        }

        if (stream_result.is_final) {
          context->set_property("get_cloud_stream", "stop");
          ClearCache();
        }
      } else if (stream_result.status ==
                 TcpZmq::LatestStatus::kTimeout) {
        context->set_property("get_cloud_stream", "starting");
      } else if (stream_result.status ==
                 TcpZmq::LatestStatus::kError) {
        context->set_property("get_cloud_stream", "error");
        ClearCache();
      } else {
        if (auto cached = GetCache(segment_input)) {
          try {
            cloud_candidates =
                build_candidates_safe(*cached, true);
          } catch (const std::exception& e) {
            AIPARA_LOG_ERROR(
                logger_,
                "Failed to build candidates from cache: " +
                    std::string(e.what()));
          }
        }
      }
    }
  }

  if (!cloud_candidates.empty()) {
    if (!spans_manager::GetSpans(context)) {
      spans_manager::ExtractAndSaveFromCandidate(
          context, first.get(), input, "cloud_ai_filter_v2", &logger_);
    }
  }

  return MakeTranslation(cloud_candidates, originals);
}

void CloudAiFilterV2::UpdateCurrentConfig(Config* config) {
  if (!config) {
    AIPARA_LOG_INFO(
        logger_,
        "UpdateCurrentConfig called with null config. "
        "cloud_ai_filter_v2 now reads configuration on demand.");
  } else {
    AIPARA_LOG_INFO(
        logger_,
        "UpdateCurrentConfig invoked; cloud_ai_filter_v2 reads "
        "configuration lazily.");
  }
  ClearCache();
}

void CloudAiFilterV2::AttachTcpZmq(TcpZmq* client) {
  tcp_zmq_ = client;
}

Config* CloudAiFilterV2::ResolveConfig() const {
  if (!engine_) {
    return nullptr;
  }
  if (auto* schema = engine_->schema()) {
    return schema->config();
  }
  return nullptr;
}

void CloudAiFilterV2::ClearCache() {
  cache_.last_input.clear();
  cache_.cloud_candidates.clear();
  cache_.ai_candidates.clear();
  cache_.timestamp = 0.0;
}

void CloudAiFilterV2::SaveCache(const std::string& input,
                                const ParsedResult& parsed) {
  if (parsed.cloud_candidates.empty() &&
      parsed.ai_candidates.empty()) {
    return;
  }
  cache_.last_input = input;
  cache_.cloud_candidates = parsed.cloud_candidates;
  cache_.ai_candidates = parsed.ai_candidates;
  cache_.timestamp = NowSeconds();
}

std::optional<CloudAiFilterV2::ParsedResult> CloudAiFilterV2::GetCache(
    const std::string& input) const {
  if (cache_.last_input != input) {
    return std::nullopt;
  }
  if (cache_.cloud_candidates.empty() &&
      cache_.ai_candidates.empty()) {
    return std::nullopt;
  }
  const double now = NowSeconds();
  if (cache_.timestamp <= 0.0 ||
      (now - cache_.timestamp) > cache_.cache_timeout) {
    return std::nullopt;
  }

  ParsedResult parsed;
  parsed.cloud_candidates = cache_.cloud_candidates;
  parsed.ai_candidates = cache_.ai_candidates;
  return parsed;
}

CloudAiFilterV2::ParsedResult CloudAiFilterV2::ParseConvertResult(
    const rapidjson::Document& doc) const {
  ParsedResult parsed;
  if (doc.HasMember("cloud_candidates") &&
      doc["cloud_candidates"].IsArray()) {
    for (const auto& item : doc["cloud_candidates"].GetArray()) {
      std::string value;
      if (item.IsString()) {
        value.assign(item.GetString(), item.GetStringLength());
      } else if (item.IsObject()) {
        if (item.HasMember("value") && item["value"].IsString()) {
          value.assign(item["value"].GetString(),
                       item["value"].GetStringLength());
        } else if (item.HasMember("text") &&
                   item["text"].IsString()) {
          value.assign(item["text"].GetString(),
                       item["text"].GetStringLength());
        }
      }
      if (!value.empty()) {
        parsed.cloud_candidates.push_back(std::move(value));
      }
    }
  }

  if (doc.HasMember("ai_candidates") &&
      doc["ai_candidates"].IsArray()) {
    for (const auto& item : doc["ai_candidates"].GetArray()) {
      std::string value;
      std::string comment_name;
      if (item.IsString()) {
        value.assign(item.GetString(), item.GetStringLength());
      } else if (item.IsObject()) {
        if (item.HasMember("value") && item["value"].IsString()) {
          value.assign(item["value"].GetString(),
                       item["value"].GetStringLength());
        } else if (item.HasMember("text") &&
                   item["text"].IsString()) {
          value.assign(item["text"].GetString(),
                       item["text"].GetStringLength());
        }
        if (item.HasMember("comment_name") &&
            item["comment_name"].IsString()) {
          comment_name.assign(
              item["comment_name"].GetString(),
              item["comment_name"].GetStringLength());
        }
      }
      if (!value.empty()) {
        parsed.ai_candidates.emplace_back(std::move(value),
                                          std::move(comment_name));
      }
    }
  }

  return parsed;
}

std::vector<an<Candidate>> CloudAiFilterV2::BuildCandidatesFromResult(
    const ParsedResult& result,
    const Candidate* reference,
    size_t segment_start,
    size_t segment_end,
    int max_cloud_candidates,
    int max_ai_candidates,
    bool from_cache) const {
  std::vector<an<Candidate>> output;
  const std::string preedit =
      reference ? reference->preedit() : std::string();

  const std::size_t cloud_limit =
      static_cast<std::size_t>(std::max(0, max_cloud_candidates));
  const std::size_t ai_limit =
      static_cast<std::size_t>(std::max(0, max_ai_candidates));

  for (std::size_t i = 0;
       i < result.cloud_candidates.size() && i < cloud_limit; ++i) {
    const std::string& text = result.cloud_candidates[i];
    auto candidate = New<SimpleCandidate>(
        "baidu_cloud", segment_start, segment_end, text,
        from_cache ? std::string(kCacheCloudComment) : std::string(),
        preedit);
    const double quality =
        900 + (static_cast<int>(cloud_limit - i)) * 10;
    candidate->set_quality(quality);
    output.push_back(candidate);
  }

  for (std::size_t i = 0;
       i < result.ai_candidates.size() && i < ai_limit; ++i) {
    const auto& [text, comment_name] = result.ai_candidates[i];
    std::string type = "ai_cloud";
    if (!comment_name.empty()) {
      type.append("/");
      type.append(comment_name);
    }
    auto candidate = New<SimpleCandidate>(
        type, segment_start, segment_end, text,
        from_cache ? std::string(kCacheAiComment) : std::string(),
        preedit);
    const double quality =
        950 + (static_cast<int>(ai_limit - i)) * 10;
    candidate->set_quality(quality);
    output.push_back(candidate);
  }

  return output;
}

std::vector<std::string> CloudAiFilterV2::CollectLongCandidateTexts(
    const CandidateList& originals,
    size_t segment_end) const {
  std::vector<std::string> result;
  for (const auto& cand : originals) {
    if (!cand) {
      continue;
    }
    if (cand->end() == segment_end) {
      result.push_back(cand->text());
    } else {
      break;
    }
  }
  return result;
}

void CloudAiFilterV2::SetCloudConvertFlag(const Candidate* candidate,
                                          Context* context,
                                          const std::string& delimiter) const {
  if (!candidate || !context) {
    return;
  }
  if (delimiter.empty()) {
    context->set_property("cloud_convert_flag", "0");
    return;
  }

  std::string preedit = candidate->preedit();
  const std::string cursor_marker(kCursorMarker);
  const auto cursor_pos = preedit.find(cursor_marker);
  if (cursor_pos != std::string::npos) {
    preedit.erase(cursor_pos);
  }

  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = preedit.find(delimiter, pos)) != std::string::npos) {
    ++count;
    pos += delimiter.size();
  }

  const bool composing = context->IsComposing();
  const std::string flag = context->get_property("cloud_convert_flag");
  if (composing && count >= 3) {
    if (flag != "1") {
      context->set_property("cloud_convert_flag", "1");
    }
  } else {
    if (flag != "0") {
      context->set_property("cloud_convert_flag", "0");
    }
  }
}

}  // namespace rime::aipara
