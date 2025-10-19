#ifndef PLUGINS_AIPARA_SRC_SMART_CURSOR_PROCESSOR_H_
#define PLUGINS_AIPARA_SRC_SMART_CURSOR_PROCESSOR_H_

#include <rime/common.h>
#include <rime/processor.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/logger.h"

namespace rime {
class Config;
class Context;
class KeyEvent;
class Composition;
class Segment;
}  // namespace rime

namespace rime::aipara {

class TcpSocketSync;

class SmartCursorProcessor : public Processor {
 public:
  explicit SmartCursorProcessor(const Ticket& ticket);
  ~SmartCursorProcessor() override;

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

  void UpdateCurrentConfig(Config* config);
  void AttachTcpSocketSync(TcpSocketSync* sync);

 private:
  void InitializeContextHooks(Context* context);
  void DisconnectAll();

  void OnSelect(Context* context);
  void OnCommit(Context* context);
  void OnUpdate(Context* context);
  void OnExtendedUpdate(Context* context);
  void OnPropertyUpdate(Context* context, const std::string& property);
  void OnUnhandledKey(Context* context, const KeyEvent& key_event);

  bool HandleSearchMode(const std::string& key_repr,
                        Context* context,
                        Config* config,
                        Composition* composition);
  void ExitSearchMode(Context* context, Segment* segment);

  bool MoveToNextPunctuation(Context* context);
  bool MoveToPrevPunctuation(Context* context);
  bool MoveBySpans(Context* context, bool move_next);

  void ApplyGlobalOptions(Context* context);
  void ApplyAppOptions(const std::string& current_app,
                       Context* context,
                       Config* config);
  void UpdateAsciiModeFromVimState(const std::string& app_key,
                                   Context* context,
                                   Config* config);

  std::string SanitizeAppKey(const std::string& app_name) const;
  Config* CurrentConfig() const;
  std::string GetConfigString(const std::string& path,
                              const std::string& fallback = std::string())
      const;
  bool GetConfigBool(const std::string& path, bool fallback) const;
  std::unordered_map<std::string, std::string> LoadChatTriggers(
      Config* config) const;

  void SyncWithServer(Context* context, bool include_config = false) const;

  Logger logger_;
  TcpSocketSync* tcp_socket_sync_ = nullptr;

  std::unordered_set<char> punctuation_chars_;
  std::unordered_map<std::string, std::string> app_vim_mode_state_;
  std::optional<bool> previous_is_composing_;
  std::string previous_client_app_;

  connection select_connection_;
  connection commit_connection_;
  connection update_connection_;
  connection extended_update_connection_;
  connection property_update_connection_;
  connection unhandled_key_connection_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_SMART_CURSOR_PROCESSOR_H_
