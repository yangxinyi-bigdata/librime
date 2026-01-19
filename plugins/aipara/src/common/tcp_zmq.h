#ifndef PLUGINS_AIPARA_SRC_COMMON_TCP_ZMQ_H_
#define PLUGINS_AIPARA_SRC_COMMON_TCP_ZMQ_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef Bool
#pragma push_macro("Bool")
#undef Bool
#define RIME_BOOL_MACRO_RESTORE
#endif
#ifdef True
#pragma push_macro("True")
#undef True
#define RIME_TRUE_MACRO_RESTORE
#endif
#ifdef False
#pragma push_macro("False")
#undef False
#define RIME_FALSE_MACRO_RESTORE
#endif
#include <rapidjson/document.h>
#ifdef RIME_FALSE_MACRO_RESTORE
#pragma pop_macro("False")
#undef RIME_FALSE_MACRO_RESTORE
#endif
#ifdef RIME_TRUE_MACRO_RESTORE
#pragma pop_macro("True")
#undef RIME_TRUE_MACRO_RESTORE
#endif
#ifdef RIME_BOOL_MACRO_RESTORE
#pragma pop_macro("Bool")
#undef RIME_BOOL_MACRO_RESTORE
#endif

#include "logger.h"

namespace rime {
class Config;
class Context;
class Engine;
}  // namespace rime

namespace rime::aipara {

class TcpZmq {
 public:
  struct SocketStats {
    int port = 0;
    bool is_connected = false;
    int connection_failures = 0;
    int write_failure_count = 0;
    int timeout_seconds = 0;
  };

  struct Stats {
    bool is_initialized = false;
    std::string host;
    SocketStats rime_state;
    SocketStats ai_convert;
  };

  struct ConnectionInfo {
    std::string host;
    SocketStats rime_state;
    SocketStats ai_convert;
  };

  enum class LatestStatus {
    kSuccess,
    kTimeout,
    kNoData,
    kError,
  };

  struct LatestAiMessage {
    LatestStatus status = LatestStatus::kNoData;
    std::optional<rapidjson::Document> data;
    std::string raw_message;
    std::optional<std::string> error_msg;
  };

  struct ConvertReadResult {
    LatestStatus status = LatestStatus::kNoData;
    std::optional<rapidjson::Document> data;
    bool is_final = false;
    bool is_partial = false;
    bool is_timeout = false;
    bool is_error = false;
    bool network_unavailable = false;
    bool cloud_response_invalid = false;
    std::optional<std::string> error_msg;
  };

  using ConfigUpdateCallback = std::function<void(rime::Config*)>;
  using PropertyUpdateCallback =
      std::function<void(const std::string&, const std::string&)>;

  TcpZmq();
  ~TcpZmq();

  TcpZmq(const TcpZmq&) = delete;
  TcpZmq& operator=(const TcpZmq&) = delete;

  static TcpZmq& Instance();

  void SetGlobalOption(const std::string& name, bool value);
  void SetGlobalProperty(const std::string& name,
                         const std::string& value);
  std::optional<std::string> GetGlobalProperty(
      const std::string& name) const;
  int ApplyGlobalOptionsToContext(rime::Context* context);

  bool should_apply_global_options() const {
    return update_global_option_state_;
  }
  void clear_global_option_update_flag() { update_global_option_state_ = false; }

  void SetConfigUpdateHandler(ConfigUpdateCallback config_cb,
                              PropertyUpdateCallback property_cb);
  void UpdateConfigs(rime::Config* config);
  void UpdateProperty(const std::string& property_name,
                      const std::string& property_value);

  bool Init();
  void Fini();

  void RefreshCurveConfig(rime::Config* config);

  bool ConnectToRimeServer();
  bool ConnectToAiServer();
  void DisconnectFromRimeServer(int retry_delay_ms = -1);
  void DisconnectFromAiServer();
  void DisconnectFromServer();
  bool CheckAiConnection() const;
  bool CheckRimeConnection() const;

  bool WriteToRimeSocket(const std::string& data);
  bool WriteToAiSocket(const std::string& data);

  std::optional<std::string> ReadFromRimeSocket(
      std::optional<double> timeout_seconds = std::nullopt);
  std::optional<std::string> ReadFromAiSocket(
      std::optional<double> timeout_seconds = std::nullopt);
  std::optional<std::string> ReadAllFromAiSocket(
      std::optional<double> timeout_seconds = std::nullopt);

  LatestAiMessage ReadLatestFromAiSocket(
      std::optional<double> timeout_seconds = std::nullopt);

  std::optional<rapidjson::Document> ParseSocketData(
      const std::string& data);

  bool HandleSocketCommand(const rapidjson::Value& command_message,
                           rime::Engine* engine);
  bool ProcessRimeSocketData(rime::Engine* engine,
                             std::optional<double> timeout_seconds =
                                 std::nullopt);

  bool SyncWithServer(
      rime::Engine* engine,
      bool include_option_info = false,
      bool send_commit_text = false,
      const std::optional<std::string>& command_key = std::nullopt,
      const std::optional<std::string>& command_value = std::nullopt,
      std::optional<double> timeout_seconds = std::nullopt,
      const std::optional<std::string>& position = std::nullopt,
      const std::optional<std::string>& character = std::nullopt);

  bool SendConvertRequest(
      const std::string& schema_name,
      const std::string& shuru_schema,
      const std::string& confirmed_pos_input,
      const std::vector<std::string>& long_candidates_text = {},
      std::optional<double> timeout_seconds = std::nullopt);
  ConvertReadResult ReadConvertResult(
      std::optional<double> timeout_seconds = std::nullopt);

  bool SendPasteCommand(rime::Engine* engine);
  bool SendChatMessage(
      const std::string& commit_text,
      const std::string& assistant_id,
      const std::optional<std::string>& response_key = std::nullopt);
  bool SendAiCommand(const std::string& message_type);

  bool IsSystemReady() const;
  bool IsRimeSocketReady() const;
  bool IsAiSocketReady() const;
  bool ForceReconnect();

  void SetConnectionParams(const std::string& host,
                           std::optional<int> rime_port = std::nullopt,
                           std::optional<int> ai_port = std::nullopt);

  Stats GetStats() const;
  ConnectionInfo GetConnectionInfo() const;

 private:
  struct SocketState {
    int port = 0;
    void* socket = nullptr;
    std::string identity;
    bool is_connected = false;
    std::int64_t last_connect_attempt = 0;
    int connect_retry_interval_ms = 5000;
    int connection_failures = 0;
    int max_connection_failures = 3;
    int write_failure_count = 0;
    int max_failure_count = 3;
    int timeout_seconds = 0;
    std::deque<std::string> recv_queue;
    std::string last_error;
    int default_rcv_timeout_ms = 0;
    int default_snd_timeout_ms = 0;
    std::int64_t last_send_at = 0;
    std::int64_t last_recv_at = 0;
    std::int64_t suspended_until = 0;
    int health_check_interval_ms = 5000;
    std::int64_t last_health_check = 0;
    std::uint64_t curve_version_applied = 0;
    bool connect_pending = false;
    bool handshake_logged = false;
    int handshake_timeout_ms = 5000;
    std::string last_endpoint;
  };

  struct ReceiveResult {
    bool ok = false;
    std::vector<std::string> messages;
    int error_code = 0;
    std::string error_message;
  };

  static std::int64_t NowMs();
  bool EnsureContext();
  void CloseSocket(void*& socket);
  void ResetSocketState(SocketState& state, bool reset_queue = true);
  void ConfigureSocketDefaults(SocketState& state);
  ReceiveResult ReceiveSocketPayloads(void* socket, int flags);
  int DrainSocketImmediate(SocketState& state,
                           const char* channel_name,
                           std::string* fatal_error);
  static std::vector<std::string> SplitPayload(const std::string& payload);
  static bool IsTemporaryError(int error_code);
  bool ConfigureCurveForSocket(SocketState& state);
  bool EnsureCurveKeysLoaded();
  bool LoadCurveKeys();
  void MarkSocketHandshakeSuccess(SocketState& state,
                                  const char* channel_name);
  static int ToMilliseconds(std::optional<double> timeout_seconds,
                            int fallback_ms);
  std::string EnsureAiIdentity();
  void SetSocketTimeout(void* socket, int option_name, int timeout_ms);
  void RestoreDefaultTimeout(SocketState& state, int option_name);

  bool UpdateConfigTable(rime::Config* config,
                         const std::string& base_path,
                         const rapidjson::Value& value);
  bool UpdateConfigField(rime::Config* config,
                         const std::string& field_path,
                         const rapidjson::Value& field_value);

  const std::string& EnglishModeSymbol(rime::Context* context,
                                       rime::Config* config,
                                       std::string* buffer);

  Logger logger_;
  void* context_ = nullptr;
  std::string host_ = "127.0.0.1";
  std::string client_id_;
  bool is_initialized_ = false;

  struct CurveSettings {
    bool configured = false;
    bool enabled = false;
    std::string cert_dir;
    std::string server_public_key;
    std::string client_public_key;
    std::string client_secret_key;
    bool keys_loaded = false;
    std::string last_error;
    std::uint64_t version = 0;
  };

  CurveSettings curve_settings_;

  SocketState rime_state_;
  SocketState ai_convert_;

  ConfigUpdateCallback config_callback_;
  PropertyUpdateCallback property_callback_;

  std::unordered_map<std::string, bool> global_option_state_;
  std::unordered_map<std::string, std::string> global_property_state_;
  bool update_global_option_state_ = false;
};

TcpZmq* AcquireGlobalTcpZmq();

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_COMMON_TCP_ZMQ_H_
