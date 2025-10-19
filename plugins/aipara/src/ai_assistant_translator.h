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
  explicit AiAssistantTranslator(const Ticket& ticket);

  an<Translation> Query(const string& input,
                        const Segment& segment) override;

  void UpdateCurrentConfig(Config* config);
  void AttachTcpSocketSync(TcpSocketSync* sync);

 private:
  struct AiStreamData;
  struct AiStreamResult;

  an<Translation> MakeSingleCandidateTranslation(an<Candidate> candidate) const;
  an<Translation> HandleAiTalkSegment(const string& input,
                                      const Segment& segment,
                                      Context* context);
  an<Translation> HandleClearHistorySegment(const Segment& segment);
  an<Translation> HandleAiReplySegment(const string& input,
                                       const Segment& segment,
                                       Context* context);

  AiStreamResult ReadLatestAiStream();
  an<Candidate> MakeCandidate(const std::string& type,
                              size_t start,
                              size_t end,
                              const std::string& text,
                              const std::string& preedit = {},
                              double quality = 1000.0) const;

  std::unordered_map<std::string, std::string> chat_triggers_;
  std::unordered_map<std::string, std::string> reply_messages_preedits_;
  std::unordered_map<std::string, std::string> chat_names_;
  std::unordered_map<std::string, std::string> reply_input_to_trigger_;

  TcpSocketSync* tcp_socket_sync_ = nullptr;
  Logger logger_;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AI_ASSISTANT_TRANSLATOR_H_
