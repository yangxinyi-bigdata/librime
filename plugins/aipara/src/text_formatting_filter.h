#ifndef PLUGINS_AIPARA_SRC_TEXT_FORMATTING_FILTER_H_
#define PLUGINS_AIPARA_SRC_TEXT_FORMATTING_FILTER_H_

#include <rime/common.h>
#include <rime/filter.h>

namespace rime::aipara {

class TextFormattingFilter : public Filter {
 public:
  explicit TextFormattingFilter(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_TEXT_FORMATTING_FILTER_H_
