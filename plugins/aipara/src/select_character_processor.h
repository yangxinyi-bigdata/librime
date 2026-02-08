#ifndef PLUGINS_AIPARA_SRC_SELECT_CHARACTER_PROCESSOR_H_
#define PLUGINS_AIPARA_SRC_SELECT_CHARACTER_PROCESSOR_H_

#include <rime/common.h>
#include <rime/processor.h>

#include <string>

namespace rime {
class Config;
class Context;
class KeyEvent;
}  // namespace rime

namespace rime::aipara {

class SelectCharacterProcessor : public Processor {
 public:
  explicit SelectCharacterProcessor(const Ticket& ticket);
  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 private:
  void LoadKeyBindings(rime::Config* config);

  std::string first_key_;
  std::string last_key_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_SELECT_CHARACTER_PROCESSOR_H_
