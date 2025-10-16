#ifndef PLUGINS_AIPARA_SRC_AI_ASSISTANT_TRANSLATOR_H_
#define PLUGINS_AIPARA_SRC_AI_ASSISTANT_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translator.h>

#include <string>
#include <unordered_map>

namespace rime {
class Config;
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
  std::unordered_map<std::string, std::string> chat_triggers_;
  std::unordered_map<std::string, std::string> reply_messages_preedits_;
  std::unordered_map<std::string, std::string> chat_names_;
  std::unordered_map<std::string, std::string> reply_input_to_trigger_;

  TcpSocketSync* tcp_socket_sync_ = nullptr;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_AI_ASSISTANT_TRANSLATOR_H_
