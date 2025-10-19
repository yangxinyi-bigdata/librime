#ifndef PLUGINS_AIPARA_SRC_COMMON_TCP_SOCKET_SYNC_H_
#define PLUGINS_AIPARA_SRC_COMMON_TCP_SOCKET_SYNC_H_
// 这一行是“头文件保护”(include guard)，防止同一个头文件被重复包含导致编译错误。
// #define 会在第一次包含时定义一个宏，后续再包含遇到同样的宏就会跳过整个文件。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logger.h"
// 头文件里只写声明，不写具体实现（实现放在 .cc）。这里提前包含 logger 的声明，方便在类里使用。

namespace rime {
class Config;
class Context;
}  // namespace rime

namespace rapidjson {
class Value;
}  // namespace rapidjson

namespace rime::aipara {

// 这个结构体用来保存“如何连接服务器”的信息。struct 在 C++ 里和 class 类似，只是默认成员是 public。
// 可以把它理解成 Python 里的简单数据类：只是把几项数据打包在一起，没有行为。
struct TcpConnectionInfo {
  std::string host;
  // std::string 等价于 Python 的 str，但它是一个真实的类型，需要通过 #include<string> 引入。
  // 这里 host 保存目标服务器的主机地址（例如 "127.0.0.1"）。
  int rime_port = 0;
  int ai_port = 0;
  // 默认值写在成员后面（C++11 语法），就好像 Python dataclass 的默认值。
  bool rime_connected = false;
  bool ai_connected = false;
};

// TcpSocketSync 是 ZeroMQ 通信层的“门面类”。从 Rime 插件的角度，这个类负责：
//   1. 连接 ZeroMQ 服务端；
//   2. 把要发送的消息排队，交给后台线程发送；
//   3. 接收后台线程收到的消息，交给 Rime 主线程处理；
//   4. 对外暴露 Lua 时代同名的接口，减少业务层改动。
// C++ class 和 Python class 有几处差别：
//   * 成员函数的声明和实现可以分开（声明在头文件，实现在 cc 文件）。
//   * 访问控制（public / private）决定外部是否能看到成员。
class TcpSocketSync {
 public:
  // 构造函数，名字和类名相同。没有返回值。用于初始化成员。
  TcpSocketSync();

  // Init / Fini 类似于 Python 里手动调用的 setup / teardown。
  // 在 Init 里会创建 ZeroMQ 上下文并启动后台线程。
  bool Init();
  void Fini();

  // SetConfigUpdateHandler 允许外部把回调函数传进来。
  // std::function 类似 Python 的 callable，把任何可调用对象包装成统一的类型。
  void SetConfigUpdateHandler(
      std::function<void(const Config&)> config_update_function,
      std::function<void(const std::string&, const std::string&)> property_update_function);

  // UpdateConfigs / UpdateProperty 会调用前面设置的回调，通知业务层刷新配置/属性。
  void UpdateConfigs(const Config& config);
  void UpdateProperty(const std::string& property_name,
                      const std::string& property_value);

  // SetGlobalOption / ApplyGlobalOptionsToContext 保留了 Lua 的“全局开关”逻辑。
  void SetGlobalOption(const std::string& name, bool value);
  int ApplyGlobalOptionsToContext(Context* context);

  // Rime 运行时会先调用 SetConnectionParams 传入 host + port，再调用 Init。
  void SetConnectionParams(std::string host, int rime_port, int ai_port);
  TcpConnectionInfo GetConnectionInfo() const;

  // 一组便捷的状态查询函数，方便外部判断网络连接状态。
  bool IsSystemReady() const;
  bool IsRimeSocketReady() const;
  bool IsAiSocketReady() const;
  void ForceReconnect();

  // 下列接口对应 Lua 时代的功能：发送转换请求 / 读取转换结果 / 聊天消息 / 主线程同步。
  // 这里保留原有函数名，让业务层可以“无感迁移”。
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
  void SyncWithServer(Context* context);
  void SyncWithServer(Context* context, Config* config);

 private:
  // 下面是一个模板类（即“泛型”），用来实现线程安全的队列。
  // 模板语法 template<typename T> 表示可以把任意类型塞进去，C++ 编译器会在用到时生成具体版本。
  // 你可以把它理解成 Python typing.Generic 的静态展开版。
  template <typename T>
  class ThreadSafeQueue {
   public:
    ThreadSafeQueue() = default;

    // Push 等价于 Python queue.Queue.put，但这里用的是互斥量 + 条件变量手写实现。
    void Push(T value) {
      {
        std::scoped_lock lock(mutex_);
        queue_.push_back(std::move(value));
        // std::move 会把 value 的所有权“转移”到队列里，避免多一次复制。
      }
      cv_.notify_one();
    }

    // PushFront 在 ZeroMQ 发送失败时回退用，把消息插回队头。
    void PushFront(T value) {
      {
        std::scoped_lock lock(mutex_);
        queue_.push_front(std::move(value));
      }
      cv_.notify_one();
    }

    // TryPop 相当于非阻塞读取：如果队列为空，直接返回 false。
    bool TryPop(T* value) {
      if (!value) {
        return false;
      }
      std::scoped_lock lock(mutex_);
      if (queue_.empty()) {
        return false;
      }
      *value = std::move(queue_.front());
      queue_.pop_front();
      return true;
    }

    // WaitPop 等价于“阻塞等待”：反复等待直到队列里有数据或者超时。
    // std::condition_variable 是 C++ 的等待/通知原语，和 Python threading.Condition 类似。
    bool WaitPop(T* value, std::chrono::milliseconds timeout) {
      if (!value) {
        return false;
      }
      std::unique_lock lock(mutex_);
      if (!cv_.wait_for(lock, timeout, [this]() { return !queue_.empty(); })) {
        return false;
      }
      *value = std::move(queue_.front());
      queue_.pop_front();
      return true;
    }

    // Empty / Clear 都是辅助工具，和 Python deque 的同名方法一样。
    bool Empty() const {
      std::scoped_lock lock(mutex_);
      return queue_.empty();
    }

    void Clear() {
      std::scoped_lock lock(mutex_);
      queue_.clear();
    }

   private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
  };

  class AiparaSocketBridge;
  // AiparaSocketBridge 是私有内部类，用来封装 ZeroMQ 的底层操作。
  // 我们在 .cc 文件里定义它的细节，头文件这里只做“前向声明”。
  // 这样可以减少头文件依赖，提高编译速度。

  // 下面这些私有成员函数没有对外暴露，只在类内部使用。
  // 命名遵循 snake_case，和 Lua 时代一样，调用方可以直接联想到原逻辑。
  bool connect_to_rime_server();
  bool connect_to_ai_server();
  void disconnect_from_rime_server();
  void disconnect_from_ai_server();
  void disconnect_from_server();
  bool send_to_rime_socket(const std::string& json_payload);
  bool send_to_ai_socket(const std::string& json_payload);
  std::optional<std::string> read_latest_from_ai_socket(double timeout_seconds);
  void process_rime_socket_payload(const std::string& payload,
                                   Context* context,
                                   Config* config);
  void handle_socket_command(const rapidjson::Value& command,
                             Context* context,
                             Config* config);

  void start_worker_if_needed();
  void stop_worker();
  void worker_loop();

  Logger logger_;
  // Logger 是自定义的日志类，类似 Python logging。放在成员里便于整个类调用。
  std::unordered_map<std::string, bool> global_option_state_;
  bool update_global_option_state_ = false;
  // unordered_map 就像 Python 的 dict，存储 option -> bool。

  std::function<void(const Config&)> update_all_modules_config_;
  std::function<void(const std::string&, const std::string&)> property_update_function_;
  // std::function 可以存储任意可调用对象（函数指针、lambda、std::bind...）。
  // 这里把回调保存起来，等收到服务器消息时再调用。

  TcpConnectionInfo connection_info_;
  // connection_info_ 保存连接状态。由于多个线程会读写，访问时要配合 mutex。
  std::unique_ptr<AiparaSocketBridge> bridge_;
  // unique_ptr 是“独占指针”，负责对象的生命周期；类似 Python 中“把对象交给另一个对象托管”，离开作用域自动删除。
  std::thread worker_thread_;
  // std::thread 封装 C++11 原生线程。后台线程会处理 ZeroMQ 的 poll 循环。
  std::atomic<bool> initialized_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> reconnect_requested_{false};
  // std::atomic 用来在多线程之间共享布尔状态，不需要额外加锁。类似“线程安全的标志位”。
  mutable std::mutex connection_mutex_;
  // mutable 允许在 const 成员函数里也能加锁（比如 GetConnectionInfo），这是 C++ 的常见技巧。

  ThreadSafeQueue<std::string> rime_message_queue_;
  ThreadSafeQueue<std::string> ai_message_queue_;
  ThreadSafeQueue<std::string> rime_outgoing_queue_;
  ThreadSafeQueue<std::string> ai_outgoing_queue_;
  // 四个队列分别存：
  //   rime_message_queue_   → 后台线程收到的 Rime 服务器回复，等待主线程处理；
  //   ai_message_queue_     → AI 服务器的流式回复；
  //   rime_outgoing_queue_  → 主线程准备好的命令，等后台线程发送到 Rime 服务器；
  //   ai_outgoing_queue_    → 发送给 AI 服务器的请求。
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_COMMON_TCP_SOCKET_SYNC_H_
