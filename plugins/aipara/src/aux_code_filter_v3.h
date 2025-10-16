#ifndef PLUGINS_AIPARA_SRC_AUX_CODE_FILTER_V3_H_
#define PLUGINS_AIPARA_SRC_AUX_CODE_FILTER_V3_H_

#include <rime/common.h>
#include <rime/filter.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace rime {
class Config;
}  // namespace rime

namespace rime::aipara {

class AuxCodeFilterV3 : public Filter {
 public:
  explicit AuxCodeFilterV3(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

  void UpdateCurrentConfig(Config* config);

 private:
  bool single_fuzhu_ = false;
  std::string fuzhu_mode_;
  std::string shuangpin_zrm_txt_;
  std::string english_mode_symbol_;

  bool set_fuzhuma_ = false;

  std::unordered_map<std::string, std::vector<std::string>> aux_hanzi_code_;
  std::unordered_map<std::string, std::vector<std::string>> aux_code_hanzi_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AUX_CODE_FILTER_V3_H_
