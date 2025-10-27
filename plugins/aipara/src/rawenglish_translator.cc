#include "rawenglish_translator.h"

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/translation.h>
#include <rime/translator.h>
#include <rime/ticket.h>
#include <rime/gear/translator_commons.h>

#include <algorithm>
#include <functional>
#include <sstream>
#include <utility>

#include "common/spans_manager.h"
#include "common/text_formatting.h"

namespace rime::aipara {

// 候选批次结构体，用于存储一组候选词及相关状态信息
struct RawEnglishTranslator::CandidateBatch {
  std::vector<CachedCandidate> candidates;  // 候选词列表
  bool used_fallback = false;               // 是否使用了回退机制
  std::size_t fallback_length_diff = 0;     // 回退时的长度差异
  std::size_t script_fail_length = 0;       // 脚本翻译失败的长度
};

namespace {

// 每个段落最大候选词数量
constexpr std::size_t kMaxCandidatesPerSegment = 2;
// 最大输出候选词数量
constexpr std::size_t kMaxOutputCandidates = 4;

// 计算UTF-8字符串的实际字符长度（非字节长度）
std::size_t Utf8Length(const std::string& text) {
  std::size_t count = 0;
  for (unsigned char ch : text) {
    if ((ch & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

// 检查文本是否包含被跟踪的标点符号
bool ContainsTrackedPunctuation(const std::string& text) {
  static const std::string kTracked = ",.!?;:()[]<>/_=+*&^%$#@~|-'\"";
  return std::any_of(text.begin(), text.end(), [](char ch) {
    return kTracked.find(ch) != std::string::npos;
  });
}

// 移除文本中的被跟踪标点符号
std::string StripTrackedPunctuation(const std::string& text) {
  static const std::string kTracked = ",.!?;:()[]<>/_=+*&^%$#@~|-'\"";
  std::string result;
  result.reserve(text.size());
  for (char ch : text) {
    if (kTracked.find(ch) == std::string::npos) {
      result.push_back(ch);
    }
  }
  return result;
}

// 从候选词中提取跨度信息
Spans ExtractSpansFromCandidate(const an<Candidate>& cand) {
  Spans spans;
  if (!cand) {
    return spans;
  }
  auto genuine = Candidate::GetGenuineCandidate(cand);
  if (auto phrase = As<Phrase>(genuine)) {
    spans = phrase->spans();
  }
  return spans;
}

// 从跨度信息中提取顶点位置
std::vector<std::size_t> VerticesFromSpans(const Spans& spans) {
  std::vector<std::size_t> vertices;
  const std::size_t first = spans.start();
  if (!spans.HasVertex(first)) {
    return vertices;
  }
  vertices.push_back(first);
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

}  // namespace

// RawEnglishTranslator构造函数，初始化翻译器和日志记录器
RawEnglishTranslator::RawEnglishTranslator(const Ticket& ticket)
    : Translator(ticket), logger_(MakeLogger("rawenglish_translator")) {
  EnsureTranslators();
}

// 确保翻译器组件已正确初始化
// 此函数负责初始化和检查两个关键的翻译器组件：
// 1. script_translator_ - 用于处理脚本翻译的主要翻译器
// 2. user_dict_set_translator_ - 用于处理用户词典集的翻译器
// 这些翻译器是处理混合输入（英文+中文）的核心组件
void RawEnglishTranslator::EnsureTranslators() {
  // 检查引擎是否存在，如果不存在则重置所有翻译器并返回
  if (!engine_) {
    script_translator_.reset();
    user_dict_set_translator_.reset();
    return;
  }

  // 获取script_translator组件，这是Rime输入法中处理脚本翻译的核心组件
  auto component = Translator::Require("script_translator");
  if (!component) {
    // 如果组件不可用，记录警告并重置所有翻译器
    AIPARA_LOG_WARN(logger_, "script_translator component unavailable.");
    script_translator_.reset();
    user_dict_set_translator_.reset();
    return;
  }

  // 初始化脚本翻译器（如果尚未初始化）
  if (!script_translator_) {
    // 创建脚本翻译器的票据，包含引擎、类型和名称信息
    Ticket script_ticket(engine_, "translator", "script_translator");
    // 使用组件创建脚本翻译器实例
    script_translator_.reset(component->Create(script_ticket));
    if (script_translator_) {
      // 成功创建后记录信息日志
      AIPARA_LOG_INFO(logger_, "script_translator initialized.");
    } else {
      // 创建失败则记录警告日志
      AIPARA_LOG_WARN(logger_, "failed to create script_translator instance.");
    }
  }

  // 初始化用户词典集翻译器（如果尚未初始化）
  if (!user_dict_set_translator_) {
    // 创建用户词典集翻译器的票据
    Ticket user_ticket(engine_, "user_dict_set", "script_translator");
    // 使用相同的script_translator组件创建用户词典集翻译器实例
    user_dict_set_translator_.reset(component->Create(user_ticket));
    if (user_dict_set_translator_) {
      // 成功创建后记录信息日志
      AIPARA_LOG_INFO(logger_, "user_dict_set script_translator initialized.");
    } else {
      // 创建失败则记录警告日志
      AIPARA_LOG_WARN(logger_,
                      "failed to create user_dict_set script_translator.");
    }
  }
}

// 重置翻译器状态
void RawEnglishTranslator::ResetState() {
  rawenglish_delimiter_before_.clear();
  rawenglish_delimiter_after_.clear();
  delimiter_ = " ";
  replace_punct_enabled_ = false;
  single_fuzhu_ = false;
  fuzhu_mode_.clear();
  english_mode_symbol_ = "`";
  combo_cache_.clear();
}

// 从配置文件中加载设置
void RawEnglishTranslator::LoadConfig(Config* config) {
  ResetState();
  if (!config) {
    return;
  }

  // 加载原始英文前后分隔符
  config->GetString("translator/rawenglish_delimiter_before",
                    &rawenglish_delimiter_before_);
  config->GetString("translator/rawenglish_delimiter_after",
                    &rawenglish_delimiter_after_);

  // 加载分隔符设置
  std::string delimiter;
  if (config->GetString("speller/delimiter", &delimiter) &&
      !delimiter.empty()) {
    delimiter_ = delimiter.substr(0, 1);
  }

  // 加载其他配置选项
  config->GetBool("translator/replace_punct_enabled", &replace_punct_enabled_);
  config->GetBool("aux_code/single_fuzhu", &single_fuzhu_);
  config->GetString("aux_code/fuzhu_mode", &fuzhu_mode_);

  // 加载英文模式符号
  std::string english_symbol;
  if (config->GetString("translator/english_mode_symbol", &english_symbol) &&
      !english_symbol.empty()) {
    english_mode_symbol_ = english_symbol;
  }
}

// 确保配置已加载，如果方案ID发生变化则重新加载
void RawEnglishTranslator::EnsureConfigLoaded() {
  if (!engine_) {
    ResetState();
    config_loaded_ = false;
    last_schema_id_.clear();
    return;
  }

  Schema* schema = engine_->schema();
  if (!schema) {
    ResetState();
    config_loaded_ = false;
    last_schema_id_.clear();
    return;
  }

  const std::string schema_id = schema->schema_id();
  if (!config_loaded_ || schema_id != last_schema_id_) {
    LoadConfig(schema->config());
    config_loaded_ = schema->config() != nullptr;
    last_schema_id_ = schema_id;
  }
}

// 查询输入并生成翻译结果
an<Translation> RawEnglishTranslator::Query(const string& input,
                                            const Segment& segment) {
  EnsureConfigLoaded();
  EnsureTranslators();

  if (!engine_) {
    return nullptr;
  }

  Context* context = engine_->context();
  if (!context) {
    return nullptr;
  }

  const std::string context_input = context->input();
  spans_manager::AutoClearCheck(context, context_input, &logger_);

  // 处理单个字符输入（英文模式符号）
  if (input.size() == 1) {
    if (input == "`") {
      auto translation = New<FifoTranslation>();
      if (!translation) {
        return nullptr;
      }
      auto cand_markdown =
          New<SimpleCandidate>("punct", segment.start, segment.end,
                               "```\n\n```", std::string(), "`");
      auto backquote_symbol = New<SimpleCandidate>(
          "punct", segment.start, segment.end, "`", std::string(), "`");
      auto three_backquote_symbol = New<SimpleCandidate>(
          "punct", segment.start, segment.end, "```", std::string(), "`");
      translation->Append(backquote_symbol);
      translation->Append(cand_markdown);
      translation->Append(three_backquote_symbol);
      return translation;
    }
    return nullptr;
  }

  // 检查段落是否具有正确的标签
  if (!segment.HasTag("rawenglish_combo") &&
      !segment.HasTag("single_rawenglish")) {
    return nullptr;
  }

  // 处理单个原始英文输入
  if (segment.HasTag("single_rawenglish")) {
    const std::size_t symbol_len = english_mode_symbol_.size();
    if (input.size() <= symbol_len) {
      return nullptr;
    }

    std::string inner_content;
    if (symbol_len > 0 && input.compare(input.size() - symbol_len, symbol_len,
                                        english_mode_symbol_) == 0) {
      inner_content = input.substr(symbol_len, input.size() - 2 * symbol_len);
    } else {
      inner_content = input.substr(symbol_len);
    }

    std::string replaced;
    if (rawenglish_delimiter_before_ == " " &&
        rawenglish_delimiter_after_ == " ") {
      replaced = inner_content + rawenglish_delimiter_after_;
    } else {
      replaced = rawenglish_delimiter_before_ + inner_content +
                 rawenglish_delimiter_after_;
    }

    auto translation = New<FifoTranslation>();
    if (!translation) {
      return nullptr;
    }
    auto candidate =
        New<SimpleCandidate>("single_rawenglish", segment.start, segment.end,
                             replaced, std::string(), input);
    translation->Append(candidate);
    return translation;
  }

  // 检查脚本翻译器是否可用
  if (!script_translator_) {
    AIPARA_LOG_WARN(logger_,
                    "script_translator unavailable, cannot handle combo.");
    return nullptr;
  }

  // 按原始英文分割文本段落
  const auto text_segments = text_formatting::SplitByRawEnglishWithLog(
      input, segment.start, segment.end, rawenglish_delimiter_before_,
      rawenglish_delimiter_after_, &logger_);

  if (text_segments.empty()) {
    AIPARA_LOG_WARN(logger_, "split_by_rawenglish returned empty result.");
    return nullptr;
  }

  // 处理只有原始英文组合的情况
  if (!text_segments.empty() &&
      text_segments.front().type == "rawenglish_combo") {
    const auto& first = text_segments.front();
    auto translation = New<FifoTranslation>();
    if (!translation) {
      return nullptr;
    }
    std::ostringstream comment;
    comment << "chinese_pos:" << first.end << "," << first.end << ",";
    auto candidate = New<SimpleCandidate>(
        "rawenglish_combo", segment.start, first.end, first.content,
        comment.str(), first.original.empty() ? first.content : first.original);
    translation->Append(candidate);
    return translation;
  }

  const std::size_t seg_count = text_segments.size();
  std::vector<std::vector<CachedCandidate>> segment_candidates(seg_count);

  bool delete_last_code = false;
  bool used_fallback = false;
  std::size_t fallback_length_diff = 0;
  std::size_t script_fail_code = 0;

  // 收集ABC候选词的Lambda函数
  auto collect_abc_candidates =
      [&](const std::string& query_content,
          const text_formatting::TextSegment& text_segment,
          std::size_t query_length, bool allow_fallback) -> CandidateBatch {
    CandidateBatch batch;
    if (query_content.empty()) {
      RawEnglishTranslator::CachedCandidate candidate;
      candidate.text = query_content;
      candidate.preedit = query_content;
      candidate.start = text_segment.start;
      candidate.end = text_segment.start + query_length;
      candidate.length = text_segment.length;
      candidate.type = text_segment.type;
      Spans spans;
      spans.AddSpan(candidate.start, candidate.end);
      candidate.spans = spans;
      batch.candidates.push_back(std::move(candidate));
      return batch;
    }

    // 创建脚本段落并设置标签
    Segment script_segment(text_segment.start,
                           text_segment.start + query_length);
    script_segment.tags.insert("abc");

    // 创建候选词的Lambda函数
    const auto make_candidate = [&](const an<Candidate>& cand,
                                    std::size_t cand_length) {
      CachedCandidate cached;
      cached.text = cand->text();
      const std::string preedit = cand->preedit();
      cached.preedit = preedit.empty() ? query_content : preedit;
      cached.spans = ExtractSpansFromCandidate(cand);
      cached.start = text_segment.start;
      cached.end = text_segment.start + cand_length;
      cached.length = text_segment.length;
      cached.type = text_segment.type;
      return cached;
    };

    std::vector<CachedCandidate> valid;
    std::vector<std::pair<CachedCandidate, std::size_t>> fallback;

    // 从翻译器收集候选词的Lambda函数
    const auto collect_from = [&](const an<Translator>& translator) {
      if (!translator) {
        return;
      }
      an<Translation> translation =
          translator->Query(query_content, script_segment);
      if (!translation) {
        return;
      }
      std::size_t enumerated = 0;
      while (!translation->exhausted() &&
             enumerated < kMaxCandidatesPerSegment) {
        an<Candidate> cand = translation->Peek();
        if (!cand) {
          break;
        }
        const std::size_t cand_length = cand->end() - cand->start();
        CachedCandidate cached = make_candidate(cand, cand_length);
        if (cand_length == query_length) {
          valid.push_back(std::move(cached));
          if (valid.size() >= kMaxCandidatesPerSegment) {
            break;
          }
        } else if (allow_fallback) {
          fallback.emplace_back(std::move(cached), cand_length);
        }
        ++enumerated;
        if (!translation->Next()) {
          break;
        }
      }
    };

    // 从脚本翻译器和用户词典集翻译器收集候选词
    collect_from(script_translator_);
    if (valid.size() < kMaxCandidatesPerSegment) {
      collect_from(user_dict_set_translator_);
    }

    // 处理没有有效候选词的情况
    if (valid.empty()) {
      if (allow_fallback && !fallback.empty()) {
        std::stable_sort(fallback.begin(), fallback.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.second > rhs.second;
                         });
        batch.used_fallback = true;
        batch.fallback_length_diff =
            text_segment.length > fallback.front().second
                ? text_segment.length - fallback.front().second
                : 0;
        for (std::size_t i = 0;
             i < fallback.size() && i < kMaxCandidatesPerSegment; ++i) {
          batch.candidates.push_back(fallback[i].first);
        }
        return batch;
      }

      if (!allow_fallback) {
        CachedCandidate fallback_candidate;
        fallback_candidate.text = text_segment.content;
        fallback_candidate.preedit = text_segment.content;
        fallback_candidate.start = text_segment.start;
        fallback_candidate.end = text_segment.end;
        fallback_candidate.length = text_segment.length;
        fallback_candidate.type = text_segment.type;
        Spans spans;
        spans.AddSpan(text_segment.start, text_segment.end);
        fallback_candidate.spans = spans;
        batch.candidates.push_back(std::move(fallback_candidate));
      } else {
        batch.script_fail_length = text_segment.length;
      }
      return batch;
    }

    batch.candidates = std::move(valid);
    return batch;
  };

  // 处理除最后一个段落外的所有段落
  for (std::size_t i = 0; i + 1 < seg_count; ++i) {
    const auto& text_segment = text_segments[i];
    const std::string cache_key = !text_segment.original.empty()
                                      ? text_segment.original
                                      : text_segment.content;
    auto cache_it = combo_cache_.find(cache_key);
    if (cache_it != combo_cache_.end()) {
      segment_candidates[i] = cache_it->second;
      continue;
    }

    std::vector<CachedCandidate> candidates_for_segment;
    if (text_segment.type == "abc") {
      CandidateBatch batch =
          collect_abc_candidates(text_segment.content, text_segment,
                                 text_segment.content.size(), false);
      candidates_for_segment = std::move(batch.candidates);
    } else if (text_segment.type == "rawenglish_combo") {
      CachedCandidate candidate;
      candidate.text = text_segment.content;
      candidate.preedit = text_segment.original.empty() ? text_segment.content
                                                        : text_segment.original;
      candidate.start = text_segment.start;
      candidate.end = text_segment.end;
      candidate.length = text_segment.length;
      candidate.type = text_segment.type;
      segment_candidates[i].push_back(candidate);
      combo_cache_[cache_key] = segment_candidates[i];
      continue;
    } else {
      CachedCandidate candidate;
      candidate.text = text_segment.content;
      candidate.preedit = text_segment.content;
      candidate.start = text_segment.start;
      candidate.end = text_segment.end;
      candidate.length = text_segment.length;
      candidate.type = text_segment.type;
      Spans spans;
      spans.AddSpan(text_segment.start, text_segment.end);
      candidate.spans = spans;
      candidates_for_segment.push_back(std::move(candidate));
    }

    segment_candidates[i] = candidates_for_segment;
    combo_cache_[cache_key] = segment_candidates[i];
  }

  // 处理最后一个段落
  if (!text_segments.empty()) {
    const std::size_t i = seg_count - 1;
    const auto& text_segment = text_segments[i];
    std::string query_content = text_segment.content;

    // 处理辅助码模式下的特殊逻辑
    if (text_segment.type == "abc" && single_fuzhu_ && fuzhu_mode_ == "all") {
      if (ContainsTrackedPunctuation(query_content)) {
        const std::string stripped = StripTrackedPunctuation(query_content);
        if (!stripped.empty() && stripped.size() % 2 == 1 &&
            stripped.size() != 1 && !query_content.empty()) {
          query_content.pop_back();
          delete_last_code = true;
        }
      } else {
        if (query_content.size() % 2 == 1 && query_content.size() != 1) {
          query_content.pop_back();
          delete_last_code = true;
        }
      }
    }

    if (text_segment.type == "abc") {
      CandidateBatch batch = collect_abc_candidates(query_content, text_segment,
                                                    query_content.size(), true);
      segment_candidates[i] = batch.candidates;
      if (batch.used_fallback) {
        used_fallback = true;
        fallback_length_diff = batch.fallback_length_diff;
      }
      if (batch.script_fail_length > 0) {
        script_fail_code = batch.script_fail_length;
      }
    } else if (text_segment.type == "rawenglish_combo") {
      CachedCandidate candidate;
      candidate.text = text_segment.content;
      candidate.preedit = text_segment.original.empty() ? text_segment.content
                                                        : text_segment.original;
      candidate.start = text_segment.start;
      candidate.end = text_segment.end;
      candidate.length = text_segment.length;
      candidate.type = text_segment.type;
      segment_candidates[i].push_back(candidate);
    } else {
      CachedCandidate candidate;
      candidate.text = text_segment.content;
      candidate.preedit = text_segment.content;
      candidate.start = text_segment.start;
      candidate.end = text_segment.end;
      candidate.length = text_segment.length;
      candidate.type = text_segment.type;
      Spans spans;
      spans.AddSpan(text_segment.start, text_segment.end);
      candidate.spans = spans;
      segment_candidates[i].push_back(candidate);
    }

    const std::string cache_key = !text_segment.original.empty()
                                      ? text_segment.original
                                      : text_segment.content;
    combo_cache_[cache_key] = segment_candidates[i];
  }

  // 检查所有段落是否有候选词
  for (const auto& candidates : segment_candidates) {
    if (candidates.empty()) {
      return nullptr;
    }
  }

  // 创建指向候选词的指针数组
  std::vector<std::vector<const CachedCandidate*>> pointer_segments;
  pointer_segments.reserve(segment_candidates.size());
  for (auto& candidates : segment_candidates) {
    std::vector<const CachedCandidate*> pointers;
    pointers.reserve(candidates.size());
    for (const auto& candidate : candidates) {
      pointers.push_back(&candidate);
    }
    pointer_segments.push_back(std::move(pointers));
  }

  // 生成所有可能的组合
  std::vector<std::vector<const CachedCandidate*>> all_combinations;
  std::vector<const CachedCandidate*> current;
  std::function<void(std::size_t)> generate = [&](std::size_t index) {
    if (index >= pointer_segments.size()) {
      all_combinations.push_back(current);
      return;
    }
    for (const CachedCandidate* candidate : pointer_segments[index]) {
      current.push_back(candidate);
      generate(index + 1);
      current.pop_back();
    }
  };

  generate(0);

  if (all_combinations.empty()) {
    return nullptr;
  }

  // 创建翻译结果
  auto translation = New<FifoTranslation>();
  if (!translation) {
    return nullptr;
  }

  bool produced = false;
  std::size_t output_count = 0;
  Spans aggregated_spans;
  bool aggregated_initialized = false;

  // 处理所有组合，生成最终候选词
  for (const auto& combination : all_combinations) {
    if (output_count >= kMaxOutputCandidates) {
      break;
    }

    std::string final_text;
    std::string final_preedit;
    std::string chinese_pos = "chinese_pos:";
    std::size_t text_len_counter = 0;

    if (output_count == 0) {
      aggregated_spans = Spans();
      aggregated_initialized = false;
    }

    // 组合候选词文本和预编辑
    for (std::size_t idx = 0; idx < combination.size(); ++idx) {
      const CachedCandidate& candidate = *combination[idx];
      final_text += candidate.text;
      final_preedit +=
          candidate.preedit.empty() ? candidate.text : candidate.preedit;

      if (candidate.type == "abc") {
        const std::size_t start_pos = text_len_counter + 1;
        text_len_counter += Utf8Length(candidate.text);
        const std::size_t end_pos = text_len_counter;
        std::ostringstream pos_stream;
        pos_stream << start_pos << ',' << end_pos << ',';
        chinese_pos.append(pos_stream.str());

        if (output_count == 0) {
          if (!aggregated_initialized) {
            aggregated_spans = candidate.spans;
            aggregated_initialized = true;
          } else {
            const auto vertices = VerticesFromSpans(candidate.spans);
            for (std::size_t vertex : vertices) {
              aggregated_spans.AddVertex(candidate.start + vertex);
            }
          }
        }
      } else {
        text_len_counter += candidate.text.size();
        if (output_count == 0) {
          if (!aggregated_initialized) {
            aggregated_spans = candidate.spans;
            aggregated_initialized = true;
          } else {
            aggregated_spans.AddSpan(candidate.start, candidate.end);
          }
        }
      }
    }

    // 保存跨度信息
    if (output_count == 0) {
      const auto vertices = VerticesFromSpans(aggregated_spans);
      if (!vertices.empty()) {
        spans_manager::SaveSpans(context, vertices, context_input,
                                 "rawenglish_translator", &logger_);
      }
    }

    // 创建最终候选词
    if (!final_text.empty() && final_text != input) {
      std::size_t candidate_end = segment.end;
      if (delete_last_code && candidate_end > segment.start) {
        --candidate_end;
      }
      if (script_fail_code > 0) {
        if (candidate_end > script_fail_code) {
          candidate_end -= script_fail_code;
        } else {
          candidate_end = segment.start;
        }
      }
      if (used_fallback && fallback_length_diff > 0) {
        if (segment.end >= fallback_length_diff) {
          candidate_end = segment.end - fallback_length_diff;
        } else {
          candidate_end = segment.start;
        }
      }

      std::string comment;
      if (text_formatting::HasPunctuationNoRawEnglish(final_text, &logger_)) {
        comment = chinese_pos;
      }

      auto candidate =
          New<SimpleCandidate>("rawenglish_combo", segment.start, candidate_end,
                               final_text, comment, final_preedit);
      translation->Append(candidate);
      ++output_count;
      produced = true;
    }
  }

  if (!produced) {
    return nullptr;
  }

  return translation;
}

// 更新当前配置
void RawEnglishTranslator::UpdateCurrentConfig(Config* config) {
  LoadConfig(config);
  config_loaded_ = (config != nullptr);
  last_schema_id_.clear();
}

}  // namespace rime::aipara