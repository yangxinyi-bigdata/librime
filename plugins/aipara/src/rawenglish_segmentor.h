#ifndef PLUGINS_AIPARA_SRC_RAWENGLISH_SEGMENTOR_H_
#define PLUGINS_AIPARA_SRC_RAWENGLISH_SEGMENTOR_H_

#include <rime/common.h>
#include <rime/segmentor.h>

#include <string>

#include "common/logger.h"

namespace rime {
class Config;
class Segmentation;
}  // namespace rime

namespace rime::aipara {

class RawEnglishSegmentor : public Segmentor {
 public:
 explicit RawEnglishSegmentor(const Ticket& ticket);

  bool Proceed(Segmentation* segmentation) override;

  void UpdateCurrentConfig(Config* config);

 private:
  std::string english_mode_symbol_ = "`";
  Logger logger_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_RAWENGLISH_SEGMENTOR_H_
