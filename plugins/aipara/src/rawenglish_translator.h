#ifndef PLUGINS_AIPARA_SRC_RAWENGLISH_TRANSLATOR_H_
#define PLUGINS_AIPARA_SRC_RAWENGLISH_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translator.h>
#include <rime/gear/translator_commons.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "common/logger.h"

namespace rime {
class Config;
struct Segment;
}  // namespace rime

namespace rime::aipara {

class RawEnglishTranslator : public Translator {
 public:
  explicit RawEnglishTranslator(const Ticket& ticket);

  an<Translation> Query(const string& input,
                        const Segment& segment) override;

  void UpdateCurrentConfig(Config* config);

 private:
  struct CachedCandidate {
    std::string text;
    std::string preedit;
    rime::Spans spans;
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t length = 0;
    std::string type;
  };
  struct CandidateBatch;

  void EnsureConfigLoaded();
  void LoadConfig(Config* config);
  void EnsureTranslators();
  void ResetState();

  std::string rawenglish_delimiter_before_;
  std::string rawenglish_delimiter_after_;
  std::string delimiter_;
  bool replace_punct_enabled_ = false;
  bool single_fuzhu_ = false;
  std::string fuzhu_mode_;
  std::string english_mode_symbol_ = "`";

  std::unordered_map<std::string, std::vector<CachedCandidate>> combo_cache_;

  Logger logger_;
  bool config_loaded_ = false;
  std::string last_schema_id_;
  an<Translator> script_translator_;
  an<Translator> user_dict_set_translator_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_RAWENGLISH_TRANSLATOR_H_
