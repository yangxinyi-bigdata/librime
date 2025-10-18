#include "smart_cursor_processor.h"

#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>

namespace rime::aipara {

SmartCursorProcessor::SmartCursorProcessor(const Ticket& ticket)
    : Processor(ticket) {
  if (engine_) {
    if (auto* context = engine_->context()) {
      update_connection_ =
          context->update_notifier().connect([this](Context* ctx) {
            OnUpdate(ctx);
          });
    }
  }
}

SmartCursorProcessor::~SmartCursorProcessor() {
  update_connection_.disconnect();
}

ProcessResult SmartCursorProcessor::ProcessKeyEvent(
    const KeyEvent&) {
  // TODO: implement smart cursor movement logic.
  return kNoop;
}

void SmartCursorProcessor::UpdateCurrentConfig(Config*) {
  // TODO: cache configuration values.
  config_initialized_ = true;
}

void SmartCursorProcessor::OnUpdate(Context*) {
  // TODO: handle context updates.
}

}  // namespace rime::aipara
