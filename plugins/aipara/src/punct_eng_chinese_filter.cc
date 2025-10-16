#include "punct_eng_chinese_filter.h"

#include <rime/config.h>
#include <rime/translation.h>

namespace rime::aipara {

PunctEngChineseFilter::PunctEngChineseFilter(const Ticket& ticket)
    : Filter(ticket) {}

an<Translation> PunctEngChineseFilter::Apply(an<Translation> translation,
                                             CandidateList* candidates) {
  // TODO: convert punctuation for AI candidates.
  return translation;
}

void PunctEngChineseFilter::UpdateCurrentConfig(Config* config) {
  ai_reply_tags_.clear();
  ai_chat_triggers_.clear();

  if (!config) {
    delimiter_ = " ";
    cloud_convert_symbol_.clear();
    return;
  }

  std::string delimiter;
  if (config->GetString("speller/delimiter", &delimiter) && !delimiter.empty()) {
    delimiter_ = delimiter.substr(0, 1);
  } else {
    delimiter_ = " ";
  }

  config->GetString("translator/cloud_convert_symbol", &cloud_convert_symbol_);
  // TODO: populate AI reply tags from configuration.
}

}  // namespace rime::aipara
