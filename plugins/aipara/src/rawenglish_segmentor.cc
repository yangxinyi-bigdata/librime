#include "rawenglish_segmentor.h"

#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>

namespace rime::aipara {

RawEnglishSegmentor::RawEnglishSegmentor(const Ticket& ticket)
    : Segmentor(ticket), logger_(MakeLogger("rawenglish_segmentor")) {}

bool RawEnglishSegmentor::Proceed(Segmentation* segmentation) {
  if (!segmentation || !engine_) {
    return true;
  }

  const std::string input = segmentation->input();
  if (input.empty()) {
    return true;
  }

  Schema* schema = engine_->schema();
  Config* config = schema ? schema->config() : nullptr;
  if (config) {
    std::string symbol;
    if (config->GetString("translator/english_mode_symbol", &symbol) &&
        !symbol.empty()) {
      english_mode_symbol_ = symbol;
    }
  }

  Context* context = engine_->context();
  if (!context) {
    return true;
  }

  if (english_mode_symbol_.empty()) {
    return true;
  }

  const std::size_t symbol_len = english_mode_symbol_.size();
  const std::size_t input_size = input.size();

  std::size_t current_start = segmentation->GetCurrentStartPosition();
  if (current_start >= input_size) {
    return false;
  }

  const std::size_t remaining = input_size - current_start;
  const bool has_symbol_prefix =
      remaining > symbol_len &&
      input.compare(current_start, symbol_len, english_mode_symbol_) == 0;

  if (has_symbol_prefix) {
    const std::size_t search_from = current_start + symbol_len;
    const std::size_t closing_pos =
        input.find(english_mode_symbol_, search_from);

    if (closing_pos == std::string::npos) {
      if (context->get_property("rawenglish_prompt") != "1") {
        context->set_property("rawenglish_prompt", "1");
      }

      Segment rawenglish_segment(current_start, input_size);
      rawenglish_segment.tags.insert("single_rawenglish");
      if (segmentation->AddSegment(rawenglish_segment)) {
        segmentation->Forward();
        return false;
      }
      return true;
    }

    if (context->get_property("rawenglish_prompt") == "1") {
      context->set_property("rawenglish_prompt", "0");
    }

    const std::size_t segment_end = closing_pos + symbol_len;
    Segment rawenglish_segment(current_start, segment_end);
    rawenglish_segment.tags.insert("single_rawenglish");
    if (segmentation->AddSegment(rawenglish_segment)) {
      segmentation->Forward();
      if (segmentation->GetCurrentEndPosition() >= input_size) {
        return false;
      }
      current_start = segmentation->GetCurrentStartPosition();
    }
  }

  if (current_start >= input_size) {
    return false;
  }

  const std::string current_input = input.substr(current_start);
  std::size_t count = 0;
  for (std::size_t pos = current_input.find(english_mode_symbol_);
       pos != std::string::npos;
       pos = current_input.find(english_mode_symbol_, pos + symbol_len)) {
    ++count;
  }

  if (count % 2 == 1) {
    if (context->get_property("rawenglish_prompt") != "1") {
      context->set_property("rawenglish_prompt", "1");
    }
  } else {
    if (context->get_property("rawenglish_prompt") == "1") {
      context->set_property("rawenglish_prompt", "0");
    }
  }

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
