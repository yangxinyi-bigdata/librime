#include "tcp_socket_sync.h"

#include <rime/config.h>
#include <rime/context.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
// 以上四个头文件来自 Rime 核心库，提供了配置对象 Config、输入上下文 Context 等类的定义。
// 它们就像 Python 包里的模块。只要你想在这里访问 Rime 的运行时信息，就必须包含对应的头文件。

#ifdef Bool
#undef Bool
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
// rapidjson 是一个高性能的 C++ JSON 库。这里包含的是解析、序列化相关的组件。
// 可以把它类比成 Python 的 json 模块，不过需要显式管理字符串缓冲区。

#include <zmq.hpp>
// zmq.hpp 是 ZeroMQ 提供的 C++ 封装，内部还是调用 libzmq 的 C API。
// 有了这个封装，就可以像使用 C++ 对象一样创建 socket、发送/接收消息。

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// 这些都是 C++ 标准库头文件，提供了常用的工具：
//   * <algorithm> 里的 std::replace 等函数；
//   * <chrono> 用于时间处理（类似 Python datetime + time 模块）；
//   * <optional> 表示“可能没有值”的对象，类似 Python 的 Optional[T]；
//   * <thread> 则是 C++11 的线程库；
//   * 其余的提供字符串处理、容器等。

namespace rime::aipara {
// 命名空间(namespace)用于给代码分组，防止名字冲突。
// 这里使用 C++17 的“嵌套命名空间”写法，相当于 namespace rime { namespace aipara { ... } }。

namespace {
// 匿名命名空间意味着这些工具函数/常量只在当前 .cc 文件可见。
// 想象成 Python 模块里的私有函数（以 _ 开头），只是 C++ 用语法强制限制了作用域。

// 统一定义轮询、重连等核心参数，方便后续调优。
constexpr std::chrono::milliseconds kWorkerPollInterval{50};
constexpr std::chrono::milliseconds kReconnectThrottle{500};
constexpr std::chrono::milliseconds kDefaultWaitTimeout{100};
constexpr int kDefaultRimeTimeoutMs = 100;
constexpr int kDefaultAiTimeoutMs = 5000;
constexpr int kDefaultHwm = 100;
// constexpr 表示“编译期常量”，类似 Python 里的全局常量，但 C++ 会把它们放在只读内存。
// 这些值控制线程轮询、重连节奏、ZeroMQ 高水位线。修改它们会直接影响网络行为。

using rapidjson::Document;
using rapidjson::Value;

// 将主机和端口拼出标准 ZeroMQ endpoint。
std::string MakeEndpoint(const std::string& host, int port) {
  std::ostringstream stream;
  stream << "tcp://" << host << ":" << port;
  return stream.str();
  // std::ostringstream 类似 Python 的 io.StringIO，用于拼接字符串。
  // ZeroMQ 的连接地址统一是 tcp://ip:port 这样的格式，所以这里做了一个小工具函数。
}

// 获取当前时间（毫秒）。
int64_t CurrentTimeMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  // std::chrono::system_clock::now() 返回一个时间点，duration_cast 会把它转换成毫秒（64 位整数）。
  // 这样既能复用 Lua 时期的时间戳逻辑，又方便写入 JSON。
}

// 将 RapidJSON 文档序列化成字符串，方便通过 ZeroMQ 发送。
std::string SerializeJson(const Document& doc) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  return buffer.GetString();
  // RapidJSON 没有像 Python json.dumps 那样的一行函数，需要先创建缓冲区，再用 writer 写入。
  // doc.Accept(writer) 会把文档树遍历一遍，写入到 buffer 里。
}

// 将 RapidJSON 值转换成字符串表示，用于调试日志。
std::string ToString(const Value& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return buffer.GetString();
  // 这个函数在需要打印 Value 的时候很有用，尤其是调试配置字段更新时。
}

// 辅助函数：比较配置并在值变化时写回，返回是否发生实际修改。
bool UpdateConfigField(Config* config,
                       const std::string& path,
                       const Value& value,
                       Logger* logger) {
  if (!config) {
    return false;
  }
  bool changed = false;
  // 下面的 if-else 分支根据 JSON 值的类型决定调用 Config 的哪个 setter。
  // Config::SetBool / SetInt / SetString 等成员函数就像 Python dict 的 setdefault，
  // 但它们写的是 Rime 的配置树。
  if (value.IsBool()) {
    bool current = value.GetBool();
    const bool existed = config->GetBool(path, &current);
    if (!existed || current != value.GetBool()) {
      config->SetBool(path, value.GetBool());
      changed = true;
    }
  } else if (value.IsInt()) {
    int current = value.GetInt();
    const bool existed = config->GetInt(path, &current);
    if (!existed || current != value.GetInt()) {
      config->SetInt(path, value.GetInt());
      changed = true;
    }
  } else if (value.IsInt64()) {
    int current = static_cast<int>(value.GetInt64());
    const bool existed = config->GetInt(path, &current);
    if (!existed || static_cast<int64_t>(current) != value.GetInt64()) {
      config->SetInt(path, static_cast<int>(value.GetInt64()));
      changed = true;
    }
  } else if (value.IsDouble()) {
    double current = value.GetDouble();
    const bool existed = config->GetDouble(path, &current);
    if (!existed || current != value.GetDouble()) {
      config->SetDouble(path, value.GetDouble());
      changed = true;
    }
  } else if (value.IsString()) {
    std::string current = value.GetString();
    const bool existed = config->GetString(path, &current);
    const std::string new_value = value.GetString();
    if (!existed || current != new_value) {
      config->SetString(path, new_value);
      changed = true;
    }
  } else {
    if (logger) {
      AIPARA_LOG_WARN(*logger, "不支持的配置项类型: " + path);
    }
  }
  if (changed && logger) {
    AIPARA_LOG_DEBUG(*logger, "配置项更新: " + path);
  }
  return changed;
  // 函数的返回值告诉调用者“这个字段是否真的发生了变化”，
  // 这样就可以避免对配置做无意义的刷新（刷新配置会触发 Rime 的重建，比较耗时）。
}

// 递归更新配置表，对比差异后再写入，避免无谓刷新。
bool UpdateConfigTable(Config* config,
                       const std::string& base_path,
                       const Value& table,
                       Logger* logger) {
  if (!config || !table.IsObject()) {
    return false;
  }
  bool changed = false;
  for (auto itr = table.MemberBegin(); itr != table.MemberEnd(); ++itr) {
    const std::string child_key = itr->name.GetString();
    const std::string child_path = base_path + "/" + child_key;
    if (itr->value.IsObject()) {
      if (UpdateConfigTable(config, child_path, itr->value, logger)) {
        changed = true;
      }
    } else {
      if (UpdateConfigField(config, child_path, itr->value, logger)) {
        changed = true;
      }
    }
  }
  return changed;
}

// 将字符串安全替换成 Rime 配置路径（将 . 替换成 /）。
std::string NormalizeConfigPath(std::string path) {
  std::replace(path.begin(), path.end(), '.', '/');
  return path;
  // Rime 的配置路径使用斜杠分隔（类似文件路径），
  // 但我们从 Python 服务端收到的是点分隔路径（类似 JSON）。
  // std::replace 会原地修改字符串，把所有 '.' 替换成 '/'。
}

}  // namespace

// ================================
// AiparaSocketBridge 实现
// ================================

class TcpSocketSync::AiparaSocketBridge {
 public:
  explicit AiparaSocketBridge(Logger logger)
      : logger_(std::move(logger)), context_(1) {}

  void RequestReconnect() {
    reconnect_needed_.store(true);
  }

  void ApplyPendingReconnect() {
    if (!reconnect_needed_.exchange(false)) {
      return;
    }
    DisconnectAll();
  }

  bool ConnectRime(const TcpConnectionInfo& info) {
    if (info.rime_port <= 0) {
      // 如果端口无效（<=0），就直接认定无法连接。C++ 中 int 没有 None，所以用这种判定。
      rime_connected_.store(false);
      return false;
    }
    const std::string endpoint = MakeEndpoint(info.host, info.rime_port);
    if (rime_socket_ && endpoint == last_rime_endpoint_) {
      // 如果已经连上相同的 endpoint，就不必重复创建 socket，这是一个性能优化。
      return true;
    }
    try {
      rime_socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::req);
      // std::make_unique 创建一个独占指针，出作用域时会自动释放资源，相当于 Python 的 with context。
      ConfigureSocket(rime_socket_.get());
      rime_socket_->set(zmq::sockopt::sndhwm, kDefaultHwm);
      rime_socket_->set(zmq::sockopt::rcvhwm, kDefaultHwm);
      rime_socket_->set(zmq::sockopt::rcvtimeo, kDefaultRimeTimeoutMs);
      rime_socket_->set(zmq::sockopt::sndtimeo, kDefaultRimeTimeoutMs);
      // 这里设置 send / recv 的超时时间，避免无限阻塞。
      rime_socket_->connect(endpoint);
      waiting_for_rime_reply_ = false;
      rime_connected_.store(true);
      last_rime_endpoint_ = endpoint;
      AIPARA_LOG_INFO(logger_, "Rime ZeroMQ 连接已建立: " + endpoint);
      return true;
    } catch (const zmq::error_t& error) {
      rime_connected_.store(false);
      AIPARA_LOG_ERROR(logger_, std::string("Rime ZeroMQ 连接失败: ") + error.what());
      rime_socket_.reset();
      // 捕获 zmq::error_t，可以读取 error.what() 得到具体错误信息。
      // reset() 会释放当前 socket，下次再调用 ConnectRime 时会重新创建。
      return false;
    }
  }

  bool ConnectAi(const TcpConnectionInfo& info) {
    if (info.ai_port <= 0) {
      // 同样地，端口非法直接返回失败。
      ai_connected_.store(false);
      return false;
    }
    const std::string endpoint = MakeEndpoint(info.host, info.ai_port);
    if (ai_socket_ && endpoint == last_ai_endpoint_) {
      return true;
    }
    try {
      ai_socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::dealer);
      ConfigureSocket(ai_socket_.get());
      ai_socket_->set(zmq::sockopt::sndhwm, kDefaultHwm);
      ai_socket_->set(zmq::sockopt::rcvhwm, kDefaultHwm);
      ai_socket_->set(zmq::sockopt::rcvtimeo, kDefaultAiTimeoutMs);
      ai_socket_->set(zmq::sockopt::sndtimeo, kDefaultAiTimeoutMs);
      // AI 端允许消息等待更久，因此使用较大的超时。
      ai_socket_->connect(endpoint);
      ai_connected_.store(true);
      last_ai_endpoint_ = endpoint;
      AIPARA_LOG_INFO(logger_, "AI ZeroMQ 连接已建立: " + endpoint);
      return true;
    } catch (const zmq::error_t& error) {
      ai_connected_.store(false);
      AIPARA_LOG_ERROR(logger_, std::string("AI ZeroMQ 连接失败: ") + error.what());
      ai_socket_.reset();
      return false;
    }
  }

  void DisconnectRime() {
    waiting_for_rime_reply_ = false;
    if (rime_socket_) {
      try {
        rime_socket_->close();
      } catch (...) {
        // 捕获所有异常，避免关闭过程中抛出错误影响主逻辑。
        // 这是一个常见的 C++ 防御式写法。
      }
      rime_socket_.reset();
    }
    rime_connected_.store(false);
  }

  void DisconnectAi() {
    if (ai_socket_) {
      try {
        ai_socket_->close();
      } catch (...) {
        // 同理，忽略 close 失败带来的异常。
      }
      ai_socket_.reset();
    }
    ai_connected_.store(false);
  }

  void DisconnectAll() {
    DisconnectRime();
    DisconnectAi();
  }

  bool CanSendRime() const { return !waiting_for_rime_reply_; }

  bool SendRime(const std::string& payload) {
    if (!rime_socket_) {
      // 如果还没有连接成功，那么直接返回 false；调用者可以随后触发重连。
      return false;
    }
    if (!CanSendRime()) {
      // 避免违反 REQ/REP 的“先 send 再 recv”规则。
      return false;
    }
    try {
      rime_socket_->send(zmq::buffer(payload), zmq::send_flags::none);
      waiting_for_rime_reply_ = true;
      return true;
    } catch (const zmq::error_t& error) {
      AIPARA_LOG_ERROR(logger_,
                       std::string("Rime ZeroMQ 发送失败: ") + error.what());
      rime_connected_.store(false);
      return false;
    }
  }

  bool SendAi(const std::string& payload) {
    if (!ai_socket_) {
      // AI 套接字是异步的，但仍要确保指针存在。
      return false;
    }
    try {
      ai_socket_->send(zmq::buffer(payload), zmq::send_flags::none);
      return true;
    } catch (const zmq::error_t& error) {
      AIPARA_LOG_ERROR(logger_,
                       std::string("AI ZeroMQ 发送失败: ") + error.what());
      ai_connected_.store(false);
      return false;
    }
  }

  void Poll(std::chrono::milliseconds timeout,
            ThreadSafeQueue<std::string>* rime_queue,
            ThreadSafeQueue<std::string>* ai_queue) {
    std::vector<zmq::pollitem_t> items;
    int rime_index = -1;
    int ai_index = -1;
    // pollitem_t 记录需要监听的 socket 以及事件掩码。
    // 这里用两个索引记录各自位置，方便后面判断哪一个触发了事件。
    if (rime_socket_) {
      rime_index = static_cast<int>(items.size());
      items.emplace_back(zmq::pollitem_t{rime_socket_->handle(), 0, ZMQ_POLLIN, 0});
    }
    if (ai_socket_) {
      ai_index = static_cast<int>(items.size());
      items.emplace_back(zmq::pollitem_t{ai_socket_->handle(), 0, ZMQ_POLLIN, 0});
    }
    if (items.empty()) {
      // 如果连一个 socket 都没有，说明当前还没连接成功，直接睡眠以避免空转。
      std::this_thread::sleep_for(timeout);
      return;
    }
    try {
      zmq::poll(items.data(), items.size(), timeout);
    } catch (const zmq::error_t& error) {
      AIPARA_LOG_WARN(logger_, std::string("ZeroMQ 轮询失败: ") + error.what());
      return;
    }
    if (rime_index >= 0 && (items[rime_index].revents & ZMQ_POLLIN)) {
      std::string message;
      if (ReceiveMessage(*rime_socket_, &message)) {
        waiting_for_rime_reply_ = false;
        if (rime_queue) {
          rime_queue->Push(std::move(message));
          // std::move 把字符串的所有权转交给队列，避免额外拷贝（C++ 中字符串复制成本较高）。
        }
      }
    }
    if (ai_index >= 0 && (items[ai_index].revents & ZMQ_POLLIN)) {
      std::string message;
      if (ReceiveMessage(*ai_socket_, &message)) {
        if (ai_queue) {
          ai_queue->Push(std::move(message));
        }
      }
    }
  }

  bool IsRimeConnected() const { return rime_connected_.load(); }
  bool IsAiConnected() const { return ai_connected_.load(); }

 private:
  void ConfigureSocket(zmq::socket_t* socket) {
    socket->set(zmq::sockopt::linger, 0);
    socket->set(zmq::sockopt::reconnect_ivl, 1000);
    socket->set(zmq::sockopt::reconnect_ivl_max, 5000);
    socket->set(zmq::sockopt::tcp_keepalive, 1);
    socket->set(zmq::sockopt::tcp_keepalive_idle, 30);
    socket->set(zmq::sockopt::tcp_keepalive_cnt, 3);
    socket->set(zmq::sockopt::tcp_keepalive_intvl, 10);
    // linger=0 表示关闭 socket 时立即丢弃未发送的消息；
    // reconnect_ivl / reconnect_ivl_max 控制自动重连的时间间隔；
    // tcp_keepalive 相关参数用于检测死连接，相当于 Python socket.setsockopt(SO_KEEPALIVE)。
  }

  bool ReceiveMessage(zmq::socket_t& socket, std::string* out) {
    if (!out) {
      return false;
    }
    try {
      zmq::message_t frame;
      std::ostringstream assembled;
      bool first_frame = true;
      while (true) {
        if (!socket.recv(frame, zmq::recv_flags::none)) {
          break;
        }
        if (!first_frame) {
          assembled << '\n';
        }
        first_frame = false;
        assembled.write(static_cast<const char*>(frame.data()),
                        static_cast<std::streamsize>(frame.size()));
        if (!frame.more()) {
          break;
        }
      }
      // ROUTER/DEALER 模型可能会拆成多帧（例如第一帧是路由 identity），
      // 这里逐帧拼接成一个字符串。为了可读性，用换行符分隔不同帧。
      *out = assembled.str();
      return true;
    } catch (const zmq::error_t& error) {
      AIPARA_LOG_WARN(logger_,
                      std::string("ZeroMQ 接收失败: ") + error.what());
      return false;
    }
  }

  Logger logger_;
  zmq::context_t context_;
  std::unique_ptr<zmq::socket_t> rime_socket_;
  std::unique_ptr<zmq::socket_t> ai_socket_;
  std::atomic<bool> rime_connected_{false};
  std::atomic<bool> ai_connected_{false};
  std::atomic<bool> reconnect_needed_{false};
  bool waiting_for_rime_reply_ = false;
  std::string last_rime_endpoint_;
  std::string last_ai_endpoint_;
};

// ================================
// TcpSocketSync 实现
// ================================

TcpSocketSync::TcpSocketSync()
    : logger_(MakeLogger("tcp_socket_sync")) {
  // 初始化列表里的 MakeLogger 会立即创建一个 Logger 对象。
  // C++ 中成员变量的初始化顺序固定为“声明顺序”，所以写在冒号后面是最佳实践。
  connection_info_.host = "127.0.0.1";
  AIPARA_LOG_INFO(logger_, "TcpSocketSync 构造完成，等待初始化");
  // 构造函数只做最基本的初始化工作，不创建线程也不连网络。
  // 这和 Python 里把 heavy 操作放在 __post_init__ 或专门的方法里是一样的。
}

bool TcpSocketSync::Init() {
  if (initialized_.exchange(true)) {
    // exchange(true) 返回旧值，如果旧值是 true，说明已经初始化过了。
    return true;
  }
  AIPARA_LOG_INFO(logger_, "TcpSocketSync 初始化，准备启动 ZeroMQ 后台线程");
  shutdown_.store(false);
  bridge_ = std::make_unique<AiparaSocketBridge>(logger_);
  start_worker_if_needed();
  return true;
  // Init 做的事情：
  //   1. 把 shutdown_ 清零；
  //   2. 创建 ZeroMQ 桥接对象；
  //   3. 启动后台线程。
  // 之所以不在构造函数里做，是为了让外部可以在设置完 host/port 后再启动线程。
}

void TcpSocketSync::Fini() {
  AIPARA_LOG_INFO(logger_, "TcpSocketSync 开始关闭流程");
  shutdown_.store(true);
  stop_worker();
  if (bridge_) {
    bridge_->DisconnectAll();
  }
  initialized_.store(false);
  // Fini 与 Init 对应：设置 shutdown_，等待线程退出，再关闭 socket。
  // 这段逻辑要在析构之前执行，否则线程可能在对象销毁后仍访问已释放的内存。
}

void TcpSocketSync::SetConfigUpdateHandler(
    std::function<void(const Config&)> config_update_function,
    std::function<void(const std::string&, const std::string&)>
        property_update_function) {
  update_all_modules_config_ = std::move(config_update_function);
  property_update_function_ = std::move(property_update_function);
  // std::move 把回调的所有权转移到成员变量里。
  // 如果传入的是 lambda，会发生一次复制；如果是 std::function 临时对象，则会原地构造。
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
  // 这些包装函数的好处是：主线程只管调用 TcpSocketSync，内部会判断回调是否存在。
  // 避免了业务层自己去 if (handler) handler() 的重复代码。
}

void TcpSocketSync::SetGlobalOption(const std::string& name, bool value) {
  global_option_state_[name] = value;
  update_global_option_state_ = true;
  // 这里沿用了 Lua 时代的设计：记录所有 option 的最新状态，并在后续 Apply 时统一生效。
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
  TcpConnectionInfo snapshot;
  {
    std::scoped_lock lock(connection_mutex_);
    // std::scoped_lock 相当于 Python 的 with mutex: 块，出了作用域自动解锁。
    connection_info_.host = std::move(host);
    connection_info_.rime_port = rime_port;
    connection_info_.ai_port = ai_port;
    snapshot = connection_info_;
  }
  reconnect_requested_.store(true);
  AIPARA_LOG_INFO(
      logger_,
      "连接参数更新: host=" + snapshot.host +
          " rime_port=" + std::to_string(snapshot.rime_port) +
          " ai_port=" + std::to_string(snapshot.ai_port));
}

TcpConnectionInfo TcpSocketSync::GetConnectionInfo() const {
  std::scoped_lock lock(connection_mutex_);
  return connection_info_;
  // 返回一个拷贝（struct 在 C++ 中可以按值返回）。
  // 由于 TcpConnectionInfo 只是几个简单字段，这里的复制成本可以接受。
}

bool TcpSocketSync::IsSystemReady() const {
  std::scoped_lock lock(connection_mutex_);
  return connection_info_.rime_connected && connection_info_.ai_connected;
}

bool TcpSocketSync::IsRimeSocketReady() const {
  std::scoped_lock lock(connection_mutex_);
  return connection_info_.rime_connected;
}

bool TcpSocketSync::IsAiSocketReady() const {
  std::scoped_lock lock(connection_mutex_);
  return connection_info_.ai_connected;
}

void TcpSocketSync::ForceReconnect() {
  reconnect_requested_.store(true);
  if (bridge_) {
    bridge_->RequestReconnect();
  }
  // “软重连”：设置标志位，让后台线程下一轮 poll 时断开重连。
  // 这样可以避免在主线程直接操作 ZeroMQ socket（ZeroMQ 不支持跨线程操作）。
}

bool TcpSocketSync::SendConvertRequest(const std::string& schema_name,
                                       const std::string& shuru_schema,
                                       const std::string& confirmed_pos_input,
                                       const std::string& long_candidates_json,
                                       const std::string& extra_payload) {
  Document doc(rapidjson::kObjectType);
  auto& allocator = doc.GetAllocator();
  doc.AddMember("messege_type", "convert", allocator);
  doc.AddMember("schema_name",
                rapidjson::Value(schema_name.c_str(), allocator), allocator);
  doc.AddMember("shuru_schema",
                rapidjson::Value(shuru_schema.c_str(), allocator), allocator);
  doc.AddMember("confirmed_pos_input",
                rapidjson::Value(confirmed_pos_input.c_str(), allocator),
                allocator);
  doc.AddMember("stream_mode", true, allocator);
  doc.AddMember("timestamp", CurrentTimeMillis(), allocator);
  doc.AddMember("timeout", kDefaultAiTimeoutMs / 1000.0, allocator);
  // RapidJSON 的 AddMember 会把数据写入文档树。
  // 注意：字符串需要提供 allocator，否则文档销毁后指针会失效（这是 C++ 常见坑）。

  if (!long_candidates_json.empty()) {
    Document candidates_doc;
    if (!candidates_doc.Parse(long_candidates_json.c_str()).HasParseError() &&
        candidates_doc.IsArray()) {
      Value arr(rapidjson::kArrayType);
      for (const auto& item : candidates_doc.GetArray()) {
        if (item.IsObject() && item.HasMember("text") &&
            item["text"].IsString()) {
          arr.PushBack(Value(item["text"].GetString(), allocator), allocator);
        }
      }
      doc.AddMember("candidates_text", arr, allocator);
      // 候选词列表是 AI 服务端补齐上下文时的重要信息。这里做一次 JSON -> JSON 的转化。
      // PushBack 会复制字符串内容到 convert 文档的 allocator 中，避免悬挂引用。
    }
  }

  if (!extra_payload.empty()) {
    Document extra_doc;
    if (!extra_doc.Parse(extra_payload.c_str()).HasParseError() &&
        extra_doc.IsObject()) {
      for (auto itr = extra_doc.MemberBegin(); itr != extra_doc.MemberEnd();
           ++itr) {
        Value key(itr->name.GetString(), allocator);
        Value value(itr->value, allocator);
        doc.AddMember(key, value, allocator);
      }
      // 这段逻辑允许上层在调用时附加自定义字段（例如 prompt 或其他 meta 信息）。
      // 注意：AddMember 要求 key 是 rapidjson::Value，所以需要额外构造一次。
    }
  }

  const std::string payload = SerializeJson(doc);
  AIPARA_LOG_DEBUG(logger_, "发送 AI 转换请求: " + payload);
  return send_to_ai_socket(payload);
}

std::optional<std::string> TcpSocketSync::ReadConvertResult(
    double timeout_seconds) {
  const auto message = read_latest_from_ai_socket(timeout_seconds);
  if (!message) {
    return std::nullopt;
  }
  // std::optional 就像 Python 的 Optional[str]：要么有字符串，要么是空。
  // read_latest_from_ai_socket 会从线程安全队列里取出最新的一条 AI 消息。

  Document doc;
  if (doc.Parse(message->c_str()).HasParseError()) {
    AIPARA_LOG_WARN(logger_,
                    "AI 返回非 JSON 数据，直接返回原始字符串: " + *message);
    return message;
  }
  if (!doc.HasMember("messege_type")) {
    return message;
  }
  const auto& type = doc["messege_type"];
  if (type.IsString() &&
      std::string_view(type.GetString()) == "convert_result_stream") {
    return message;
  }
  // 对于非转换类消息，仍然返回给调用者，由上层自行决定如何处理。
  return message;
}

bool TcpSocketSync::SendChatMessage(const std::string& commit_text,
                                    const std::string& assistant_id,
                                    const std::string& response_key) {
  Document doc(rapidjson::kObjectType);
  auto& allocator = doc.GetAllocator();
  doc.AddMember("messege_type", "chat", allocator);
  doc.AddMember("commit_text",
                rapidjson::Value(commit_text.c_str(), allocator), allocator);
  doc.AddMember("assistant_id",
                rapidjson::Value(assistant_id.c_str(), allocator), allocator);
  doc.AddMember("timestamp", CurrentTimeMillis(), allocator);
  if (!response_key.empty()) {
    doc.AddMember("response_key",
                  rapidjson::Value(response_key.c_str(), allocator), allocator);
  }
  // response_key 是可选字段，只有在服务端指定快捷按键时才会出现。

  const std::string payload = SerializeJson(doc);
  AIPARA_LOG_DEBUG(logger_, "发送 AI 对话请求: " + payload);
  return send_to_ai_socket(payload);
}

std::optional<std::string> TcpSocketSync::ReadLatestAiMessage(
    double timeout_seconds) {
  return read_latest_from_ai_socket(timeout_seconds);
}

void TcpSocketSync::SyncWithServer() {
  SyncWithServer(nullptr, nullptr);
}

void TcpSocketSync::SyncWithServer(Context* context) {
  SyncWithServer(context, nullptr);
}

void TcpSocketSync::SyncWithServer(Context* context, Config* config) {
  // context / config 可能是空指针（nullptr），主线程会根据调用场景传入不同组合。
  // C++ 中的指针就像 Python 里的可选对象，需要手动判空。
  std::string payload;
  while (rime_message_queue_.TryPop(&payload)) {
    process_rime_socket_payload(payload, context, config);
  }
  // SyncWithServer 会在主线程被调用（例如云端处理器的 Tick）。
  // TryPop 循环把后台线程积累的消息一条条取出来处理，直到队列清空。
  // 这样保证了 Rime 主线程永远只在自己的线程内访问 Context / Config，避免跨线程访问崩溃。
}

// ================================
// 私有辅助方法实现
// ================================

bool TcpSocketSync::connect_to_rime_server() {
  if (!bridge_) {
    return false;
  }
  TcpConnectionInfo snapshot;
  {
    std::scoped_lock lock(connection_mutex_);
    snapshot = connection_info_;
  }
  // 同样的策略：先复制，后连接。
  // 这里通过复制 snapshot 避免在调用 ZeroMQ 时持有锁。
  const bool ok = bridge_->ConnectRime(snapshot);
  {
    std::scoped_lock lock(connection_mutex_);
    connection_info_.rime_connected = ok;
    // connection_mutex_ 保护 connection_info_，避免主线程和后台线程同时写导致数据竞争。
  }
  return ok;
  // connect_to_* 一律先复制 connection_info_，再调用 bridge_，最后写回状态。
  // 这样做的原因是 ZeroMQ socket 只能在创建它的线程里操作（这里是 worker 线程），
  // 所以主线程只能通过 bridge_ 间接操作。
}

bool TcpSocketSync::connect_to_ai_server() {
  if (!bridge_) {
    return false;
  }
  TcpConnectionInfo snapshot;
  {
    std::scoped_lock lock(connection_mutex_);
    snapshot = connection_info_;
  }
  const bool ok = bridge_->ConnectAi(snapshot);
  {
    std::scoped_lock lock(connection_mutex_);
    connection_info_.ai_connected = ok;
    // 这里的写入会被主线程的 GetConnectionInfo() 读取，因此必须加锁。
  }
  return ok;
}

void TcpSocketSync::disconnect_from_rime_server() {
  if (!bridge_) {
    return;
  }
  bridge_->DisconnectRime();
  std::scoped_lock lock(connection_mutex_);
  connection_info_.rime_connected = false;
}

void TcpSocketSync::disconnect_from_ai_server() {
  if (!bridge_) {
    return;
  }
  bridge_->DisconnectAi();
  std::scoped_lock lock(connection_mutex_);
  connection_info_.ai_connected = false;
}

void TcpSocketSync::disconnect_from_server() {
  if (!bridge_) {
    return;
  }
  bridge_->DisconnectAll();
  std::scoped_lock lock(connection_mutex_);
  connection_info_.rime_connected = false;
  connection_info_.ai_connected = false;
}

bool TcpSocketSync::send_to_rime_socket(const std::string& json_payload) {
  if (!initialized_.load()) {
    Init();
    // 如果外部忘记调用 Init，这里会自动完成初始化。相当于提供一个“惰性启动”。
  }
  rime_outgoing_queue_.Push(json_payload);
  start_worker_if_needed();
  return true;
  // send_to_rime_socket 并不直接访问 ZeroMQ，而是把消息放到队列里。
  // 这类似 Python 中把任务放到 queue.Queue，然后由后台线程消费。
  // start_worker_if_needed() 确保后台线程已经启动。
}

bool TcpSocketSync::send_to_ai_socket(const std::string& json_payload) {
  if (!initialized_.load()) {
    Init();
  }
  ai_outgoing_queue_.Push(json_payload);
  start_worker_if_needed();
  return true;
}

std::optional<std::string> TcpSocketSync::read_latest_from_ai_socket(
    double timeout_seconds) {
  const auto timeout =
      timeout_seconds > 0.0
          ? std::chrono::milliseconds(static_cast<int64_t>(timeout_seconds * 1000))
          : std::chrono::milliseconds(0);
  std::string latest;
  if (timeout.count() > 0) {
    if (!ai_message_queue_.WaitPop(&latest, timeout)) {
      return std::nullopt;
    }
  } else {
    if (!ai_message_queue_.TryPop(&latest)) {
      return std::nullopt;
    }
  }
  // WaitPop 会在队列空时阻塞一小段时间，适合轮询式的“等待下一帧”场景；
  // TryPop 则是立即返回，用于超时时间为 0 的场景。
  std::string candidate;
  while (ai_message_queue_.TryPop(&candidate)) {
    latest = std::move(candidate);
  }
  return latest;
  // 读取“最新”消息的逻辑：先取一条，如果队列里还有，就把后面的覆盖前面的。
  // 这样可以丢弃旧片段，尽可能减少流式堆积的延迟。
  // 注意 std::move(candidate) 能避免多次复制字符串，这在 C++ 中非常重要。
}

void TcpSocketSync::process_rime_socket_payload(const std::string& payload,
                                                Context* context,
                                                Config* config) {
  if (payload.empty()) {
    return;
  }
  Document doc;
  if (doc.Parse(payload.c_str()).HasParseError()) {
    AIPARA_LOG_WARN(logger_, "Rime 端返回的 JSON 解析失败: " + payload);
    return;
  }
  // rapidjson::Document 的 Parse 接受 C 字符串指针，解析失败时 HasParseError() 会返回 true。
  if (!doc.HasMember("messege_type")) {
    AIPARA_LOG_DEBUG(logger_, "Rime 消息缺少 messege_type 字段: " + payload);
    return;
  }

  const auto& type = doc["messege_type"];
  if (!type.IsString()) {
    return;
  }
  const std::string message_type = type.GetString();
  if (message_type == "command_response") {
    // command_response 是 Python 端返回的一批命令（数组或单个对象）。
    // 这里逐条调用 handle_socket_command，保证逻辑与 Lua 版本一致。
    if (doc.HasMember("command_messege")) {
      const auto& command_msg = doc["command_messege"];
      if (command_msg.IsArray()) {
        for (const auto& item : command_msg.GetArray()) {
          handle_socket_command(item, context, config);
        }
      } else if (command_msg.IsObject()) {
        handle_socket_command(command_msg, context, config);
      }
    }
  } else if (message_type == "command_executed") {
    AIPARA_LOG_INFO(logger_, "收到命令执行完成通知: " + payload);
  } else {
    AIPARA_LOG_DEBUG(logger_, "收到未知 Rime 消息类型: " + message_type);
  }
}

void TcpSocketSync::handle_socket_command(const Value& command,
                                          Context* context,
                                          Config* config) {
  if (!command.IsObject() || !command.HasMember("command")) {
    return;
  }
  const auto& command_name_value = command["command"];
  if (!command_name_value.IsString()) {
    return;
  }
  const std::string command_name = command_name_value.GetString();
  AIPARA_LOG_DEBUG(logger_, "处理 Rime 命令: " + command_name);

  if (command_name == "ping") {
    // 服务端定期发送 ping，用于检测活跃状态。这里直接回复 pong。
    Document response(rapidjson::kObjectType);
    auto& allocator = response.GetAllocator();
    response.AddMember("response", "pong", allocator);
    response.AddMember("timestamp", CurrentTimeMillis(), allocator);
    send_to_rime_socket(SerializeJson(response));
    return;
  }

  if (command_name == "set_option") {
    // 修改 Rime 开关选项，与 Lua 时代的逻辑相同。
    if (!command.HasMember("option_name") || !command.HasMember("option_value")) {
      return;
    }
    const auto& option_name = command["option_name"];
    const auto& option_value = command["option_value"];
    if (!option_name.IsString() || !option_value.IsBool()) {
      return;
    }
    if (context) {
      if (context->get_option(option_name.GetString()) != option_value.GetBool()) {
        context->set_option(option_name.GetString(), option_value.GetBool());
      }
      SetGlobalOption(option_name.GetString(), option_value.GetBool());
    } else {
      SetGlobalOption(option_name.GetString(), option_value.GetBool());
    }
    // 即使没有 Context（例如在非交互时），也要把值记录到全局状态，供下一次 Apply 使用。

    Document response(rapidjson::kObjectType);
    auto& allocator = response.GetAllocator();
    response.AddMember("response", "option_set", allocator);
    response.AddMember("option_name",
                       rapidjson::Value(option_name.GetString(), allocator),
                       allocator);
    response.AddMember("success", true, allocator);
    response.AddMember("timestamp", CurrentTimeMillis(), allocator);
    response.AddMember("responding_to", "set_option", allocator);
    send_to_rime_socket(SerializeJson(response));
    return;
  }

  if (command_name == "set_config") {
    if (!config) {
      AIPARA_LOG_WARN(logger_, "set_config 命令收到，但当前上下文未提供 Config 对象");
      return;
    }
    // 注意：Config* 是裸指针，可能为 nullptr。必须先判空，否则会触发访问冲突。
    // set_config 带来了配置路径 + 新值，需要写入 Rime 的 Config。
    // C++ 的 Config::SetXX 接口会立即更新内存中的配置树。
    if (!command.HasMember("config_path") ||
        !command.HasMember("config_value")) {
      return;
    }
    const auto& config_path_value = command["config_path"];
    if (!config_path_value.IsString()) {
      return;
    }
    const std::string config_path = NormalizeConfigPath(config_path_value.GetString());
    // NormalizeConfigPath 把 "translator.option" 这种点分路径改成 Rime 需要的 "translator/option"。
    const Value& config_value = command["config_value"];
    bool success = false;
    bool need_refresh = false;

    if (config_value.IsObject()) {
      need_refresh = UpdateConfigTable(config, config_path, config_value, &logger_);
      success = true;
    } else {
      success = UpdateConfigField(config, config_path, config_value, &logger_);
      need_refresh = success;
    }

    if (success && need_refresh) {
      UpdateConfigs(*config);
      UpdateProperty("config_update_flag", "1");
      // 更新成功后调用回调，让上层“热刷新”模块；同时设置一个 property 作为其他模块的通知。
    }
    return;
  }

  if (command_name == "set_property") {
    // set_property 直接调用 UpdateProperty，通常用于同步客户端状态到 Rime Context。
    if (!command.HasMember("property_name") ||
        !command.HasMember("property_value")) {
      return;
    }
    const auto& property_name = command["property_name"];
    const auto& property_value = command["property_value"];
    if (!property_name.IsString() || !property_value.IsString()) {
      return;
    }
    UpdateProperty(property_name.GetString(), property_value.GetString());
    return;
  }

  if (command_name == "clipboard_data") {
    // 剪贴板内容需要写入当前 Rime 上下文，因此必须在主线程处理。
    if (!context) {
      AIPARA_LOG_WARN(logger_, "clipboard_data 命令收到，但当前上下文为空");
      return;
    }
    bool success_flag = true;
    std::string clipboard_text;
    if (command.HasMember("success") && command["success"].IsBool()) {
      success_flag = command["success"].GetBool();
    }
    if (!success_flag) {
      std::string error = "unknown";
      if (command.HasMember("error") && command["error"].IsString()) {
        error = command["error"].GetString();
      }
      AIPARA_LOG_WARN(logger_, "获取剪贴板失败: " + error);
      return;
    }
    if (command.HasMember("clipboard") && command["clipboard"].IsObject()) {
      const auto& clipboard_obj = command["clipboard"];
      if (clipboard_obj.HasMember("text") && clipboard_obj["text"].IsString()) {
        clipboard_text = clipboard_obj["text"].GetString();
      }
    }
    if (clipboard_text.empty()) {
      auto& composition = context->composition();
      if (!composition.empty()) {
        composition.back().prompt = " [剪贴板为空] ";
      }
      return;
    }

    std::string english_mode_symbol;
    if (config) {
      config->GetString("translator/english_mode_symbol", &english_mode_symbol);
    }
    if (!english_mode_symbol.empty()) {
      size_t pos = 0;
      while ((pos = clipboard_text.find(english_mode_symbol, pos)) !=
             std::string::npos) {
        clipboard_text.replace(pos, english_mode_symbol.length(), " ");
        pos += 1;
      }
      // 为了兼容英文模式，服务端会把特定符号包裹的文本替换成空格。
      // find + replace 的写法和 Python 的 while find != -1 类似，只是语法更啰嗦。
    }
    const std::string rawenglish_prompt = context->get_property("rawenglish_prompt");
    std::string new_input = context->input();
    if (rawenglish_prompt == "1") {
      new_input += clipboard_text;
    } else {
      new_input += english_mode_symbol + clipboard_text + english_mode_symbol;
    }
    context->set_input(new_input);
    // Context::set_input 会直接修改未上屏的输入串，相当于 Lua 时代操作 context.input。
    // 注意：Context 不是线程安全对象，所以一定要在主线程调用。
    return;
  }

  if (command_name == "paste_executed") {
    AIPARA_LOG_INFO(logger_, "服务端粘贴命令已执行");
    return;
  }

  if (command_name == "paste_failed") {
    if (command.HasMember("error") && command["error"].IsString()) {
      AIPARA_LOG_ERROR(logger_, "服务端粘贴命令失败: " +
                                     std::string(command["error"].GetString()));
    } else {
      AIPARA_LOG_ERROR(logger_, "服务端粘贴命令失败: 未提供错误信息");
    }
    return;
  }

  AIPARA_LOG_WARN(logger_, "未识别的命令: " + command_name);
}

void TcpSocketSync::start_worker_if_needed() {
  if (running_.load()) {
    return;
  }
  running_.store(true);
  worker_thread_ = std::thread(&TcpSocketSync::worker_loop, this);
  // std::thread 构造时需要一个函数指针，这里传入成员函数指针 &TcpSocketSync::worker_loop，
  // 同时把 this 作为第一个参数（类似 Python 的 threading.Thread(target=obj.method, args=())）。
}

void TcpSocketSync::stop_worker() {
  if (!running_.exchange(false)) {
    return;
  }
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  // join() 会阻塞当前线程直到后台线程结束，相当于等待 worker 完成清理工作。
}

void TcpSocketSync::worker_loop() {
  AIPARA_LOG_INFO(logger_, "ZeroMQ 后台线程启动");
  while (!shutdown_.load()) {
    // shutdown_ 是一个原子标志，外部调用 Fini() 会把它设为 true，循环随之退出。
    if (bridge_) {
      if (reconnect_requested_.exchange(false)) {
        bridge_->RequestReconnect();
      }
      bridge_->ApplyPendingReconnect();
    }

    connect_to_rime_server();
    connect_to_ai_server();
    // connect_to_* 会在连接断开时尝试重新连接；如果已经连接则立即返回。

    if (bridge_) {
      std::string payload;
      if (bridge_->CanSendRime() && rime_outgoing_queue_.TryPop(&payload)) {
        if (!bridge_->SendRime(payload)) {
          rime_outgoing_queue_.PushFront(std::move(payload));
          std::this_thread::sleep_for(kReconnectThrottle);
        }
      }
      // Rime 通道由于使用 REQ/REP，一次只能发送一条，失败时把消息放回队头等待重试。
      while (ai_outgoing_queue_.TryPop(&payload)) {
        if (!bridge_->SendAi(payload)) {
          ai_outgoing_queue_.PushFront(std::move(payload));
          break;
        }
      }
      // AI 通道可以一次发送多条，如果发送失败就中断循环，以免忙等。
      bridge_->Poll(kWorkerPollInterval, &rime_message_queue_, &ai_message_queue_);

      {
        std::scoped_lock lock(connection_mutex_);
        connection_info_.rime_connected = bridge_->IsRimeConnected();
        connection_info_.ai_connected = bridge_->IsAiConnected();
      }
      // 每一轮 poll 之后刷新连接状态，方便主线程查询。
    } else {
      std::this_thread::sleep_for(kWorkerPollInterval);
    }
  }
  AIPARA_LOG_INFO(logger_, "ZeroMQ 后台线程退出");
}

}  // namespace rime::aipara
