#include <rime/common.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/processor.h>

namespace rime {

// struct Log;

class KeyLogger : public Processor {
 public:
  explicit KeyLogger(const Ticket& ticket)
    : Processor(ticket) {
    Context* context = engine_->context();
    update_connection_ = context->update_notifier()
      .connect([this](Context* ctx) { OnUpdate(ctx); });
  }

  virtual ~KeyLogger() {
    update_connection_.disconnect();
  }

  // void StartLogging();
  // void Endlogging();

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 private:
  void OnUpdate(Context* ctx) {}

  connection update_connection_;

  // vector<Log> logs_;
  // bool is_logging_;
};
}  // namespace rime