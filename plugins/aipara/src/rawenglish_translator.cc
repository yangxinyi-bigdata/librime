#include "rawenglish_translator.h"

#include <rime/config.h>
#include <rime/translation.h>

namespace rime::aipara {

RawEnglishTranslator::RawEnglishTranslator(const Ticket& ticket)
    : Translator(ticket) {}

an<Translation> RawEnglishTranslator::Query(const string&,
                                            const Segment&) {
  // TODO: provide raw english translation candidates.
  return nullptr;
}

void RawEnglishTranslator::UpdateCurrentConfig(Config* config) {
  combo_cache_.clear();

  if (!config) {
    return;
  }

  config->GetString("translator/rawenglish_delimiter_before",
                    &rawenglish_delimiter_before_);
  config->GetString("translator/rawenglish_delimiter_after",
                    &rawenglish_delimiter_after_);
  std::string delimiter;
  if (config->GetString("speller/delimiter", &delimiter) && !delimiter.empty()) {
    delimiter_ = delimiter.substr(0, 1);
  } else {
    delimiter_ = " ";
  }

  config->GetBool("translator/replace_punct_enabled",
                  &replace_punct_enabled_);
  config->GetBool("aux_code/single_fuzhu", &single_fuzhu_);
  config->GetString("aux_code/fuzhu_mode", &fuzhu_mode_);
  config->GetString("translator/english_mode_symbol",
                    &english_mode_symbol_);
}

}  // namespace rime::aipara
