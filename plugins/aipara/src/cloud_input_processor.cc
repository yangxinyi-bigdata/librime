#include "cloud_input_processor.h"

#include <rime/config.h>
#include <rime/context.h>
#include <rime/key_event.h>
#include <rime/schema.h>

#include "common/tcp_zmq.h"

namespace rime::aipara {

CloudInputProcessor::CloudInputProcessor(const Ticket& ticket)
    : Processor(ticket) {}

CloudInputProcessor::~CloudInputProcessor() = default;

ProcessResult CloudInputProcessor::ProcessKeyEvent(
    const KeyEvent&) {
  // TODO: port cloud input handling logic.
  return kNoop;
}

void CloudInputProcessor::UpdateCurrentConfig(Config* config) {
  if (!config) {
    return;
  }

  std::string value;
  if (config->GetString("speller/delimiter", &value)) {
    delimiter_ = value;
  } else {
    delimiter_ = " ";
  }

  if (config->GetString("translator/cloud_convert_symbol", &value)) {
    cloud_convert_symbol_ = value;
  } else {
    cloud_convert_symbol_ = "Return";
  }

  if (config->GetString("translator/english_mode_symbol", &value)) {
    english_mode_symbol_ = value;
  } else {
    english_mode_symbol_ = "`";
  }

  if (config->GetString("translator/rawenglish_delimiter_after", &value)) {
    rawenglish_delimiter_after_ = value;
  } else {
    rawenglish_delimiter_after_ = "`";
  }

  if (config->GetString("translator/rawenglish_delimiter_before", &value)) {
    rawenglish_delimiter_before_ = value;
  } else {
    rawenglish_delimiter_before_ = "`";
  }

  RefreshAiPrompts(config);
  config_initialized_ = true;
}

void CloudInputProcessor::UpdateProperty(const std::string&,
                                         const std::string&) {
  // TODO: update context properties received from tcp_zmq.
}

void CloudInputProcessor::AttachTcpZmq(TcpZmq* client) {
  tcp_zmq_ = client;
}

void CloudInputProcessor::EnsureConfigLoaded(const Schema* schema) {
  if (config_initialized_ || schema == nullptr) {
    return;
  }
  if (auto* config = schema->config()) {
    UpdateCurrentConfig(config);
  }
}

void CloudInputProcessor::RefreshAiPrompts(Config*) {
  // TODO: hydrate ai_assistant_config_ based on configuration.
}

}  // namespace rime::aipara
