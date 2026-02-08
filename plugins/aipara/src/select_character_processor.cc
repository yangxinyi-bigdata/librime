#include "select_character_processor.h"

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <utf8.h>

namespace rime::aipara {
namespace {

constexpr const char* kSelectFirstKeyPath =
    "key_binder/select_first_character";
constexpr const char* kSelectLastKeyPath =
    "key_binder/select_last_character";

bool GetUtf8First(const std::string& text, std::string* out) {
  if (!out || text.empty()) {
    return false;
  }
  auto it = text.begin();
  auto next = it;
  utf8::next(next, text.end());
  out->assign(it, next);
  return !out->empty();
}

bool GetUtf8Last(const std::string& text, std::string* out) {
  if (!out || text.empty()) {
    return false;
  }
  auto it = text.end();
  auto prev = it;
  utf8::prior(prev, text.begin());
  out->assign(prev, it);
  return !out->empty();
}

}  // namespace

SelectCharacterProcessor::SelectCharacterProcessor(const Ticket& ticket)
    : Processor(ticket) {
  if (engine_ && engine_->schema()) {
    LoadKeyBindings(engine_->schema()->config());
  }
}

void SelectCharacterProcessor::LoadKeyBindings(rime::Config* config) {
  first_key_.clear();
  last_key_.clear();
  if (!config) {
    return;
  }
  config->GetString(kSelectFirstKeyPath, &first_key_);
  config->GetString(kSelectLastKeyPath, &last_key_);
}

ProcessResult SelectCharacterProcessor::ProcessKeyEvent(
    const KeyEvent& key_event) {
  if (key_event.release()) {
    return kNoop;
  }
  if (!engine_) {
    return kNoop;
  }
  Context* context = engine_->context();
  if (!context) {
    return kNoop;
  }
  if (!context->IsComposing() && !context->HasMenu()) {
    return kNoop;
  }

  if (engine_->schema()) {
    LoadKeyBindings(engine_->schema()->config());
  }

  if (first_key_.empty() && last_key_.empty()) {
    return kNoop;
  }

  const std::string key_repr = key_event.repr();
  const bool match_first = !first_key_.empty() && key_repr == first_key_;
  const bool match_last = !last_key_.empty() && key_repr == last_key_;
  if (!match_first && !match_last) {
    return kNoop;
  }

  std::string text;
  if (auto cand = context->GetSelectedCandidate()) {
    text = cand->text();
  } else {
    text = context->input();
  }

  if (text.empty()) {
    return kNoop;
  }

  if (utf8::distance(text.begin(), text.end()) <= 1) {
    return kNoop;
  }

  std::string commit_text;
  if (match_first) {
    if (!GetUtf8First(text, &commit_text)) {
      return kNoop;
    }
  } else {
    if (!GetUtf8Last(text, &commit_text)) {
      return kNoop;
    }
  }

  engine_->CommitText(commit_text);
  context->Clear();
  return kAccepted;
}

}  // namespace rime::aipara
