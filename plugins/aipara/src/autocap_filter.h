#ifndef PLUGINS_AIPARA_SRC_AUTOCAP_FILTER_H_
#define PLUGINS_AIPARA_SRC_AUTOCAP_FILTER_H_

#include <rime/common.h>
#include <rime/filter.h>

namespace rime::aipara {

class AutoCapFilter : public Filter {
 public:
  explicit AutoCapFilter(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AUTOCAP_FILTER_H_
