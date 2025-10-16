#ifndef PLUGINS_AIPARA_SRC_TCP_SOCKET_SYNC_H_
#define PLUGINS_AIPARA_SRC_TCP_SOCKET_SYNC_H_

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace rime {
class Config;
class Context;
}  // namespace rime

namespace rime::aipara {

struct TcpConnectionInfo {
  std::string host;
  int rime_port = 0;
  int ai_port = 0;
  bool rime_connected = false;
  bool ai_connected = false;
};

class TcpSocketSync {
 public:
  TcpSocketSync();

  bool Init();
  void Fini();

  void SetConfigUpdateHandler(
      std::function<void(const Config&)> config_update_function,
      std::function<void(const std::string&, const std::string&)> property_update_function);

  void UpdateConfigs(const Config& config);
  void UpdateProperty(const std::string& property_name,
                      const std::string& property_value);

  void SetGlobalOption(const std::string& name, bool value);
  int ApplyGlobalOptionsToContext(Context* context);

  void SetConnectionParams(std::string host, int rime_port, int ai_port);
  TcpConnectionInfo GetConnectionInfo() const;

  bool IsSystemReady() const;
  bool IsRimeSocketReady() const;
  bool IsAiSocketReady() const;
  void ForceReconnect();

  bool SendConvertRequest(const std::string& schema_name,
                          const std::string& shuru_schema,
                          const std::string& confirmed_pos_input,
                          const std::string& long_candidates_table,
                          const std::string& extra_payload);
  std::optional<std::string> ReadConvertResult(double timeout_seconds);

  bool SendChatMessage(const std::string& commit_text,
                       const std::string& assistant_id,
                       const std::string& response_key);

  void SyncWithServer();

 private:
  std::unordered_map<std::string, bool> global_option_state_;
  bool update_global_option_state_ = false;

  std::function<void(const Config&)> update_all_modules_config_;
  std::function<void(const std::string&, const std::string&)> property_update_function_;

  TcpConnectionInfo connection_info_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_TCP_SOCKET_SYNC_H_
