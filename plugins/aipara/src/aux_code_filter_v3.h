#ifndef PLUGINS_AIPARA_SRC_AUX_CODE_FILTER_V3_H_
#define PLUGINS_AIPARA_SRC_AUX_CODE_FILTER_V3_H_

#include <string>
#include <unordered_map>
#include <vector>

#include <boost/signals2/connection.hpp>
#include <rime/common.h>
#include <rime/filter.h>

#include "common/logger.h"

namespace rime {
class Config;
class Context;
}  // namespace rime

namespace rime::aipara {

class AuxCodeFilterV3 : public Filter {
 public:
  explicit AuxCodeFilterV3(const Ticket& ticket);
  ~AuxCodeFilterV3() override;

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

 private:
  void AttachContextHooks(Context* context);
  void DetachContextHooks();
  void OnSelect(Context* context);
  bool EnsureAuxTables(const std::string& txt_name);
  int MatchAuxiliaryCode(const std::string& character,
                         const std::string& match_code) const;
  an<Translation> HandleAllMode(const std::string& segment_input,
                                const std::string& last_code,
                                const std::string& last_three_code,
                                size_t current_end,
                                an<Translation> translation);
  an<Translation> HandleBeforeMode(const std::string& last_code,
                                   size_t current_end,
                                   an<Translation> translation);
  an<Translation> HandleAfterMode(const std::string& last_code,
                                  an<Translation> translation);

  Logger logger_;
  boost::signals2::connection select_connection_;
  bool set_fuzhuma_ = false;
  std::string cached_aux_code_file_;

  std::unordered_map<std::string, std::vector<std::string>> aux_hanzi_code_;
  std::unordered_map<std::string, std::vector<std::string>> aux_code_hanzi_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AUX_CODE_FILTER_V3_H_
