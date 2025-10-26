#include "rawenglish_segmentor.h"

#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>

namespace rime::aipara {

RawEnglishSegmentor::RawEnglishSegmentor(const Ticket& ticket)
    : Segmentor(ticket), logger_(MakeLogger("rawenglish_segmentor")) {}

/**
 * @brief 处理原始英文输入的分割逻辑
 * 
 * 该函数是原始英文分割器的核心方法，负责识别和分割包含英文模式符号的输入文本。
 * 主要功能包括：
 * 1. 从配置中加载英文模式符号（默认为反引号 `）
 * 2. 识别以英文模式符号开头和结尾的文本段
 * 3. 处理未闭合的英文模式符号（只有开始符号没有结束符号）
 * 4. 根据符号出现次数设置原始英文提示状态
 * 5. 创建带有相应标签的文本段，供后续处理使用
 * 
 * @param segmentation Rime输入法的分割对象，包含当前输入状态和分割结果
 * @return bool 返回true表示继续处理，返回false表示停止处理
 */
bool RawEnglishSegmentor::Proceed(Segmentation* segmentation) {
  // 验证分割对象和引擎指针的有效性
  if (!segmentation || !engine_) {
    return true;
  }

  // 获取当前输入文本
  const std::string input = segmentation->input();
  if (input.empty()) {
    return true;
  }

  // 从配置中获取英文模式符号
  Schema* schema = engine_->schema();
  Config* config = schema ? schema->config() : nullptr;
  if (config) {
    std::string symbol;
    if (config->GetString("translator/english_mode_symbol", &symbol) &&
        !symbol.empty()) {
      english_mode_symbol_ = symbol;
    }
  }

  // 获取输入上下文
  Context* context = engine_->context();
  if (!context) {
    return true;
  }

  // 如果英文模式符号为空，则直接返回
  if (english_mode_symbol_.empty()) {
    return true;
  }

  // 计算符号长度和输入文本长度
  const std::size_t symbol_len = english_mode_symbol_.size();
  const std::size_t input_size = input.size();

  // 获取当前分割起始位置
  std::size_t current_start = segmentation->GetCurrentStartPosition();
  if (current_start >= input_size) {
    return false;
  }

  // 计算剩余文本长度并检查是否有英文模式符号前缀
  const std::size_t remaining = input_size - current_start;
  const bool has_symbol_prefix =
      remaining > symbol_len &&
      input.compare(current_start, symbol_len, english_mode_symbol_) == 0;

  // 处理以英文模式符号开头的文本段
  if (has_symbol_prefix) {
    // 从符号后开始查找闭合符号
    const std::size_t search_from = current_start + symbol_len;
    const std::size_t closing_pos =
        input.find(english_mode_symbol_, search_from);

    // 如果找不到闭合符号，处理未闭合的英文模式
    if (closing_pos == std::string::npos) {
      // 设置原始英文提示状态为"1"（表示需要提示）
      if (context->get_property("rawenglish_prompt") != "1") {
        context->set_property("rawenglish_prompt", "1");
      }

      // 创建从当前位置到输入结束的原始英文段
      Segment rawenglish_segment(current_start, input_size);
      rawenglish_segment.tags.insert("single_rawenglish");
      if (segmentation->AddSegment(rawenglish_segment)) {
        segmentation->Forward();
        return false;
      }
      return true;
    }

    // 如果找到闭合符号，清除原始英文提示状态
    if (context->get_property("rawenglish_prompt") == "1") {
      context->set_property("rawenglish_prompt", "0");
    }

    // 创建从开始符号到闭合符号的原始英文段
    const std::size_t segment_end = closing_pos + symbol_len;
    Segment rawenglish_segment(current_start, segment_end);
    rawenglish_segment.tags.insert("single_rawenglish");
    if (segmentation->AddSegment(rawenglish_segment)) {
      segmentation->Forward();
      // 如果已处理到输入末尾，则返回false
      if (segmentation->GetCurrentEndPosition() >= input_size) {
        return false;
      }
      current_start = segmentation->GetCurrentStartPosition();
    }
  }

  // 再次检查当前位置是否超出输入范围
  if (current_start >= input_size) {
    return false;
  }

  // 获取当前位置开始的剩余输入
  const std::string current_input = input.substr(current_start);
  
  // 统计英文模式符号出现的次数
  std::size_t count = 0;
  for (std::size_t pos = current_input.find(english_mode_symbol_);
       pos != std::string::npos;
       pos = current_input.find(english_mode_symbol_, pos + symbol_len)) {
    ++count;
  }

  // 根据符号出现次数设置原始英文提示状态
  // 奇数次表示未闭合，偶数次表示已闭合
  if (count % 2 == 1) {
    if (context->get_property("rawenglish_prompt") != "1") {
      context->set_property("rawenglish_prompt", "1");
    }
  } else {
    if (context->get_property("rawenglish_prompt") == "1") {
      context->set_property("rawenglish_prompt", "0");
    }
  }

  // 如果当前输入包含英文模式符号，创建组合段
  if (current_input.find(english_mode_symbol_) != std::string::npos) {
    Segment combo_segment(current_start, input_size);
    combo_segment.tags.insert("rawenglish_combo");
    combo_segment.tags.insert("abc");
    if (segmentation->AddSegment(combo_segment)) {
      return false;
    }
  }

  return true;
}

void RawEnglishSegmentor::UpdateCurrentConfig(Config* config) {
  if (!config) {
    return;
  }
  std::string value;
  if (config->GetString("translator/english_mode_symbol", &value)) {
    english_mode_symbol_ = value;
  } else {
    english_mode_symbol_ = "`";
  }
}

}  // namespace rime::aipara