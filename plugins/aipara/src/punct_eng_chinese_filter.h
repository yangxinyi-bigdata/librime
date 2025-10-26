#ifndef PLUGINS_AIPARA_SRC_PUNCT_ENG_CHINESE_FILTER_H_
#define PLUGINS_AIPARA_SRC_PUNCT_ENG_CHINESE_FILTER_H_

#include <rime/common.h>
#include <rime/filter.h>

#include "common/logger.h"

namespace rime::aipara {

class PunctEngChineseFilter : public Filter {
 public:
  explicit PunctEngChineseFilter(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

 private:
  Logger logger_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_PUNCT_ENG_CHINESE_FILTER_H_
