// 说明：本头文件声明了 SmartCursorProcessor，它继承自 Rime 的 Processor，
// 负责处理按键事件、响应上下文变化、与外部服务同步、并提供“智能光标”操作
//（按分片跳转、按标点跳转、搜索移动模式等）。
//
// 新手要点：
// - Processor::ProcessKeyEvent 返回枚举 ProcessResult，告诉 Rime 事件是否已被处理。
// - 类里大量使用指针（Context*、Config*）：可能为空，要判空。
// - connection 是 Rime 的连接器类型，用于挂接上下文的各种 Notifier（信号/槽）。
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

class TcpZmq;

class SmartCursorProcessor : public Processor {
 public:
  explicit SmartCursorProcessor(const Ticket& ticket);
  ~SmartCursorProcessor() override;

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

  void UpdateCurrentConfig(Config* config);

 private:
  // 将本类的回调函数挂接到给定 Context 的各类事件（选中/提交/更新/属性更新/未处理键）。
  void InitializeContextHooks(Context* context);
  // 断开所有已挂接的连接，防止悬空回调。
  void DisconnectAll();

  // 上下文事件回调：
  void OnSelect(Context* context);
  void OnCommit(Context* context);
  void OnUpdate(Context* context);
  void OnExtendedUpdate(Context* context);
  void OnPropertyUpdate(Context* context, const std::string& property);
  void OnUnhandledKey(Context* context, const KeyEvent& key_event);

  // “搜索移动模式”处理：根据用户键入的字符逐步拼接搜索串并跳转光标。
  bool HandleSearchMode(const std::string& key_repr,
                        Context* context,
                        Config* config,
                        Composition* composition);
  void ExitSearchMode(Context* context, Segment* segment);

  // 基于标点或分片的移动：
  bool MoveToNextPunctuation(Context* context);
  bool MoveToPrevPunctuation(Context* context);
  bool MoveBySpans(Context* context, bool move_next);

  // 同步与设置：
  void ApplyGlobalOptions(Context* context);
  void ApplyAppOptions(const std::string& current_app,
                       Context* context,
                       Config* config);
  void UpdateAsciiModeFromVimState(const std::string& app_key,
                                   Context* context,
                                   Config* config);

  // 杂项工具：
  std::string SanitizeAppKey(const std::string& app_name) const;
  Config* CurrentConfig() const;
  std::string GetConfigString(const std::string& path,
                              const std::string& fallback = std::string())
      const;
  bool GetConfigBool(const std::string& path, bool fallback) const;
  std::unordered_map<std::string, std::string> LoadChatTriggers(
      Config* config) const;

  // 将上下文/配置同步到外部服务端（通过 TcpZmq）。
  void SyncWithServer(Context* context, bool include_config = false) const;

  // 成员字段：
  Logger logger_;
  TcpZmq* tcp_zmq_ = nullptr;

  // 快速判定是否为标点字符的集合（ASCII）。
  std::unordered_set<char> punctuation_chars_;
  // 记录每个 app 的 Vim 模式（normal/insert），用于切换 ascii_mode。
  std::unordered_map<std::string, std::string> app_vim_mode_state_;
  // 上次是否处于 composing 状态（候选正在生成中）。
  std::optional<bool> previous_is_composing_;
  // 记录最近一次的客户端 app 名，便于在切换 app 时应用 app_options。
  std::string previous_client_app_;

  // Rime 的“连接器”，用于自动管理回调连接的生命周期。
  connection select_connection_;
  connection commit_connection_;
  connection update_connection_;
  connection extended_update_connection_;
  connection property_update_connection_;

};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_SMART_CURSOR_PROCESSOR_H_