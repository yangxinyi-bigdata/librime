#include "common/tcp_zmq.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <zmq.h>

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/composition.h>

namespace rime::aipara {
namespace {

constexpr int kDefaultRimePort = 10089;
constexpr int kDefaultAiPort = 10090;
constexpr int kMaxProcessMessages = 5;

Logger MakeTcpZmqLogger() {
  Logger::Options options;
  options.enabled = true;
  options.unique_file_log = false;
  options.log_level = std::string("DEBUG");
  return MakeLogger("tcp_zmq", options);
}

std::string JsonStringify(const rapidjson::Value& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
}

std::string DotPathToRimePath(const std::string& dot_path) {
  std::string result = dot_path;
  std::replace(result.begin(), result.end(), '.', '/');
  return result;
}

void ReplaceAll(std::string* text,
                const std::string& from,
                const std::string& to) {
  if (!text || from.empty()) {
    return;
  }
  size_t pos = 0;
  while ((pos = text->find(from, pos)) != std::string::npos) {
    text->replace(pos, from.length(), to);
    pos += to.length();
  }
}

std::optional<std::string> GetOptionalString(const rapidjson::Value& value,
                                             const char* key) {
  if (!value.IsObject() || !value.HasMember(key)) {
    return std::nullopt;
  }
  const rapidjson::Value& member = value[key];
  if (member.IsString()) {
    return std::string(member.GetString(), member.GetStringLength());
  }
  return std::nullopt;
}

bool GetOptionalBool(const rapidjson::Value& value,
                     const char* key,
                     bool* result) {
  if (!value.IsObject() || !value.HasMember(key) || !result) {
    return false;
  }
  const rapidjson::Value& member = value[key];
  if (member.IsBool()) {
    *result = member.GetBool();
    return true;
  }
  if (member.IsInt()) {
    *result = member.GetInt() != 0;
    return true;
  }
  if (member.IsString()) {
    const std::string text(member.GetString(), member.GetStringLength());
    if (text == "1" || text == "true" || text == "True" ||
        text == "TRUE") {
      *result = true;
      return true;
    }
    if (text == "0" || text == "false" || text == "False" ||
        text == "FALSE") {
      *result = false;
      return true;
    }
  }
  return false;
}

}  // namespace

TcpZmq::TcpZmq()
    : logger_(MakeTcpZmqLogger()) {
  client_id_ = "rime-cpp-" + std::to_string(NowMs());

  rime_state_.port = kDefaultRimePort;
  rime_state_.connect_retry_interval_ms = 5000;
  rime_state_.default_rcv_timeout_ms = 0;
  rime_state_.default_snd_timeout_ms = 0;
  rime_state_.timeout_seconds = 0;
  rime_state_.health_check_interval_ms = 5000;

  ai_convert_.port = kDefaultAiPort;
  ai_convert_.connect_retry_interval_ms = 5000;
  ai_convert_.default_rcv_timeout_ms = 100;
  ai_convert_.default_snd_timeout_ms = 100;
  ai_convert_.timeout_seconds = 0;
}

TcpZmq::~TcpZmq() {
  Fini();
}

void TcpZmq::SetGlobalOption(const std::string& name, bool value) {
  const auto it = global_option_state_.find(name);
  if (it != global_option_state_.end() && it->second == value) {
    return;
  }
  global_option_state_[name] = value;
  AIPARA_LOG_DEBUG(
      logger_, "记录全局开关: " + name + " = " + (value ? "true" : "false"));
}

int TcpZmq::ApplyGlobalOptionsToContext(rime::Context* context) {
  if (!context) {
    return 0;
  }
  int applied = 0;
  for (const auto& [name, value] : global_option_state_) {
    if (context->get_option(name) != value) {
      context->set_option(name, value);
      ++applied;
      AIPARA_LOG_DEBUG(
          logger_, "应用全局开关到context: " + name + " = " +
                       (value ? "true" : "false"));
    }
  }
  return applied;
}

void TcpZmq::SetConfigUpdateHandler(ConfigUpdateCallback config_cb,
                                    PropertyUpdateCallback property_cb) {
  config_callback_ = std::move(config_cb);
  property_callback_ = std::move(property_cb);
}

void TcpZmq::UpdateConfigs(rime::Config* config) {
  if (config_callback_) {
    config_callback_(config);
  }
}

void TcpZmq::UpdateProperty(const std::string& property_name,
                            const std::string& property_value) {
  if (property_callback_) {
    property_callback_(property_name, property_value);
  }
}

bool TcpZmq::Init() {
  AIPARA_LOG_INFO(logger_, "双端口TCP套接字状态同步系统初始化");

  if (is_initialized_) {
    return true;
  }

  logger_.Clear();

  const bool rime_connected = ConnectToRimeServer();
  const bool ai_connected = ConnectToAiServer();

  if (rime_connected || ai_connected) {
    is_initialized_ = true;
    AIPARA_LOG_INFO(logger_, "双端口TCP套接字系统初始化成功");
    if (rime_connected) {
      AIPARA_LOG_INFO(logger_, "Rime状态服务连接成功");
    }
    if (ai_connected) {
      AIPARA_LOG_INFO(logger_, "AI转换服务连接成功");
    }
    AIPARA_LOG_INFO(logger_, "双端口TCP套接字系统初始化完成");
    return true;
  }

  AIPARA_LOG_INFO(
      logger_,
      "双端口TCP套接字系统初始化失败，但系统仍可工作（离线模式）");
  is_initialized_ = true;
  AIPARA_LOG_INFO(logger_, "双端口TCP套接字系统初始化完成");
  return true;
}

void TcpZmq::Fini() {
  AIPARA_LOG_INFO(logger_, "双端口ZeroMQ套接字系统清理");

  DisconnectFromServer();

  if (context_) {
    zmq_ctx_term(context_);
    context_ = nullptr;
  }

  is_initialized_ = false;
  AIPARA_LOG_INFO(logger_, "双端口ZeroMQ套接字系统清理完成");
}

bool TcpZmq::EnsureContext() {
  if (context_) {
    return true;
  }
  context_ = zmq_ctx_new();
  if (!context_) {
    AIPARA_LOG_ERROR(
        logger_, "ZeroMQ 上下文创建失败: " + std::string(zmq_strerror(errno)));
    return false;
  }
  return true;
}

void TcpZmq::CloseSocket(void*& socket) {
  if (!socket) {
    return;
  }
  zmq_close(socket);
  socket = nullptr;
}

void TcpZmq::ResetSocketState(SocketState& state, bool reset_queue) {
  CloseSocket(state.socket);
  state.is_connected = false;
  state.last_error.clear();
  state.last_send_at = 0;
  state.last_recv_at = 0;
  state.suspended_until = 0;
  state.write_failure_count = 0;
  if (reset_queue) {
    state.recv_queue.clear();
  }
}

void TcpZmq::ConfigureSocketDefaults(SocketState& state) {
  if (!state.socket) {
    return;
  }
  const int linger = 0;
  zmq_setsockopt(state.socket, ZMQ_LINGER, &linger, sizeof(linger));
  if (state.default_snd_timeout_ms >= 0) {
    SetSocketTimeout(state.socket, ZMQ_SNDTIMEO,
                     state.default_snd_timeout_ms);
  }
  if (state.default_rcv_timeout_ms >= 0) {
    SetSocketTimeout(state.socket, ZMQ_RCVTIMEO,
                     state.default_rcv_timeout_ms);
  }
}

TcpZmq::ReceiveResult TcpZmq::ReceiveSocketPayloads(void* socket, int flags) {
  ReceiveResult result;
  if (!socket) {
    result.error_code = EINVAL;
    result.error_message = "no_socket";
    return result;
  }

  std::string payload;
  while (true) {
    zmq_msg_t msg;
    zmq_msg_init(&msg);
    const int rc = zmq_msg_recv(&msg, socket, flags);
    if (rc == -1) {
      const int err = zmq_errno();
      result.error_code = err;
      result.error_message = zmq_strerror(err);
      zmq_msg_close(&msg);
      return result;
    }
    const char* data_ptr =
        static_cast<const char*>(zmq_msg_data(&msg));
    payload.append(data_ptr, rc);
    const int more = zmq_msg_more(&msg);
    zmq_msg_close(&msg);
    if (!more) {
      break;
    }
  }

  if (payload.empty()) {
    result.error_code = 0;
    result.error_message = "empty_payload";
    return result;
  }

  result.messages = SplitPayload(payload);
  if (result.messages.empty()) {
    result.error_code = 0;
    result.error_message = "empty_payload";
    return result;
  }

  result.ok = true;
  return result;
}

int TcpZmq::DrainSocketImmediate(SocketState& state,
                                 std::string* fatal_error) {
  if (!state.socket) {
    return 0;
  }
  int drained = 0;
  while (true) {
    ReceiveResult recv = ReceiveSocketPayloads(state.socket, ZMQ_DONTWAIT);
    if (recv.ok) {
      for (const auto& message : recv.messages) {
        state.recv_queue.emplace_back(message);
        drained += static_cast<int>(message.size());
      }
      continue;
    }
    if (IsTemporaryError(recv.error_code)) {
      break;
    }
    if (recv.error_code != 0 && fatal_error) {
      *fatal_error = recv.error_message;
    }
    break;
  }
  return drained;
}

std::vector<std::string> TcpZmq::SplitPayload(const std::string& payload) {
  if (payload.empty()) {
    return {};
  }
  if (payload.find('\n') == std::string::npos &&
      payload.find('\r') == std::string::npos) {
    return {payload};
  }
  std::vector<std::string> result;
  std::string current;
  for (char ch : payload) {
    if (ch == '\n' || ch == '\r') {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    result.push_back(current);
  }
  if (result.empty()) {
    result.push_back(payload);
  }
  return result;
}

bool TcpZmq::IsTemporaryError(int error_code) {
  return error_code == EAGAIN || error_code == EINTR ||
         error_code == ETIMEDOUT;
}

int TcpZmq::ToMilliseconds(std::optional<double> timeout_seconds,
                           int fallback_ms) {
  if (!timeout_seconds.has_value()) {
    return fallback_ms;
  }
  double seconds = *timeout_seconds;
  if (seconds < 0) {
    seconds = 0.0;
  }
  return static_cast<int>(std::round(seconds * 1000.0));
}

std::string TcpZmq::EnsureAiIdentity() {
  if (!ai_convert_.identity.empty()) {
    return ai_convert_.identity;
  }
  std::mt19937 rng(static_cast<std::mt19937::result_type>(NowMs()));
  std::uniform_int_distribution<int> dist(0, 999999);
  std::ostringstream oss;
  oss << (client_id_.empty() ? "rime-cpp" : client_id_) << "-"
      << std::setw(6) << std::setfill('0') << dist(rng);
  ai_convert_.identity = oss.str();
  return ai_convert_.identity;
}

void TcpZmq::SetSocketTimeout(void* socket,
                              int option_name,
                              int timeout_ms) {
  if (!socket) {
    return;
  }
  zmq_setsockopt(socket, option_name, &timeout_ms, sizeof(timeout_ms));
}

void TcpZmq::RestoreDefaultTimeout(SocketState& state, int option_name) {
  if (!state.socket) {
    return;
  }
  int timeout_ms = (option_name == ZMQ_RCVTIMEO)
                       ? state.default_rcv_timeout_ms
                       : state.default_snd_timeout_ms;
  if (timeout_ms >= 0) {
    SetSocketTimeout(state.socket, option_name, timeout_ms);
  }
}

std::int64_t TcpZmq::NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

bool TcpZmq::ConnectToRimeServer() {
  SocketState& state = rime_state_;
  if (state.socket && state.is_connected) {
    return true;
  }

  if (!EnsureContext()) {
    state.connection_failures++;
    state.last_error = "context_creation_failed";
    return false;
  }

  const std::int64_t now = NowMs();
  if (state.suspended_until > 0 && now < state.suspended_until) {
    return false;
  }
  if (now - state.last_connect_attempt < state.connect_retry_interval_ms) {
    return state.socket != nullptr && state.is_connected;
  }
  state.last_connect_attempt = now;

  ResetSocketState(state);

  state.socket = zmq_socket(context_, ZMQ_DEALER);
  if (!state.socket) {
    state.connection_failures++;
    state.last_error = zmq_strerror(errno);
    AIPARA_LOG_ERROR(logger_, "创建 Rime DEALER 套接字失败: " +
                                  state.last_error);
    return false;
  }

  const std::string identity = client_id_ + "-rime";
  zmq_setsockopt(state.socket, ZMQ_IDENTITY, identity.data(),
                 static_cast<int>(identity.size()));

  ConfigureSocketDefaults(state);

  const std::string endpoint =
      "tcp://" + host_ + ":" + std::to_string(state.port);
  if (zmq_connect(state.socket, endpoint.c_str()) != 0) {
    state.connection_failures++;
    state.last_error = zmq_strerror(errno);
    AIPARA_LOG_WARN(
        logger_, "连接 Rime ZeroMQ 服务失败: " + state.last_error);
    ResetSocketState(state, true);
    return false;
  }

  state.is_connected = true;
  state.connection_failures = 0;
  state.write_failure_count = 0;
  state.recv_queue.clear();
  state.last_error.clear();

  AIPARA_LOG_DEBUG(logger_,
                   "Rime状态ZeroMQ连接建立成功: " + endpoint +
                       " identity=" + identity);
  return true;
}

bool TcpZmq::ConnectToAiServer() {
  SocketState& state = ai_convert_;
  if (state.socket && state.is_connected) {
    return true;
  }

  const std::int64_t now = NowMs();
  if (now - state.last_connect_attempt < state.connect_retry_interval_ms) {
    return state.socket != nullptr && state.is_connected;
  }
  state.last_connect_attempt = now;

  if (!EnsureContext()) {
    state.connection_failures++;
    state.last_error = "context_creation_failed";
    return false;
  }

  ResetSocketState(state);

  state.socket = zmq_socket(context_, ZMQ_DEALER);
  if (!state.socket) {
    state.connection_failures++;
    state.last_error = zmq_strerror(errno);
    AIPARA_LOG_ERROR(logger_, "创建 AI DEALER 套接字失败: " +
                                  state.last_error);
    return false;
  }

  const std::string identity = EnsureAiIdentity();
  zmq_setsockopt(state.socket, ZMQ_IDENTITY, identity.data(),
                 static_cast<int>(identity.size()));
  ConfigureSocketDefaults(state);

  const std::string endpoint =
      "tcp://" + host_ + ":" + std::to_string(state.port);
  if (zmq_connect(state.socket, endpoint.c_str()) != 0) {
    state.connection_failures++;
    state.last_error = zmq_strerror(errno);
    AIPARA_LOG_WARN(
        logger_, "连接 AI ZeroMQ 服务失败: " + state.last_error);
    ResetSocketState(state, true);
    return false;
  }

  state.is_connected = true;
  state.connection_failures = 0;
  state.write_failure_count = 0;
  state.recv_queue.clear();
  state.last_error.clear();

  AIPARA_LOG_DEBUG(logger_,
                   "AI转换ZeroMQ连接建立成功: " + endpoint +
                       " identity=" + identity);
  return true;
}

void TcpZmq::DisconnectFromRimeServer(int retry_delay_ms) {
  ResetSocketState(rime_state_);
  if (retry_delay_ms < 0) {
    retry_delay_ms = rime_state_.connect_retry_interval_ms;
  }
  rime_state_.suspended_until = NowMs() + retry_delay_ms;
  AIPARA_LOG_DEBUG(logger_, "Rime状态服务连接已断开");
}

void TcpZmq::DisconnectFromAiServer() {
  ResetSocketState(ai_convert_);
  AIPARA_LOG_DEBUG(logger_, "AI转换服务连接已断开");
}

void TcpZmq::DisconnectFromServer() {
  DisconnectFromRimeServer();
  DisconnectFromAiServer();
  AIPARA_LOG_DEBUG(logger_, "所有ZeroMQ连接已断开");
}

bool TcpZmq::CheckAiConnection() const {
  return ai_convert_.socket != nullptr && ai_convert_.is_connected;
}

bool TcpZmq::CheckRimeConnection() const {
  return rime_state_.socket != nullptr && rime_state_.is_connected;
}

bool TcpZmq::WriteToRimeSocket(const std::string& data) {
  if (!is_initialized_) {
    return false;
  }
  if (!ConnectToRimeServer()) {
    AIPARA_LOG_WARN(logger_, "Rime状态服务连接不可用");
    return false;
  }

  SocketState& state = rime_state_;
  std::string fatal_error;
  const int drained = DrainSocketImmediate(state, &fatal_error);
  if (!fatal_error.empty()) {
    state.last_error = fatal_error;
    AIPARA_LOG_WARN(
        logger_,
        "Rime状态通道在发送前检测到读取错误，准备重连: " + fatal_error);
    DisconnectFromRimeServer();
    return false;
  }
  if (drained > 0) {
    state.last_recv_at = NowMs();
    AIPARA_LOG_DEBUG(logger_, "Rime状态通道发送前收到了 " +
                                  std::to_string(drained) + " 字节积压数据");
  }

  const int rc =
      zmq_send(state.socket, data.data(), data.size(), ZMQ_DONTWAIT);
  if (rc >= 0) {
    state.write_failure_count = 0;
    state.last_error.clear();
    state.last_send_at = NowMs();

    std::string fatal_after;
    const int drained_after = DrainSocketImmediate(state, &fatal_after);
    if (!fatal_after.empty()) {
      state.last_error = fatal_after;
      AIPARA_LOG_WARN(
          logger_,
          "Rime状态通道发送后检测到读取错误: " + fatal_after);
      DisconnectFromRimeServer();
    } else if (drained_after > 0) {
      state.last_recv_at = NowMs();
      AIPARA_LOG_DEBUG(
          logger_, "Rime状态通道发送后立即收到了 " +
                       std::to_string(drained_after) + " 字节数据");
    }
    return true;
  }

  const int err = zmq_errno();
  const std::string err_str = zmq_strerror(err);
  state.write_failure_count++;
  state.last_error = err_str;

  if (IsTemporaryError(err)) {
    if (state.write_failure_count == 1 ||
        state.write_failure_count % state.max_failure_count == 0) {
      AIPARA_LOG_WARN(
          logger_,
          "Rime状态ZeroMQ发送被丢弃（连接忙碌），累计丢弃次数: " +
              std::to_string(state.write_failure_count));
    }
    if (state.write_failure_count >= state.max_failure_count) {
      AIPARA_LOG_WARN(logger_,
                      "Rime状态通道连续发送失败，暂停发送并等待重连");
      DisconnectFromRimeServer(state.connect_retry_interval_ms * 2);
      state.write_failure_count = 0;
    }
    return false;
  }

  AIPARA_LOG_ERROR(
      logger_, "Rime状态ZeroMQ写入失败: " + err_str + " (失败次数: " +
                   std::to_string(state.write_failure_count) + ")");
  DisconnectFromRimeServer(state.connect_retry_interval_ms * 2);
  return false;
}

bool TcpZmq::WriteToAiSocket(const std::string& data) {
  if (!is_initialized_) {
    return false;
  }
  if (!ConnectToAiServer()) {
    AIPARA_LOG_WARN(logger_, "AI转换服务连接不可用");
    return false;
  }

  SocketState& state = ai_convert_;
  const int rc = zmq_send(state.socket, data.data(), data.size(), 0);
  if (rc >= 0) {
    state.write_failure_count = 0;
    state.last_error.clear();
    AIPARA_LOG_DEBUG(logger_, "AI接口数据发送成功");
    return true;
  }

  const int err = zmq_errno();
  state.write_failure_count++;
  state.last_error = zmq_strerror(err);
  AIPARA_LOG_ERROR(
      logger_, "AI转换服务ZeroMQ写入失败: " + state.last_error +
                   " (失败次数: " +
                   std::to_string(state.write_failure_count) + ")");
  DisconnectFromAiServer();
  return false;
}

std::optional<std::string> TcpZmq::ReadFromRimeSocket(
    std::optional<double> timeout_seconds) {
  SocketState& state = rime_state_;
  state.last_error.clear();

  if (!ConnectToRimeServer()) {
    state.last_error = "connection_failed";
    return std::nullopt;
  }

  std::string fatal_before;
  const int drained = DrainSocketImmediate(state, &fatal_before);
  if (!fatal_before.empty()) {
    state.last_error = fatal_before;
    AIPARA_LOG_WARN(logger_,
                    "Rime状态通道读取失败，准备重连: " + fatal_before);
    DisconnectFromRimeServer();
    return std::nullopt;
  }
  if (!state.recv_queue.empty()) {
    std::string message = std::move(state.recv_queue.front());
    state.recv_queue.pop_front();
    state.last_error.clear();
    return message;
  }

  const bool custom_timeout = timeout_seconds.has_value();
  const int custom_ms = ToMilliseconds(timeout_seconds,
                                       state.default_rcv_timeout_ms);
  if (custom_timeout) {
    SetSocketTimeout(state.socket, ZMQ_RCVTIMEO, custom_ms);
  }

  ReceiveResult result = ReceiveSocketPayloads(state.socket, 0);

  if (custom_timeout &&
      custom_ms != state.default_rcv_timeout_ms) {
    RestoreDefaultTimeout(state, ZMQ_RCVTIMEO);
  }

  if (result.ok && !result.messages.empty()) {
    if (result.messages.size() > 1) {
      for (size_t i = 1; i < result.messages.size(); ++i) {
        state.recv_queue.emplace_back(result.messages[i]);
      }
    }
    state.last_error.clear();
    return result.messages[0];
  }

  if (IsTemporaryError(result.error_code)) {
    state.last_error = "timeout";
    return std::nullopt;
  }

  if (!result.error_message.empty()) {
    state.last_error = result.error_message;
  } else {
    state.last_error = "unknown_error";
  }
  AIPARA_LOG_WARN(
      logger_, "Rime状态ZeroMQ读取失败: " + state.last_error);
  DisconnectFromRimeServer();
  return std::nullopt;
}

std::optional<std::string> TcpZmq::ReadFromAiSocket(
    std::optional<double> timeout_seconds) {
  SocketState& state = ai_convert_;
  state.last_error.clear();

  if (!ConnectToAiServer()) {
    state.last_error = "connection_failed";
    return std::nullopt;
  }

  if (!state.recv_queue.empty()) {
    std::string message = std::move(state.recv_queue.front());
    state.recv_queue.pop_front();
    state.last_error.clear();
    return message;
  }

  const bool custom_timeout = timeout_seconds.has_value();
  const int custom_ms = ToMilliseconds(timeout_seconds,
                                       state.default_rcv_timeout_ms);
  if (custom_timeout) {
    SetSocketTimeout(state.socket, ZMQ_RCVTIMEO, custom_ms);
  }

  ReceiveResult result = ReceiveSocketPayloads(state.socket, 0);

  if (custom_timeout &&
      custom_ms != state.default_rcv_timeout_ms) {
    RestoreDefaultTimeout(state, ZMQ_RCVTIMEO);
  }

  if (result.ok && !result.messages.empty()) {
    if (result.messages.size() > 1) {
      for (size_t i = 1; i < result.messages.size(); ++i) {
        state.recv_queue.emplace_back(result.messages[i]);
      }
    }
    state.last_error.clear();
    return result.messages[0];
  }

  if (IsTemporaryError(result.error_code)) {
    state.last_error = "timeout";
    return std::nullopt;
  }

  if (!result.error_message.empty()) {
    state.last_error = result.error_message;
  } else {
    state.last_error = "unknown_error";
  }
  AIPARA_LOG_WARN(
      logger_, "AI转换ZeroMQ读取失败: " + state.last_error);
  DisconnectFromAiServer();
  return std::nullopt;
}

std::optional<std::string> TcpZmq::ReadAllFromAiSocket(
    std::optional<double> timeout_seconds) {
  std::optional<std::string> first =
      ReadFromAiSocket(timeout_seconds);
  if (!first) {
    return std::nullopt;
  }

  std::vector<std::string> messages;
  messages.emplace_back(std::move(*first));
  while (true) {
    std::optional<std::string> next = ReadFromAiSocket(0.0);
    if (!next) {
      break;
    }
    messages.emplace_back(std::move(*next));
  }

  std::string combined;
  for (size_t i = 0; i < messages.size(); ++i) {
    if (i > 0) {
      combined.push_back('\n');
    }
    combined.append(messages[i]);
  }
  AIPARA_LOG_DEBUG(logger_,
                   "📥 累计读取AI消息数量: " +
                       std::to_string(messages.size()));
  return combined;
}

TcpZmq::LatestAiMessage TcpZmq::ReadLatestFromAiSocket(
    std::optional<double> timeout_seconds) {
  LatestAiMessage result;
  if (!ConnectToAiServer()) {
    result.status = LatestStatus::kError;
    result.error_msg =
        std::string("服务未连接且重连失败");
    return result;
  }

  const double timeout = timeout_seconds.value_or(0.1);
  std::optional<std::string> latest = ReadFromAiSocket(timeout);
  if (!latest) {
    if (!ai_convert_.last_error.empty() &&
        ai_convert_.last_error != "timeout") {
      result.status = LatestStatus::kError;
      result.error_msg = ai_convert_.last_error;
    } else {
      result.status = LatestStatus::kTimeout;
    }
    return result;
  }

  int total_lines = 1;
  while (true) {
    std::optional<std::string> next = ReadFromAiSocket(0.0);
    if (!next) {
      break;
    }
    *latest = std::move(*next);
    ++total_lines;
  }

  if (total_lines > 1) {
    AIPARA_LOG_DEBUG(
        logger_, "🎯 共读取了 " + std::to_string(total_lines) +
                     " 条消息，保留最后一条");
  } else {
    AIPARA_LOG_DEBUG(logger_,
                     "📥 从AI转换服务读取到1条最新消息");
  }

  AIPARA_LOG_DEBUG(logger_, "🎯 返回最新消息: " + *latest);

  result.status = LatestStatus::kSuccess;
  result.raw_message = *latest;
  if (auto parsed = ParseSocketData(*latest)) {
    result.data.emplace(std::move(*parsed));
  }
  return result;
}

std::optional<rapidjson::Document> TcpZmq::ParseSocketData(
    const std::string& data) {
  if (data.empty()) {
    return std::nullopt;
  }
  AIPARA_LOG_DEBUG(
      logger_, "🔍 解析socket数据data: " + data +
                   " (长度: " + std::to_string(data.size()) + ")");

  rapidjson::Document doc;
  rapidjson::ParseResult parse_result =
      doc.Parse(data.c_str(), data.size());
  if (!parse_result) {
    AIPARA_LOG_ERROR(
        logger_, "❌ 解析TCP数据失败: " + data +
                     " error: " +
                     std::string(rapidjson::GetParseError_En(
                         parse_result.Code())));
    return std::nullopt;
  }

  AIPARA_LOG_DEBUG(logger_, "🔍 解析TCP数据成功");
  return doc;
}

bool TcpZmq::UpdateConfigField(rime::Config* config,
                               const std::string& field_path,
                               const rapidjson::Value& field_value) {
  if (!config) {
    return false;
  }
  bool changed = false;
  if (field_value.IsBool()) {
    bool current = false;
    const bool has_value = config->GetBool(field_path, &current);
    const bool new_value = field_value.GetBool();
    if (!has_value || current != new_value) {
      config->SetBool(field_path, new_value);
      changed = true;
      AIPARA_LOG_DEBUG(
          logger_, "表字段更新布尔值: " + field_path + " = " +
                       (new_value ? "true" : "false"));
    }
  } else if (field_value.IsInt()) {
    int current = 0;
    const bool has_value = config->GetInt(field_path, &current);
    const int new_value = field_value.GetInt();
    if (!has_value || current != new_value) {
      config->SetInt(field_path, new_value);
      changed = true;
      AIPARA_LOG_DEBUG(
          logger_, "表字段更新整数: " + field_path + " = " +
                       std::to_string(new_value));
    }
  } else if (field_value.IsDouble()) {
    double current = 0.0;
    const bool has_value = config->GetDouble(field_path, &current);
    const double new_value = field_value.GetDouble();
    if (!has_value || std::abs(current - new_value) > 1e-9) {
      config->SetDouble(field_path, new_value);
      changed = true;
      AIPARA_LOG_DEBUG(
          logger_, "表字段更新浮点数: " + field_path + " = " +
                       std::to_string(new_value));
    }
  } else if (field_value.IsString()) {
    std::string current;
    const bool has_value = config->GetString(field_path, &current);
    const std::string new_value(field_value.GetString(),
                                field_value.GetStringLength());
    if (!has_value || current != new_value) {
      config->SetString(field_path, new_value);
      changed = true;
      AIPARA_LOG_DEBUG(
          logger_, "表字段更新字符串: " + field_path + " = " +
                       new_value);
    }
  } else {
    AIPARA_LOG_WARN(
        logger_, "表字段类型暂不支持自动更新: " + field_path);
  }
  return changed;
}

bool TcpZmq::UpdateConfigTable(rime::Config* config,
                               const std::string& base_path,
                               const rapidjson::Value& value) {
  if (!config || !value.IsObject()) {
    return false;
  }
  bool changed = false;
  for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
    const std::string key(it->name.GetString(),
                          it->name.GetStringLength());
    const std::string child_path = base_path + "/" + key;
    if (it->value.IsObject()) {
      if (UpdateConfigTable(config, child_path, it->value)) {
        changed = true;
      }
    } else {
      if (UpdateConfigField(config, child_path, it->value)) {
        changed = true;
      }
    }
  }
  return changed;
}

const std::string& TcpZmq::EnglishModeSymbol(rime::Context*,
                                             rime::Config* config,
                                             std::string* buffer) {
  static const std::string kEmpty;
  if (!buffer) {
    return kEmpty;
  }
  buffer->clear();
  if (config && config->GetString("translator/english_mode_symbol",
                                  buffer)) {
    return *buffer;
  }
  return kEmpty;
}

bool TcpZmq::HandleSocketCommand(const rapidjson::Value& command_message,
                                 rime::Engine* engine) {
  if (!command_message.IsObject() || !engine) {
    return false;
  }
  rime::Context* context = engine->context();
  rime::Config* config =
      engine->schema() ? engine->schema()->config() : nullptr;

  const auto command_opt =
      GetOptionalString(command_message, "command");
  if (!command_opt) {
    return false;
  }
  const std::string& command = *command_opt;

  AIPARA_LOG_DEBUG(logger_,
                   "🎯 处理TCP命令: " + command);

  if (command == "ping") {
    AIPARA_LOG_DEBUG(logger_, "📞 收到ping命令");
    WriteToRimeSocket("{\"response\":\"pong\"}");
    return true;
  }

  if (command == "set_option") {
    if (!context) {
      return true;
    }

    const auto option_name =
        GetOptionalString(command_message, "option_name");
    bool option_value = false;
    if (!option_name ||
        !GetOptionalBool(command_message, "option_value",
                         &option_value)) {
      return false;
    }

    if (context->get_option(*option_name) != option_value) {
      update_global_option_state_ = true;
      SetGlobalOption(*option_name, option_value);
      context->set_option(*option_name, option_value);
      AIPARA_LOG_DEBUG(
          logger_,
          "tcp_zmq.update_global_option_state = true");
    }
    return true;
  }

  if (command == "set_config") {
    if (!config) {
      return false;
    }

    const auto config_path =
        GetOptionalString(command_message, "config_path");
    if (!config_path) {
      return false;
    }

    const std::string rime_config_path =
        DotPathToRimePath(*config_path);

    AIPARA_LOG_INFO(logger_, "🔧 收到配置变更通知:");
    if (auto config_name =
            GetOptionalString(command_message, "config_name")) {
      AIPARA_LOG_INFO(logger_,
                      "   配置名称: " + *config_name);
    }
    AIPARA_LOG_INFO(logger_,
                    "   配置路径: " + rime_config_path);

    bool success = false;
    bool need_refresh = false;

    if (command_message.HasMember("config_value") &&
        !command_message["config_value"].IsNull()) {
      const rapidjson::Value& config_value =
          command_message["config_value"];
      if (config_value.IsBool()) {
        config->SetBool(rime_config_path, config_value.GetBool());
        success = true;
        need_refresh = true;
        AIPARA_LOG_DEBUG(
            logger_, "设置布尔配置: " + rime_config_path);
      } else if (config_value.IsInt()) {
        config->SetInt(rime_config_path, config_value.GetInt());
        success = true;
        need_refresh = true;
        AIPARA_LOG_DEBUG(
            logger_, "设置整数配置: " + rime_config_path);
      } else if (config_value.IsDouble()) {
        config->SetDouble(rime_config_path,
                          config_value.GetDouble());
        success = true;
        need_refresh = true;
        AIPARA_LOG_DEBUG(
            logger_, "设置浮点数配置: " + rime_config_path);
      } else if (config_value.IsString()) {
        config->SetString(
            rime_config_path,
            std::string(config_value.GetString(),
                        config_value.GetStringLength()));
        success = true;
        need_refresh = true;
        AIPARA_LOG_DEBUG(
            logger_, "设置字符串配置: " + rime_config_path);
      } else if (config_value.IsObject()) {
        const bool changed =
            UpdateConfigTable(config, rime_config_path,
                              config_value);
        success = true;
        need_refresh = changed;
        if (changed) {
          AIPARA_LOG_DEBUG(
              logger_, "表配置更新完成: " + rime_config_path);
        } else {
          AIPARA_LOG_DEBUG(
              logger_, "表配置未发生变化: " + rime_config_path);
        }
      } else {
        AIPARA_LOG_WARN(
            logger_, "不支持的配置值类型: " + rime_config_path);
      }
    } else {
      success = true;
      config->SetString(rime_config_path, "__DELETED__");
      need_refresh = true;
      AIPARA_LOG_DEBUG(
          logger_, "设置配置删除标记: " + rime_config_path +
                       " = __DELETED__");
    }

    if (success) {
      if (need_refresh) {
        UpdateConfigs(config);
        AIPARA_LOG_INFO(logger_,
                        "✅ update_all_modules_config配置更新成功");
        UpdateProperty("config_update_flag", "1");
      } else {
        AIPARA_LOG_DEBUG(logger_,
                         "表配置无变化，跳过模块刷新: " +
                             rime_config_path);
      }
    } else {
      AIPARA_LOG_ERROR(logger_,
                       "❌ 配置更新失败: " + rime_config_path);
    }
    return true;
  }

  if (command == "set_property") {
    const auto property_name =
        GetOptionalString(command_message, "property_name");
    const auto property_value =
        GetOptionalString(command_message, "property_value");
    if (property_name && property_value) {
      UpdateProperty(*property_name, *property_value);
    }
    return true;
  }

  if (command == "clipboard_data") {
    if (!context) {
      return true;
    }
    AIPARA_LOG_DEBUG(logger_, "command_messege: clipboard_data");

    const rapidjson::Value* clipboard =
        command_message.HasMember("clipboard")
            ? &command_message["clipboard"]
            : nullptr;
    std::string clipboard_text;
    if (clipboard && clipboard->IsObject()) {
      if (auto text = GetOptionalString(*clipboard, "text")) {
        clipboard_text = *text;
      }
    }

    bool success_flag = true;
    GetOptionalBool(command_message, "success", &success_flag);
    if (!success_flag) {
      std::string err_msg;
      if (clipboard && clipboard->HasMember("error") &&
          (*clipboard)["error"].IsString()) {
        err_msg.assign((*clipboard)["error"].GetString(),
                       (*clipboard)["error"].GetStringLength());
      } else if (auto error =
                     GetOptionalString(command_message, "error")) {
        err_msg = *error;
      } else {
        err_msg = "unknown";
      }
      AIPARA_LOG_WARN(
          logger_,
          "get_clipboard 返回失败，错误信息: " + err_msg);
      return true;
    }

    if (!clipboard_text.empty()) {
      std::string symbol_buffer;
      const std::string& english_mode_symbol =
          EnglishModeSymbol(context, config, &symbol_buffer);
      if (!english_mode_symbol.empty()) {
        ReplaceAll(&clipboard_text, english_mode_symbol, " ");
      }

      const std::string rawenglish_prompt =
          context->get_property("rawenglish_prompt");
      std::string new_input = context->input();
      if (rawenglish_prompt == "1") {
        new_input.append(clipboard_text);
        AIPARA_LOG_DEBUG(logger_,
                         "get_clipboard 粘贴clipboard_text: " +
                             clipboard_text);
      } else {
        new_input.append(english_mode_symbol);
        new_input.append(clipboard_text);
        new_input.append(english_mode_symbol);
        AIPARA_LOG_DEBUG(
            logger_,
            "get_clipboard 粘贴clipboard_text: " +
                english_mode_symbol + clipboard_text +
                english_mode_symbol);
      }
      context->set_input(new_input);
    } else {
      AIPARA_LOG_WARN(
          logger_, "get_clipboard 命令未提供有效的文本可追加");
      rime::Composition& composition = context->composition();
      if (!composition.empty()) {
        composition.back().prompt = " [剪贴板为空] ";
      }
    }
    return true;
  }

  if (command == "paste_executed") {
    AIPARA_LOG_INFO(
        logger_, "✅ 服务端已成功执行粘贴操作");
    return true;
  }

  if (command == "paste_failed") {
    auto error = GetOptionalString(command_message, "error");
    AIPARA_LOG_ERROR(
        logger_, "❌ 服务端执行粘贴操作失败: " +
                     (error ? *error : "未知错误"));
    return true;
  }

  AIPARA_LOG_WARN(logger_, "❓ 未知的TCP命令: " + command);
  return false;
}

bool TcpZmq::ProcessRimeSocketData(
    rime::Engine* engine,
    std::optional<double> timeout_seconds) {
  if (!engine) {
    return false;
  }

  bool processed_any = false;
  int processed_count = 0;
  while (processed_count < kMaxProcessMessages) {
    std::optional<std::string> data =
        ReadFromRimeSocket(timeout_seconds);
    if (!data) {
      break;
    }

    AIPARA_LOG_DEBUG(
        logger_, "🎯 成功接收到Rime状态服务完整消息: " + *data);

    auto parsed_data = ParseSocketData(*data);
    if (parsed_data && parsed_data->IsObject()) {
      auto& doc = *parsed_data;
      if (doc.HasMember("messege_type") &&
          doc["messege_type"].IsString()) {
        const std::string message_type(
            doc["messege_type"].GetString(),
            doc["messege_type"].GetStringLength());
        if (message_type == "command_response") {
          AIPARA_LOG_DEBUG(
              logger_, "📨 检测到嵌套命令 command_response 字段.");
          if (doc.HasMember("command_messege")) {
            rapidjson::Value& command_message =
                doc["command_messege"];
            if (command_message.IsArray()) {
              int index = 0;
              for (auto& item : command_message.GetArray()) {
                ++index;
                if (!item.IsObject()) {
                  continue;
                }
                AIPARA_LOG_DEBUG(
                    logger_, "📨 处理第 " +
                                 std::to_string(index) +
                                 " 条命令");
                HandleSocketCommand(item, engine);
              }
            } else if (command_message.IsObject()) {
              HandleSocketCommand(command_message, engine);
            }
          }
        } else if (message_type == "command_executed") {
          AIPARA_LOG_INFO(
              logger_, "✅ 收到命令执行成功通知: paste_executed");
        }
      }
      processed_any = true;
    } else {
      AIPARA_LOG_WARN(
          logger_, "⚠️  Rime状态消息解析失败");
    }

    ++processed_count;
  }

  return processed_any;
}

bool TcpZmq::SyncWithServer(
    rime::Engine* engine,
    bool include_option_info,
    bool send_commit_text,
    const std::optional<std::string>& command_key,
    const std::optional<std::string>& command_value,
    std::optional<double> timeout_seconds,
    const std::optional<std::string>& position,
    const std::optional<std::string>& character) {
  if (!engine || !engine->context()) {
    return false;
  }
  rime::Context* context = engine->context();

  const std::int64_t current_time = NowMs();

  rapidjson::Document doc(rapidjson::kObjectType);
  rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

  doc.AddMember("messege_type", rapidjson::Value("state", allocator),
                allocator);
  doc.AddMember("is_composing", context->IsComposing(), allocator);
  doc.AddMember("timestamp", current_time, allocator);

  rapidjson::Value switches(rapidjson::kArrayType);
  if (include_option_info) {
    const char* simple_switches[] = {"ascii_punct"};
    for (const char* switch_name : simple_switches) {
      rapidjson::Value switch_obj(rapidjson::kObjectType);
      const bool state =
          context->get_option(switch_name);
      switch_obj.AddMember(
          "name", rapidjson::Value(switch_name, allocator),
          allocator);
      switch_obj.AddMember(
          "type", rapidjson::Value("simple", allocator), allocator);
      switch_obj.AddMember("state", state, allocator);
      switch_obj.AddMember("state_index", state ? 1 : 0, allocator);
      switches.PushBack(switch_obj, allocator);
    }
  }
  doc.AddMember("switches_option", switches, allocator);

  rapidjson::Value properties(rapidjson::kArrayType);
  const char* property_names[] = {"keepon_chat_trigger"};
  for (const char* property_name : property_names) {
    rapidjson::Value property(rapidjson::kObjectType);
    property.AddMember(
        "name", rapidjson::Value(property_name, allocator),
        allocator);
    property.AddMember(
        "type", rapidjson::Value("string", allocator), allocator);
    const std::string property_value =
        context->get_property(property_name);
    property.AddMember(
        "value", rapidjson::Value(property_value.c_str(), allocator),
        allocator);
    properties.PushBack(property, allocator);
  }
  doc.AddMember("properties", properties, allocator);

  if (command_key) {
    rapidjson::Value command_message(rapidjson::kObjectType);
    command_message.AddMember(
        "messege_type", rapidjson::Value("command", allocator),
        allocator);
    command_message.AddMember(
        "command", rapidjson::Value(command_key->c_str(), allocator),
        allocator);
    if (command_value) {
      command_message.AddMember(
          "command_value",
          rapidjson::Value(command_value->c_str(), allocator),
          allocator);
    }
    command_message.AddMember("timestamp", current_time, allocator);
    command_message.AddMember(
        "client_id", rapidjson::Value("lua_tcp_client", allocator),
        allocator);
    doc.AddMember("command_message", command_message, allocator);
  }

  if (send_commit_text) {
    doc["messege_type"].SetString("commit", allocator);
    const std::string current_app =
        context->get_property("client_app");
    doc.AddMember(
        "current_app", rapidjson::Value(current_app.c_str(), allocator),
        allocator);
    const std::string& input = context->input();
    doc.AddMember("commit_pinyin",
                  rapidjson::Value(input.c_str(), allocator), allocator);
    const std::string commit_text = context->GetCommitText();
    doc.AddMember(
        "commit_text",
        rapidjson::Value(commit_text.c_str(), allocator), allocator);
  }

  if (position &&
      *position == "unhandled_key_notifier" && character) {
    doc["messege_type"].SetString("commit", allocator);
    const std::string current_app =
        context->get_property("client_app");
    doc.AddMember(
        "current_app", rapidjson::Value(current_app.c_str(), allocator),
        allocator);
    doc.AddMember(
        "commit_pinyin",
        rapidjson::Value(character->c_str(), allocator), allocator);
    doc.AddMember(
        "commit_text",
        rapidjson::Value(character->c_str(), allocator), allocator);
  }

  const std::string json_data = JsonStringify(doc);
  WriteToRimeSocket(json_data);

  if (is_initialized_ && rime_state_.is_connected) {
    ProcessRimeSocketData(engine, timeout_seconds);
  }
  return true;
}

bool TcpZmq::SendConvertRequest(
    const std::string& schema_name,
    const std::string& shuru_schema,
    const std::string& confirmed_pos_input,
    const std::vector<std::string>& long_candidates_text,
    std::optional<double> timeout_seconds) {
  const double timeout =
      timeout_seconds.value_or(ai_convert_.timeout_seconds);

  rapidjson::Document doc(rapidjson::kObjectType);
  auto& allocator = doc.GetAllocator();
  doc.AddMember("messege_type", "convert", allocator);
  doc.AddMember(
      "confirmed_pos_input",
      rapidjson::Value(confirmed_pos_input.c_str(), allocator),
      allocator);
  doc.AddMember("schema_name",
                rapidjson::Value(schema_name.c_str(), allocator),
                allocator);
  doc.AddMember(
      "shuru_schema",
      rapidjson::Value(shuru_schema.c_str(), allocator), allocator);
  doc.AddMember("stream_mode", true, allocator);
  doc.AddMember("timestamp", NowMs(), allocator);
  doc.AddMember("timeout", timeout, allocator);

  if (!long_candidates_text.empty()) {
    rapidjson::Value candidates(rapidjson::kArrayType);
    for (const auto& text : long_candidates_text) {
      candidates.PushBack(
          rapidjson::Value(text.c_str(), allocator), allocator);
    }
    doc.AddMember("candidates_text", candidates, allocator);
  }

  const std::string json_data = JsonStringify(doc);
  AIPARA_LOG_DEBUG(logger_,
                   "发送转换请求json_data: " + json_data);
  return WriteToAiSocket(json_data);
}

TcpZmq::ConvertReadResult TcpZmq::ReadConvertResult(
    std::optional<double> timeout_seconds) {
  const double timeout = timeout_seconds.value_or(0.1);
  LatestAiMessage stream_result =
      ReadLatestFromAiSocket(timeout);

  ConvertReadResult result;
  result.status = stream_result.status;
  result.error_msg = stream_result.error_msg;

  if (stream_result.status == LatestStatus::kSuccess &&
      stream_result.data) {
    rapidjson::Document& parsed_data = stream_result.data.value();
    if (parsed_data.HasMember("messege_type") &&
        parsed_data["messege_type"].IsString() &&
        std::string(parsed_data["messege_type"].GetString(),
                    parsed_data["messege_type"].GetStringLength()) ==
            "convert_result_stream") {
      AIPARA_LOG_DEBUG(logger_, "读取到转换结果数据");
      if (parsed_data.HasMember("is_final") &&
          parsed_data["is_final"].IsBool()) {
        result.is_final = parsed_data["is_final"].GetBool();
      }
      if (parsed_data.HasMember("is_partial") &&
          parsed_data["is_partial"].IsBool()) {
        result.is_partial = parsed_data["is_partial"].GetBool();
      }
      if (parsed_data.HasMember("is_timeout") &&
          parsed_data["is_timeout"].IsBool()) {
        result.is_timeout = parsed_data["is_timeout"].GetBool();
      }
      if (parsed_data.HasMember("is_error") &&
          parsed_data["is_error"].IsBool()) {
        result.is_error = parsed_data["is_error"].GetBool();
      }
      result.data = std::move(stream_result.data);
    } else {
      AIPARA_LOG_DEBUG(
          logger_,
          "收到非转换结果数据，类型: " +
              (parsed_data.HasMember("messege_type") &&
                       parsed_data["messege_type"].IsString()
                   ? std::string(
                         parsed_data["messege_type"].GetString(),
                         parsed_data["messege_type"].GetStringLength())
                   : "unknown"));
      result.status = LatestStatus::kNoData;
    }
  } else if (stream_result.status == LatestStatus::kTimeout) {
    AIPARA_LOG_DEBUG(
        logger_,
        "转换结果读取超时(正常) - 服务端可能还没处理完成");
  } else if (stream_result.status == LatestStatus::kError) {
    AIPARA_LOG_ERROR(
        logger_,
        "转换结果读取错误: " +
            (stream_result.error_msg ? *stream_result.error_msg
                                     : "unknown"));
    result.is_final = true;
  } else {
    AIPARA_LOG_DEBUG(logger_,
                     "未知的转换结果读取状态");
  }

  return result;
}

bool TcpZmq::SendPasteCommand(rime::Engine* engine) {
  if (!engine) {
    return false;
  }

  rapidjson::Document doc(rapidjson::kObjectType);
  auto& allocator = doc.GetAllocator();
  doc.AddMember("messege_type", "command", allocator);
  doc.AddMember("command", "paste", allocator);
  doc.AddMember("timestamp", NowMs(), allocator);
  doc.AddMember("client_id", "lua_tcp_client", allocator);

  const std::string json_data = JsonStringify(doc);
  AIPARA_LOG_DEBUG(
      logger_, "发送粘贴命令json_data: " + json_data);

  if (!WriteToRimeSocket(json_data)) {
    AIPARA_LOG_ERROR(logger_, "❌ 粘贴命令发送失败");
    return false;
  }

  AIPARA_LOG_INFO(logger_, "🍴 粘贴命令发送成功，等待服务端执行");
  if (ProcessRimeSocketData(engine)) {
    AIPARA_LOG_INFO(logger_, "📥 收到粘贴命令执行响应");
  } else {
    AIPARA_LOG_WARN(logger_, "⚠️ 未收到粘贴命令执行响应");
  }
  return true;
}

bool TcpZmq::SendChatMessage(
    const std::string& commit_text,
    const std::string& assistant_id,
    const std::optional<std::string>& response_key) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& allocator = doc.GetAllocator();
  doc.AddMember("messege_type", "chat", allocator);
  doc.AddMember(
      "commit_text",
      rapidjson::Value(commit_text.c_str(), allocator), allocator);
  doc.AddMember(
      "assistant_id",
      rapidjson::Value(assistant_id.c_str(), allocator), allocator);
  doc.AddMember("timestamp", NowMs(), allocator);
  if (response_key) {
    doc.AddMember(
        "response_key",
        rapidjson::Value(response_key->c_str(), allocator), allocator);
  }

  const std::string json_data = JsonStringify(doc);
  AIPARA_LOG_DEBUG(
      logger_, "发送对话消息json_data: " + json_data);
  return WriteToAiSocket(json_data);
}

bool TcpZmq::IsSystemReady() const {
  return is_initialized_ &&
         (rime_state_.is_connected || ai_convert_.is_connected);
}

bool TcpZmq::IsRimeSocketReady() const {
  return is_initialized_ && rime_state_.is_connected;
}

bool TcpZmq::IsAiSocketReady() const {
  return is_initialized_ && ai_convert_.is_connected;
}

bool TcpZmq::ForceReconnect() {
  AIPARA_LOG_INFO(logger_, "强制重置所有TCP连接状态");

  rime_state_.last_connect_attempt = 0;
  ai_convert_.last_connect_attempt = 0;
  rime_state_.connection_failures = 0;
  ai_convert_.connection_failures = 0;
  rime_state_.write_failure_count = 0;
  ai_convert_.write_failure_count = 0;

  DisconnectFromServer();

  const bool rime_connected = ConnectToRimeServer();
  const bool ai_connected = ConnectToAiServer();

  AIPARA_LOG_INFO(logger_,
                  "强制重连结果 - Rime:" +
                      std::string(rime_connected ? "true" : "false") +
                      " AI:" +
                      std::string(ai_connected ? "true" : "false"));

  return rime_connected || ai_connected;
}

void TcpZmq::SetConnectionParams(
    const std::string& host,
    std::optional<int> rime_port,
    std::optional<int> ai_port) {
  if (!host.empty()) {
    host_ = host;
  }
  if (rime_port) {
    rime_state_.port = *rime_port;
  }
  if (ai_port) {
    ai_convert_.port = *ai_port;
  }
  AIPARA_LOG_DEBUG(
      logger_,
      "连接参数已更新: " + host_ + " Rime:" +
          std::to_string(rime_state_.port) + " AI:" +
          std::to_string(ai_convert_.port));
}

TcpZmq::Stats TcpZmq::GetStats() const {
  Stats stats;
  stats.is_initialized = is_initialized_;
  stats.host = host_;
  stats.rime_state.port = rime_state_.port;
  stats.rime_state.is_connected = rime_state_.is_connected;
  stats.rime_state.connection_failures =
      rime_state_.connection_failures;
  stats.rime_state.write_failure_count =
      rime_state_.write_failure_count;
  stats.rime_state.timeout_seconds = rime_state_.timeout_seconds;

  stats.ai_convert.port = ai_convert_.port;
  stats.ai_convert.is_connected = ai_convert_.is_connected;
  stats.ai_convert.connection_failures =
      ai_convert_.connection_failures;
  stats.ai_convert.write_failure_count =
      ai_convert_.write_failure_count;
  stats.ai_convert.timeout_seconds = ai_convert_.timeout_seconds;

  return stats;
}

TcpZmq::ConnectionInfo TcpZmq::GetConnectionInfo() const {
  ConnectionInfo info;
  info.host = host_;
  info.rime_state.port = rime_state_.port;
  info.rime_state.is_connected = rime_state_.is_connected;
  info.ai_convert.port = ai_convert_.port;
  info.ai_convert.is_connected = ai_convert_.is_connected;
  return info;
}

}  // namespace rime::aipara
