#include "tcp_socket_sync.h"

#include <rime/config.h>
#include <rime/context.h>

namespace rime::aipara {

TcpSocketSync::TcpSocketSync() = default;

bool TcpSocketSync::Init() {
  // TODO: establish socket connections.
  return true;
}

void TcpSocketSync::Fini() {
  // TODO: tear down socket connections.
}

void TcpSocketSync::SetConfigUpdateHandler(
    std::function<void(const Config&)> config_update_function,
    std::function<void(const std::string&, const std::string&)>
        property_update_function) {
  update_all_modules_config_ = std::move(config_update_function);
  property_update_function_ = std::move(property_update_function);
}

void TcpSocketSync::UpdateConfigs(const Config& config) {
  if (update_all_modules_config_) {
    update_all_modules_config_(config);
  }
}

void TcpSocketSync::UpdateProperty(const std::string& property_name,
                                   const std::string& property_value) {
  if (property_update_function_) {
    property_update_function_(property_name, property_value);
  }
}

void TcpSocketSync::SetGlobalOption(const std::string& name, bool value) {
  global_option_state_[name] = value;
  update_global_option_state_ = true;
}

int TcpSocketSync::ApplyGlobalOptionsToContext(Context* context) {
  if (!context) {
    return 0;
  }
  int applied = 0;
  for (const auto& [name, value] : global_option_state_) {
    if (context->get_option(name) != value) {
      context->set_option(name, value);
      ++applied;
    }
  }
  update_global_option_state_ = false;
  return applied;
}

void TcpSocketSync::SetConnectionParams(std::string host,
                                        int rime_port,
                                        int ai_port) {
  connection_info_.host = std::move(host);
  connection_info_.rime_port = rime_port;
  connection_info_.ai_port = ai_port;
}

TcpConnectionInfo TcpSocketSync::GetConnectionInfo() const {
  return connection_info_;
}

bool TcpSocketSync::IsSystemReady() const {
  return connection_info_.rime_connected && connection_info_.ai_connected;
}

bool TcpSocketSync::IsRimeSocketReady() const {
  return connection_info_.rime_connected;
}

bool TcpSocketSync::IsAiSocketReady() const {
  return connection_info_.ai_connected;
}

void TcpSocketSync::ForceReconnect() {
  connection_info_.rime_connected = false;
  connection_info_.ai_connected = false;
}

bool TcpSocketSync::SendConvertRequest(const std::string&,
                                       const std::string&,
                                       const std::string&,
                                       const std::string&,
                                       const std::string&) {
  // TODO: implement request dispatching.
  return false;
}

std::optional<std::string> TcpSocketSync::ReadConvertResult(
    double /*timeout_seconds*/) {
  // TODO: read responses from AI socket.
  return std::nullopt;
}

bool TcpSocketSync::SendChatMessage(const std::string&,
                                    const std::string&,
                                    const std::string&) {
  // TODO: send chat message through AI socket.
  return false;
}

void TcpSocketSync::SyncWithServer() {
  // TODO: integrate with socket event loop.
}

}  // namespace rime::aipara
