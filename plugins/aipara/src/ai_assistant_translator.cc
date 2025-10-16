#include "ai_assistant_translator.h"

#include <rime/config.h>
#include <rime/translation.h>

#include "tcp_socket_sync.h"

namespace rime::aipara {

AiAssistantTranslator::AiAssistantTranslator(const Ticket& ticket)
    : Translator(ticket) {}

an<Translation> AiAssistantTranslator::Query(const string&,
                                             const Segment&) {
  // TODO: emit AI assistant candidates.
  return nullptr;
}

void AiAssistantTranslator::UpdateCurrentConfig(Config* config) {
  chat_triggers_.clear();
  reply_messages_preedits_.clear();
  chat_names_.clear();
  reply_input_to_trigger_.clear();

  if (!config) {
    return;
  }

  // TODO: populate lookup tables from config.
}

void AiAssistantTranslator::AttachTcpSocketSync(TcpSocketSync* sync) {
  tcp_socket_sync_ = sync;
}

}  // namespace rime::aipara
