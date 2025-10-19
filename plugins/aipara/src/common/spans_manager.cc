#include "spans_manager.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/gear/translator_commons.h>

#include <ctime>
#include <sstream>
#include <unordered_map>

namespace rime::aipara::spans_manager {
namespace {

constexpr const char kSpansVerticesKey[] = "spans_vertices";
constexpr const char kSpansInputKey[] = "spans_input";
constexpr const char kSpansSourceKey[] = "spans_source";
constexpr const char kSpansTimestampKey[] = "spans_timestamp";

const std::unordered_map<std::string, int>& SourcePriority() {
  static const std::unordered_map<std::string, int>* priority = [] {
    auto* map = new std::unordered_map<std::string, int>{
        {"rawenglish_translator", 1},
        {"cloud_ai_filter_v2", 2},
        {"baidu_filter", 2},
        {"punct_eng_chinese_filter", 3},
        {"unknown", 99},
    };
    return map;
  }();
  return *priority;
}

int PriorityFor(const std::string& source) {
  const auto& table = SourcePriority();
  if (auto it = table.find(source); it != table.end()) {
    return it->second;
  }
  return 99;
}

std::vector<std::size_t> VerticesFromSpans(const rime::Spans& spans) {
  std::vector<std::size_t> vertices;
  const std::size_t first = spans.start();
  if (spans.HasVertex(first)) {
    vertices.push_back(first);
  }
  std::size_t caret = first;
  while (true) {
    const std::size_t next = spans.NextStop(caret);
    if (next == caret) {
      break;
    }
    vertices.push_back(next);
    caret = next;
  }
  return vertices;
}

std::string JoinVertices(const std::vector<std::size_t>& vertices) {
  std::ostringstream stream;
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    if (i != 0) {
      stream << ',';
    }
    stream << vertices[i];
  }
  return stream.str();
}

}  // namespace

bool SaveSpans(Context* context,
               const std::vector<std::size_t>& vertices,
               const std::string& input,
               const std::string& source,
               Logger* logger) {
  if (!context) {
    if (logger) {
      AIPARA_LOG_ERROR(*logger, "save_spans: context不能为空");
    }
    return false;
  }
  if (vertices.empty() || input.empty()) {
    if (logger) {
      AIPARA_LOG_ERROR(*logger, "save_spans: vertices或input不能为空");
    }
    return false;
  }

  const std::string source_name = source.empty() ? "unknown" : source;
  const std::string existing_source = context->get_property(kSpansSourceKey);
  const std::string existing_input = context->get_property(kSpansInputKey);

  if (!existing_source.empty()) {
    const int existing_priority = PriorityFor(existing_source);
    const int new_priority = PriorityFor(source_name);
    if (new_priority > existing_priority) {
      if (logger) {
        std::ostringstream stream;
        stream << "save_spans: 跳过保存，已有更高优先级的spans (现有:" << existing_source
               << '[' << existing_priority << "] vs 新:" << source_name << '['
               << new_priority << ")";
        AIPARA_LOG_INFO(*logger, stream.str());
      }
      return false;
    }
    if (existing_input == input && new_priority == existing_priority) {
      if (logger) {
        AIPARA_LOG_DEBUG(*logger,
                         "save_spans: 跳过保存，输入内容和优先级相同");
      }
      return false;
    }
  }

  const std::string vertices_str = JoinVertices(vertices);
  context->set_property(kSpansVerticesKey, vertices_str);
  context->set_property(kSpansInputKey, input);
  context->set_property(kSpansSourceKey, source_name);
  context->set_property(kSpansTimestampKey, std::to_string(std::time(nullptr)));

  if (logger) {
    std::ostringstream stream;
    stream << "save_spans: 保存成功 [来源:" << source_name << "] [输入:"
           << input << "] [分割点:" << vertices_str << "]";
    AIPARA_LOG_INFO(*logger, stream.str());
  }
  return true;
}

std::optional<SpansInfo> GetSpans(Context* context) {
  if (!context) {
    return std::nullopt;
  }
  SpansInfo info;
  info.vertices_str = context->get_property(kSpansVerticesKey);
  info.input = context->get_property(kSpansInputKey);
  info.source = context->get_property(kSpansSourceKey);
  info.timestamp = context->get_property(kSpansTimestampKey);

  if (info.vertices_str.empty() || info.input.empty()) {
    return std::nullopt;
  }

  info.vertices = ParseVerticesString(info.vertices_str);
  return info;
}

std::vector<std::size_t> ParseVerticesString(const std::string& vertices_str) {
  std::vector<std::size_t> vertices;
  if (vertices_str.empty()) {
    return vertices;
  }
  std::size_t pos = 0;
  while (pos <= vertices_str.size()) {
    const std::size_t comma = vertices_str.find(',', pos);
    const std::string token =
        comma == std::string::npos ? vertices_str.substr(pos)
                                   : vertices_str.substr(pos, comma - pos);
    if (!token.empty()) {
      try {
        vertices.push_back(static_cast<std::size_t>(std::stoul(token)));
      } catch (...) {
        // ignore malformed entries
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return vertices;
}

void ClearSpans(Context* context,
                const std::string& reason,
                Logger* logger) {
  if (!context) {
    if (logger) {
      AIPARA_LOG_ERROR(*logger, "clear_spans: context不能为空");
    }
    return;
  }

  if (logger) {
    if (auto existing = GetSpans(context)) {
      std::ostringstream stream;
      stream << "clear_spans: 清除spans信息 [原因:" << reason << "] [原输入:"
             << existing->input << "] [原来源:" << existing->source << "]";
      AIPARA_LOG_INFO(*logger, stream.str());
    }
  }

  context->set_property(kSpansVerticesKey, "");
  context->set_property(kSpansInputKey, "");
  context->set_property(kSpansSourceKey, "");
  context->set_property(kSpansTimestampKey, "");
}

std::pair<bool, std::string> ShouldClear(
    Context* context,
    const std::optional<std::string>& current_input) {
  if (!context) {
    return {true, "context为空"};
  }

  const auto spans = GetSpans(context);
  if (!spans) {
    return {false, "无spans信息"};
  }

  const std::string input_to_check = current_input
                                         ? *current_input
                                         : context->input();

  if (input_to_check != spans->input) {
    return {true, "输入内容变化"};
  }

  if (!context->IsComposing()) {
    return {true, "组合状态结束"};
  }

  return {false, "无需清除"};
}

bool AutoClearCheck(Context* context,
                    const std::optional<std::string>& current_input,
                    Logger* logger) {
  const auto decision = ShouldClear(context, current_input);
  if (decision.first) {
    ClearSpans(context, decision.second, logger);
    return true;
  }
  return false;
}

bool ExtractAndSaveFromCandidate(Context* context,
                                 const Candidate* candidate,
                                 const std::string& input,
                                 const std::string& source,
                                 Logger* logger) {
  if (!candidate) {
    if (logger) {
      AIPARA_LOG_ERROR(*logger,
                       "extract_and_save_from_candidate: candidate不能为空");
    }
    return false;
  }

  const auto* phrase = dynamic_cast<const Phrase*>(candidate);
  if (!phrase) {
    if (logger) {
      AIPARA_LOG_DEBUG(*logger,
                       "extract_and_save_from_candidate: 候选词非Phrase类型");
    }
    return false;
  }

  rime::Spans spans = const_cast<Phrase*>(phrase)->spans();
  auto vertices = VerticesFromSpans(spans);
  if (vertices.empty()) {
    if (logger) {
      AIPARA_LOG_DEBUG(*logger,
                       "extract_and_save_from_candidate: spans中无vertices信息");
    }
    return false;
  }

  if (logger) {
    AIPARA_LOG_DEBUG(
        *logger,
        "extract_and_save_from_candidate: 候选词包含spans信息，继续处理");
    AIPARA_LOG_INFO(*logger,
                    "extract_and_save_from_candidate函数中执行save_spans");
  }
  return SaveSpans(context, vertices, input, source, logger);
}

std::optional<std::size_t> GetNextCursorPosition(Context* context,
                                                  std::size_t current_pos) {
  const auto spans = GetSpans(context);
  if (!spans || spans->vertices.empty()) {
    return std::nullopt;
  }

  const std::size_t input_length = context ? context->input().size() : 0;

  if (current_pos >= input_length) {
    if (spans->vertices.size() >= 2) {
      return spans->vertices[1];
    }
    return std::size_t{0};
  }

  for (std::size_t vertex : spans->vertices) {
    if (vertex > current_pos) {
      return vertex;
    }
  }

  return input_length;
}

std::optional<std::size_t> GetPrevCursorPosition(Context* context,
                                                  std::size_t current_pos) {
  const auto spans = GetSpans(context);
  if (!spans || spans->vertices.empty()) {
    return std::nullopt;
  }

  const std::size_t input_length = context ? context->input().size() : 0;

  if (current_pos == 0) {
    return input_length;
  }

  for (auto it = spans->vertices.rbegin(); it != spans->vertices.rend(); ++it) {
    if (*it < current_pos) {
      return *it;
    }
  }

  return std::size_t{0};
}

void DebugInfo(Context* context, Logger* logger) {
  if (!logger) {
    return;
  }
  if (auto spans = GetSpans(context)) {
    AIPARA_LOG_INFO(*logger, "=== Spans Debug Info ===");
    AIPARA_LOG_INFO(*logger, std::string("输入: ") + spans->input);
    AIPARA_LOG_INFO(*logger, std::string("来源: ") + spans->source);
    AIPARA_LOG_INFO(*logger, std::string("时间戳: ") + spans->timestamp);
    AIPARA_LOG_INFO(*logger, std::string("分割点: ") + spans->vertices_str);
    std::ostringstream stream;
    for (std::size_t i = 0; i < spans->vertices.size(); ++i) {
      if (i != 0) {
        stream << ',';
      }
      stream << spans->vertices[i];
    }
    AIPARA_LOG_INFO(*logger, std::string("分割点数组: ") + stream.str());
    AIPARA_LOG_INFO(*logger, "========================");
  } else {
    AIPARA_LOG_INFO(*logger, "=== Spans Debug Info ===");
    AIPARA_LOG_INFO(*logger, "无spans信息");
    AIPARA_LOG_INFO(*logger, "========================");
  }
}

}  // namespace rime::aipara::spans_manager
