#ifndef PLUGINS_AIPARA_SRC_RAWENGLISH_TRANSLATOR_H_
#define PLUGINS_AIPARA_SRC_RAWENGLISH_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translator.h>

#include <string>
#include <unordered_map>
#include <vector>

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
  std::string rawenglish_delimiter_before_;
  std::string rawenglish_delimiter_after_;
  std::string delimiter_;
  bool replace_punct_enabled_ = false;
  bool single_fuzhu_ = false;
  std::string fuzhu_mode_;
  std::string english_mode_symbol_ = "`";

  std::unordered_map<std::string, std::vector<std::string>> combo_cache_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_RAWENGLISH_TRANSLATOR_H_
