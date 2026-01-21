#include "punct_eng_chinese_filter.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/config/config_types.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/segmentation.h>
#include <rime/schema.h>
#include <rime/translation.h>

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_set>

#include "common/spans_manager.h"
#include "common/text_formatting.h"

namespace rime::aipara {
namespace {

constexpr std::string_view kLoggerName = "punct_eng_chinese_filter";
constexpr std::string_view kCloudPromptError = u8"    ▶[服务端未连接] ";
constexpr std::string_view kCloudPromptStarting = u8"    ▶[云端获取中] ";
constexpr std::string_view kCloudPromptStop = u8"    ▶[云端转换完成] ";
constexpr std::string_view kCloudPromptNetworkError =
    u8"    ▶[网络未连接] ";
constexpr std::string_view kRawEnglishPrompt = u8"    ▶ [英文模式]  ";
constexpr std::string_view kSearchPrompt = u8"    ▶ [搜索模式]  ";

bool StartsWith(const std::string& text, std::string_view prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

std::string FormatComment(const std::string& cand_type,
                          const std::string& original_comment,
                          bool remove_chinese_pos) {
  if (cand_type == "web_cloud") {
    return std::string(u8"   [云输入]");
  }
  if (cand_type == "ai_cloud") {
    return std::string(u8"   [AI识别]");
  }
  constexpr std::string_view kAiCloudPrefix = "ai_cloud";
  if (cand_type.size() > kAiCloudPrefix.size() &&
      cand_type.compare(0, kAiCloudPrefix.size(), kAiCloudPrefix) == 0) {
    const std::size_t pos = cand_type.find_last_of('/');
    if (pos != std::string::npos && pos + 1 < cand_type.size()) {
      std::string suffix = cand_type.substr(pos + 1);
      std::string comment("   [");
      comment.append(suffix);
      comment.append("]");
      return comment;
    }
  }
  if (cand_type == "rawenglish_combo") {
    return std::string();
  }
  if (remove_chinese_pos && StartsWith(original_comment, "chinese_pos:")) {
    return std::string();
  }
  return original_comment;
}

std::string CloudSymbolDisplay(const std::string& symbol) {
  if (symbol == "Shift+Return") {
    return std::string(u8"⇧+回车");
  }
  if (symbol == "Return") {
    return std::string(u8"回车");
  }
  return symbol;
}

std::string MakeCloudPrompt(const std::string& symbol_display) {
  std::string prompt(u8"    ▶ [");
  prompt.append(symbol_display);
  prompt.append(u8" AI转换]  ");
  return prompt;
}

std::string MakeSpeechOptimizePrompt(const std::string& symbol_display) {
  std::string prompt(u8"    ▶ [");
  prompt.append(symbol_display);
  prompt.append(u8"AI优化]  ");
  return prompt;
}

std::string MakeSpeechOptimizeRunningPrompt() {
  return std::string(u8"    ▶ [AI优化中...]");
}

std::string MakeSpeechOptimizeDonePrompt() {
  return std::string(u8"    ▶ [AI优化完成]");
}

std::string MakeSearchPrompt(const std::string& value) {
  if (value.empty()) {
    return std::string(kSearchPrompt);
  }
  std::string prompt(u8"    ▶ [搜索模式:");
  prompt.append(value);
  prompt.append(u8"]  ");
  return prompt;
}

an<Translation> MakeTranslationFrom(const std::vector<an<Candidate>>& cands) {
  auto fifo = New<FifoTranslation>();
  for (const auto& cand : cands) {
    if (cand) {
      fifo->Append(cand);
    }
  }
  return fifo;
}

}  // namespace

PunctEngChineseFilter::PunctEngChineseFilter(const Ticket& ticket)
    : Filter(ticket), logger_(MakeLogger(std::string(kLoggerName))) {
  logger_.Clear();
}

an<Translation> PunctEngChineseFilter::Apply(an<Translation> translation,
                                             CandidateList* /*candidates*/) {
  // 检查翻译对象和引擎是否存在，如果不存在则直接返回原始翻译
  if (!translation || !engine_) {
    return translation;
  }

  // 获取输入法上下文对象
  Context* context = engine_->context();
  if (!context) {
    return translation;
  }

  // 获取配置对象
  Config* config = nullptr;
  if (engine_->schema()) {
    config = engine_->schema()->config();
  }

  // 初始化变量，用于存储配置信息
  std::string cloud_convert_symbol;  // 云转换符号
  std::unordered_set<std::string> ai_reply_tags;  // AI回复标签集合
  std::unordered_set<std::string> ai_chat_triggers;  // AI聊天触发器集合

  // 从配置中读取相关设置
  if (config) {
    // 获取云转换符号配置
    if (!config->GetString("translator/cloud_convert_symbol",
                           &cloud_convert_symbol)) {
      cloud_convert_symbol.clear();
    }

    // 获取AI助手提示配置，并构建触发器和回复标签集合
    if (an<ConfigMap> ai_prompts =
            config->GetMap("ai_assistant/ai_prompts")) {
      for (auto it = ai_prompts->begin(); it != ai_prompts->end(); ++it) {
        const std::string trigger_name = it->first;
        if (trigger_name.empty()) {
          continue;
        }
        ai_chat_triggers.insert(trigger_name);
        ai_reply_tags.insert(trigger_name + "_reply");
      }
    }
  }

  // 获取当前编辑组合和最后一个段落
  Composition& composition = context->composition();
  Segment* segment =
      composition.empty() ? nullptr : &composition.back();

  // 如果存在段落，根据不同状态设置提示信息
  if (segment) {
    // 获取各种状态标志
    const bool search_move = context->get_option("search_move");
    const std::string rawenglish_prompt =
        context->get_property("rawenglish_prompt");
    const std::string cloud_convert_flag =
        context->get_property("cloud_convert_flag");
    const std::string get_cloud_stream =
        context->get_property("get_cloud_stream");
    const std::string speech_recognition_mode =
        context->get_property("speech_recognition_mode");
    const std::string get_speech_optimize_stream =
        context->get_property("get_speech_optimize_stream");

    // 根据不同状态设置不同的提示信息
    if (search_move) {
      // 搜索模式提示
      const std::string add_search_move_str =
          context->get_property("search_move_str");
      const std::string prompt = MakeSearchPrompt(add_search_move_str);
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (rawenglish_prompt == "1") {
      // 英文模式提示
      const std::string prompt(kRawEnglishPrompt);
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (get_speech_optimize_stream == "starting") {
      const std::string prompt = MakeSpeechOptimizeRunningPrompt();
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (get_speech_optimize_stream == "stop") {
      const std::string prompt = MakeSpeechOptimizeDonePrompt();
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (speech_recognition_mode == "1") {
      const std::string prompt =
          MakeSpeechOptimizePrompt(CloudSymbolDisplay(cloud_convert_symbol));
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (get_cloud_stream == "network_error") {
      // 云端网络错误提示
      const std::string prompt(kCloudPromptNetworkError);
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (get_cloud_stream == "error") {
      // 云端错误提示
      const std::string prompt(kCloudPromptError);
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (get_cloud_stream == "starting") {
      // 云端开始提示
      const std::string prompt(kCloudPromptStarting);
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (get_cloud_stream == "stop") {
      // 云端停止提示
      const std::string prompt(kCloudPromptStop);
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    } else if (cloud_convert_flag == "1") {
      // 云端转换提示
      const std::string prompt =
          MakeCloudPrompt(CloudSymbolDisplay(cloud_convert_symbol));
      if (segment->prompt != prompt) {
        segment->prompt = prompt;
      }
    }
  }

  // 收集原始候选词
  std::vector<an<Candidate>> originals;
  while (!translation->exhausted()) {
    an<Candidate> cand = translation->Peek();
    translation->Next();
    if (cand) {
      originals.push_back(cand);
    }
  }

  // 如果没有候选词，直接返回空翻译
  if (originals.empty()) {
    return MakeTranslationFrom(originals);
  }

  // 获取用户输入
  const std::string& input = context->input();

  // 初始化状态标志：
  // - ai_reply：命中 AI 回复标签后跳过所有标点替换。
  // - ai_chat：命中 AI 聊天触发器后继续替换标点，但跳过 spans 保存。
  // - punch_flag：首个候选包含 ASCII 标点时才触发整体替换逻辑。
  bool ai_reply = false;
  bool ai_chat = false;
  bool punch_flag = false;

  // 检查第一个候选词。Lua 版本里（count == 1）就在这里判定是否豁免。
  // 这里同步实现：若首候选属于 AI 回复标签，则直接设置 ai_reply，
  // 后续候选都不会做标点替换。
  const an<Candidate>& first = originals.front();
  if (first) {
    const std::string first_type = first->type();
    if (!first_type.empty()) {
      if (ai_reply_tags.count(first_type) > 0) {
        ai_reply = true;
        AIPARA_LOG_INFO(
            logger_,
            "Detected AI reply candidate; punctuation replacement disabled. type=" +
                first_type);
      } else if (ai_chat_triggers.count(first_type) > 0) {
        ai_chat = true;
        AIPARA_LOG_INFO(
            logger_,
            "匹配到ai_chat: " + first_type);
      }
    }
    if (!ai_reply && !ai_chat) {
      const std::string& first_text = first->text();
      if (text_formatting::HasPunctuationNoRawEnglish(first_text, &logger_)) {
        punch_flag = true;
      }
    }
  }

  // 创建重写后的候选词列表
  std::vector<an<Candidate>> rewritten;
  rewritten.reserve(originals.size());

  // 计数器，用于限制处理的候选词数量
  std::size_t count = 0;

  try {
    // 遍历所有原始候选词。
    // 注意：ai_reply 为 true 时直接走到 else 分支，保持原样输出。
    for (const auto& cand : originals) {
      ++count;
      // 获取候选词的各种属性
      const std::string cand_type = cand->type();
      const std::string cand_text = cand->text();
      const std::string cand_comment = cand->comment();
      const bool has_chinese_pos = StartsWith(cand_comment, "chinese_pos:");
      // 判断是否需要转换标点符号：
      // - ai_reply 为 true 表示命中 AI 回复豁免，保持原文。
      // - punch_flag 为 true 才代表首候选包含 ASCII 标点。
      // - count < 10 避免无意义地处理过多候选。
      const bool convert = !ai_reply && punch_flag && count < 10;

      if (convert) {
        // 需要转换标点符号
        std::string new_text = cand_text;
        if (has_chinese_pos) {
          // 如果有中文词性标注，尝试跳过词性标注进行替换
          if (auto replaced = text_formatting::ReplacePunctSkipPos(
                  cand_text, cand_comment, &logger_)) {
            new_text = *replaced;
          } else {
            new_text = text_formatting::ReplacePunct(cand_text);
          }
        } else {
          // 没有中文词性标注，直接替换标点符号
          new_text = text_formatting::ReplacePunct(cand_text);
          // 如果不是AI聊天，保存候选词的跨度信息
          if (!ai_chat) {
            auto genuine = Candidate::GetGenuineCandidate(cand);
            spans_manager::ExtractAndSaveFromCandidate(
                context,
                genuine.get(),
                input,
                "punct_eng_chinese_filter",
                &logger_);
          }
        }

        // 格式化注释
        std::string comment =
            FormatComment(cand_type, cand_comment, /*remove_chinese_pos=*/false);

        // 设置候选词类型
        std::string type = cand_type;
        if (type.empty()) {
          type = "punct_converted";
        }

        // 创建新的候选词对象
        auto replaced_cand = New<SimpleCandidate>(
            type,
            cand->start(),
            cand->end(),
            new_text,
            comment,
            cand->preedit());
        replaced_cand->set_quality(cand->quality());
        rewritten.push_back(replaced_cand);
      } else {
        // 不需要转换标点符号
        if (ai_reply && count == 1) {
          AIPARA_LOG_INFO(
              logger_,
              "AI reply exemption active; emitting original candidate text.");
        }
        std::string comment =
            FormatComment(cand_type,
                          cand_comment,
                          /*remove_chinese_pos=*/has_chinese_pos);
        if (comment != cand_comment) {
          // 如果注释发生变化，创建影子候选词
          auto shadow = New<ShadowCandidate>(
              cand,
              cand->type(),
              std::string(),
              comment,
              false);
          rewritten.push_back(shadow);
        } else {
          // 注释没有变化，直接使用原候选词
          rewritten.push_back(cand);
        }
      }
    }
  } catch (const std::exception& e) {
    // 捕获并处理异常，记录错误日志并返回原始候选词
    AIPARA_LOG_ERROR(logger_, std::string("punctuation filter error: ") + e.what());
    return MakeTranslationFrom(originals);
  } catch (...) {
    // 捕获未知异常，记录错误日志并返回原始候选词
    AIPARA_LOG_ERROR(logger_, "punctuation filter encountered unknown error.");
    return MakeTranslationFrom(originals);
  }

  // 记录处理完成日志
  AIPARA_LOG_INFO(logger_, "punctuation filter processed candidates.");
  // 返回处理后的候选词列表
  return MakeTranslationFrom(rewritten);
}

}  // namespace rime::aipara
