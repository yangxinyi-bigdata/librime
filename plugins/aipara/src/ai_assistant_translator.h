// 说明：本头文件声明一个自定义 Translator（翻译器/译码器）类，
// 用于在 Rime 输入法框架中，将特定的“分段标签”（segment tags）
// 映射为候选项（candidate），并与外部 AI 服务进行交互。
//
// 新手要点：
// - 继承（class Derived : public Base）是 C++ 实现多态的方式之一，
//   让子类覆写虚函数来改变行为（类似 Python 子类方法覆盖）。
// - Rime 使用了很多智能指针封装（an<T>），用来安全管理对象生命周期。
// - 此类主要职责：根据上下文/标签产出候选；读取 AI 流消息；将结果填回上下文。
#ifndef PLUGINS_AIPARA_SRC_AI_ASSISTANT_TRANSLATOR_H_
#define PLUGINS_AIPARA_SRC_AI_ASSISTANT_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translator.h>

#include <string>
#include <unordered_map>
#include "common/logger.h"

namespace rime {
class Config;
class Context;
class Candidate;
struct Segment;
}  // namespace rime

namespace rime::aipara {

class TcpSocketSync;

class AiAssistantTranslator : public Translator {
 public:
  // 构造函数：从 Rime 传入的 Ticket 中获取与当前引擎/配置关联的信息。
  // 初始化列表（: Translator(ticket), logger_(...)）是 C++ 的惯用写法，
  // 用于直接初始化基类与成员（避免先默认构造再赋值）。
  explicit AiAssistantTranslator(const Ticket& ticket);

  // Query：Translator 的核心接口。输入 input、segment（含标签与位置信息），
  // 返回一个 Translation（候选流）。如果返回 nullptr，表示“我不处理”。
  an<Translation> Query(const string& input,
                        const Segment& segment) override;

  // 从 Rime Config 更新触发词、预编辑文本等配置信息。
  void UpdateCurrentConfig(Config* config);
  // 注入用于与外部服务同步的 TCP 同步器。
  void AttachTcpSocketSync(TcpSocketSync* sync);

 private:
  // 内部数据结构：描述 AI 流的单个数据片段与汇总结果。
  struct AiStreamData;
  struct AiStreamResult;

  // 工具函数：
  // - 把单个候选封装为一个“一次性翻译流”，供 Rime 消费。
  an<Translation> MakeSingleCandidateTranslation(an<Candidate> candidate) const;
  // - 处理带有 "ai_talk" 标签的分段（根据触发词生成一个展示候选）。
  an<Translation> HandleAiTalkSegment(const string& input,
                                      const Segment& segment,
                                      Context* context);
  // - 处理清空历史的分段。
  an<Translation> HandleClearHistorySegment(const Segment& segment);
  // - 处理 AI 回复的分段：轮询 socket，解析 JSON，更新上下文缓存，生成候选。
  an<Translation> HandleAiReplySegment(const string& input,
                                       const Segment& segment,
                                       Context* context);

  // 读取最近一次 AI 流消息，并解析为结构化结果。
  AiStreamResult ReadLatestAiStream();
  // 构造一个 SimpleCandidate：
  // type/start/end 用于在 Rime 中标识候选类型与覆盖范围；text 为实际显示文本；
  // preedit 是“预编辑（灰字提示）”；quality 是候选质量分，数值越大越靠前。
  an<Candidate> MakeCandidate(const std::string& type,
                              size_t start,
                              size_t end,
                              const std::string& text,
                              const std::string& preedit = {},
                              double quality = 1000.0) const;

  // 配置缓存：
  // - chat_triggers_：触发“对话”的前缀（例如“/ai”）。
  // - reply_messages_preedits_：回复候选的预览文本。
  // - chat_names_：聊天场景展示名。
  // - reply_input_to_trigger_：从预编辑文本反查触发器。
  std::unordered_map<std::string, std::string> chat_triggers_;
  std::unordered_map<std::string, std::string> reply_messages_preedits_;
  std::unordered_map<std::string, std::string> chat_names_;
  std::unordered_map<std::string, std::string> reply_input_to_trigger_;

  // 指向 TCP 同步器的裸指针：由外部管理生命周期，此类只“借用”使用。
  // 新手注意：裸指针可能为 nullptr，使用前必须判空；不负责 delete。
  TcpSocketSync* tcp_socket_sync_ = nullptr;
  // 日志器：用于打印调试/信息/错误日志。
  Logger logger_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AI_ASSISTANT_TRANSLATOR_H_
