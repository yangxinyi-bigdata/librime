#ifndef PLUGINS_AIPARA_SRC_SMART_CURSOR_PROCESSOR_H_
#define PLUGINS_AIPARA_SRC_SMART_CURSOR_PROCESSOR_H_

#include <rime/common.h>
#include <rime/processor.h>

#include <string>
#include <unordered_map>

namespace rime {
class Config;
class Context;
class KeyEvent;
}  // namespace rime

namespace rime::aipara {

class SmartCursorProcessor : public Processor {
 public:
  explicit SmartCursorProcessor(const Ticket& ticket);
  ~SmartCursorProcessor() override;

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

  void UpdateCurrentConfig(Config* config);

 private:
  void OnUpdate(Context* ctx);

  std::string move_next_punct_;
  std::string move_prev_punct_;
  std::string paste_to_input_;
  std::string search_move_cursor_;
  std::string shuru_schema_;
  bool keep_input_uncommit_ = false;

  std::unordered_map<std::string, std::string> chat_triggers_;
  std::string previous_client_app_;
  std::unordered_map<std::string, std::string> app_vim_mode_state_;

  connection update_connection_;
  bool config_initialized_ = false;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_SMART_CURSOR_PROCESSOR_H_
