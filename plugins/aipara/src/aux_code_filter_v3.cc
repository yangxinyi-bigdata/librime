#include "aux_code_filter_v3.h"

#include <rime/config.h>
#include <rime/translation.h>

namespace rime::aipara {

AuxCodeFilterV3::AuxCodeFilterV3(const Ticket& ticket)
    : Filter(ticket) {}

an<Translation> AuxCodeFilterV3::Apply(an<Translation> translation,
                                       CandidateList* candidates) {
  // TODO: reorder candidates based on auxiliary codes.
  return translation;
}

void AuxCodeFilterV3::UpdateCurrentConfig(Config* config) {
  if (!config) {
    return;
  }

  config->GetBool("aux_code/single_fuzhu", &single_fuzhu_);
  config->GetString("aux_code/fuzhu_mode", &fuzhu_mode_);
  config->GetString("aux_code/shuangpin_zrm_txt", &shuangpin_zrm_txt_);
  config->GetString("translator/english_mode_symbol", &english_mode_symbol_);

  aux_hanzi_code_.clear();
  aux_code_hanzi_.clear();
  // TODO: load auxiliary code tables.
}

}  // namespace rime::aipara
