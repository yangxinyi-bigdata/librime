#ifndef PLUGINS_AIPARA_SRC_AI_ASSISTANT_SEGMENTOR_H_
#define PLUGINS_AIPARA_SRC_AI_ASSISTANT_SEGMENTOR_H_

// 头文件通常只放“声明”，便于其他源文件引用。
// include guard（上面这两行宏）防止头文件被重复包含导致的重定义错误，
// 语法是 #ifndef / #define / #endif，一定要成对出现，是 C++ 的常见基础语法。

// 引入 Rime 框架提供的公共接口。尖括号表示系统或第三方库头文件。
#include <rime/common.h>
#include <rime/segmentor.h>

// C++ 标准库容器与字符串类型，功能与 Python 的 str 和 dict 类似。
#include <string>
#include <unordered_map>

// 项目内部的日志工具头文件，使用双引号表示相对路径包含。
#include "common/logger.h"

// 前向声明：这里只声明类的名字，不需要完整定义即可让指针/引用类型工作。
// 有助于降低耦合、减少编译耗时。类似 Python 在类型注解里写 "ClassName"。
namespace rime {
class Config;
class Context;
class Schema;
class Segmentation;
}  // namespace rime

namespace rime::aipara {
// 使用 C++17 的嵌套命名空间写法（rime::aipara），作用是避免名字冲突并分模块管理。

// struct 默认成员是 public，适合做纯数据载体。
// 这里记录了 AI 助手的行为策略开关，默认值写在等号右侧。
struct AiAssistantSegmentorBehavior {
  bool commit_question = false;
  bool auto_commit_reply = false;
  bool clipboard_mode = false;
  std::string prompt_chat;
};

// 用于记录触发器相关的附加信息，在运行时帮助定位具体的聊天场景。
struct TriggerMetadata {
  std::string trigger_name;
  std::string trigger_prefix;
  std::string chat_name;
};

// Segmentor 是 Rime 输入法里负责“切分输入串”的基类。
// 这里通过 public 继承（: public Segmentor）来扩展它，就像 Python 子类继承父类。
class AiAssistantSegmentor : public Segmentor {
 public:
  // explicit 阻止编译器进行隐式转换，Ticket& 表示引用类型，类似 Python 里的传参“原地引用”。
  explicit AiAssistantSegmentor(const Ticket& ticket);

  // override 说明这是重写基类的虚函数，编译器会帮忙检查签名是否匹配。
  // 函数参数 Segmentation* 是指针，可能为 nullptr，要在实现里做安全检查。
  bool Proceed(Segmentation* segmentation) override;

  // 当外部配置变更时，可主动调用此函数刷新内部缓存。
  void UpdateCurrentConfig(Config* config);

 private:
  // EnsureConfigLoaded：确保配置已经按当前 schema 加载。
  // ResetConfigCaches：清理缓存，避免使用过期数据。
  // 命名使用大写驼峰风格是 C++ 常见约定。
  void EnsureConfigLoaded();
  void ResetConfigCaches();
  void LoadConfig(Config* config);
  void UpdateKeepInputProperty(Context* context) const;
  bool HandleClearHistoryShortcut(Segmentation* segmentation,
                                  const std::string& ai_context,
                                  const std::string& segmentation_input,
                                  size_t current_start,
                                  size_t current_end) const;
  bool HandleReplyInput(Segmentation* segmentation,
                        const std::string& segmentation_input) const;
  bool HandlePromptSegment(Segmentation* segmentation,
                           const std::string& segmentation_input) const;
  bool HandleChatTrigger(Segmentation* segmentation,
                         Context* context,
                         const std::string& segmentation_input,
                         bool* should_stop) const;
  // static 成员函数不依赖对象状态，可以理解为工具函数。
  static bool EndsWith(const std::string& value, const std::string& suffix);

  // 以下是成员变量（对象状态）。
  // logger_：负责输出调试信息。命名末尾的下划线是 Google C++ 风格指南常用的约定。
  Logger logger_;
  // config_loaded_ 标记配置是否已经从 schema 读取。
  bool config_loaded_ = false;
  std::string last_schema_id_;

  // enabled_ 控制功能是否对用户开放；keep_input_uncommit_ 管理输入是否自动保留。
  bool enabled_ = false;
  bool keep_input_uncommit_ = false;
  AiAssistantSegmentorBehavior behavior_;

  // 多个哈希表维护触发器、回复内容等映射关系。
  // unordered_map 的 key/value 都是 std::string，使用时要注意 copy 成本，
  // 若数据量大可考虑 std::string_view（但生命周期管理更复杂，是 C++ 常见坑）。
  std::unordered_map<std::string, std::string> chat_triggers_;
  std::unordered_map<std::string, std::string> reply_messages_preedits_;
  std::unordered_map<std::string, std::string> reply_tags_;
  std::unordered_map<std::string, std::string> chat_names_;
  std::unordered_map<std::string, TriggerMetadata> clean_prefix_to_trigger_;
  std::unordered_map<std::string, std::string> reply_inputs_to_trigger_;
  std::unordered_map<std::string, std::string> chat_triggers_reverse_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AI_ASSISTANT_SEGMENTOR_H_
