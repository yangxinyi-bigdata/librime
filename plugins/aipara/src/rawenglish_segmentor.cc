#include "rawenglish_segmentor.h"

#include <rime/config.h>
#include <rime/segmentation.h>

namespace rime::aipara {

RawEnglishSegmentor::RawEnglishSegmentor(const Ticket& ticket)
    : Segmentor(ticket) {}

bool RawEnglishSegmentor::Proceed(Segmentation*) {
  // TODO: implement raw english segmentation behaviour.
  return false;
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
