#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "ai_assistant_segmentor.h"
#include "ai_assistant_translator.h"
#include "autocap_filter.h"
#include "aux_code_filter_v3.h"
#include "cloud_ai_filter_v2.h"
#include "cloud_input_processor.h"
#include "punct_eng_chinese_filter.h"
#include "rawenglish_segmentor.h"
#include "rawenglish_translator.h"
#include "select_character_processor.h"
#include "smart_cursor_processor.h"
#include "text_formatting_filter.h"
#include "common/tcp_zmq.h"

using rime::Component;
using rime::Registry;
using rime::aipara::AcquireGlobalTcpZmq;

static void rime_aipara_initialize() {
  AcquireGlobalTcpZmq();

  Registry& r = Registry::instance();

  r.Register("smart_cursor_processor",
             new Component<rime::aipara::SmartCursorProcessor>);
  r.Register("cloud_input_processor",
             new Component<rime::aipara::CloudInputProcessor>);
  r.Register("select_character_processor",
             new Component<rime::aipara::SelectCharacterProcessor>);

  r.Register("ai_assistant_segmentor",
             new Component<rime::aipara::AiAssistantSegmentor>);
  r.Register("rawenglish_segment",
             new Component<rime::aipara::RawEnglishSegmentor>);

  r.Register("ai_assistant_translator",
             new Component<rime::aipara::AiAssistantTranslator>);
  r.Register("rawenglish_translator",
             new Component<rime::aipara::RawEnglishTranslator>);
  r.Register("aux_code_filter_v3",
             new Component<rime::aipara::AuxCodeFilterV3>);
  r.Register("punct_eng_chinese_filter",
             new Component<rime::aipara::PunctEngChineseFilter>);
  r.Register("cloud_ai_filter_v2",
             new Component<rime::aipara::CloudAiFilterV2>);
  r.Register("autocap_filter",
             new Component<rime::aipara::AutoCapFilter>);
  r.Register("text_formatting",
             new Component<rime::aipara::TextFormattingFilter>);
}

static void rime_aipara_finalize() {}

RIME_REGISTER_MODULE(aipara)
