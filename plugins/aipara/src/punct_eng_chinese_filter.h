#ifndef PLUGINS_AIPARA_SRC_PUNCT_ENG_CHINESE_FILTER_H_
#define PLUGINS_AIPARA_SRC_PUNCT_ENG_CHINESE_FILTER_H_

#include <rime/common.h>
#include <rime/filter.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace rime {
class Config;
}  // namespace rime

namespace rime::aipara {

class PunctEngChineseFilter : public Filter {
 public:
  explicit PunctEngChineseFilter(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

  void UpdateCurrentConfig(Config* config);

 private:
  std::string delimiter_;
  std::string cloud_convert_symbol_;
  std::unordered_set<std::string> ai_reply_tags_;
  std::unordered_set<std::string> ai_chat_triggers_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_PUNCT_ENG_CHINESE_FILTER_H_
