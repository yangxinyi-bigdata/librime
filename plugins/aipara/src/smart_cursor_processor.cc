#include "smart_cursor_processor.h"

#include <rime/composition.h>
#include <rime/config.h>
#include <rime/config/config_types.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include "common/spans_manager.h"
#include "common/text_formatting.h"
#include "common/tcp_socket_sync.h"

namespace rime::aipara {

namespace {
constexpr std::string_view kSearchMovePrompt = u8" ▶ [搜索模式:] ";
constexpr std::string_view kSearchMovePromptPrefix = u8" ▶ [搜索模式:";

std::string MakeSearchPrompt(const std::string& value) {
  std::string prompt(kSearchMovePromptPrefix);
  prompt.append(value);
  prompt.append("] ");
  return prompt;
}

bool IsAsciiAlpha(const std::string& key_repr) {
  return key_repr.size() == 1 &&
         ((key_repr[0] >= 'a' && key_repr[0] <= 'z') ||
          (key_repr[0] >= 'A' && key_repr[0] <= 'Z'));
}

bool IsAsciiPunctChar(const std::string& key_repr) {
  return key_repr.size() == 1 &&
         std::ispunct(static_cast<unsigned char>(key_repr[0])) != 0;
}
}  // namespace

SmartCursorProcessor::SmartCursorProcessor(const Ticket& ticket)
    : Processor(ticket),
      logger_(MakeLogger("smart_cursor_processor_cpp")) {
  logger_.Clear();
  AIPARA_LOG_INFO(logger_, "SmartCursorProcessor initialized.");

  const std::string punctuation_chars = ",.!?;:()[]<>/_=+*&^%$#@~|-'\"";
  for (char ch : punctuation_chars) {
    punctuation_chars_.insert(ch);
  }

  if (engine_) {
    if (Context* context = engine_->context()) {
      InitializeContextHooks(context);
    }
  }
}

SmartCursorProcessor::~SmartCursorProcessor() {
  DisconnectAll();
}

void SmartCursorProcessor::AttachTcpSocketSync(TcpSocketSync* sync) {
  tcp_socket_sync_ = sync;
  if (engine_ && engine_->context() && tcp_socket_sync_) {
    ApplyGlobalOptions(engine_->context());
  }
}

void SmartCursorProcessor::DisconnectAll() {
  select_connection_.disconnect();
  commit_connection_.disconnect();
  update_connection_.disconnect();
  extended_update_connection_.disconnect();
  property_update_connection_.disconnect();
  unhandled_key_connection_.disconnect();
}

void SmartCursorProcessor::InitializeContextHooks(Context* context) {
  DisconnectAll();
  if (!context) {
    return;
  }

  select_connection_ =
      context->select_notifier().connect([this](Context* ctx) {
        OnSelect(ctx);
      });

  commit_connection_ =
      context->commit_notifier().connect([this](Context* ctx) {
        OnCommit(ctx);
      });

  update_connection_ =
      context->update_notifier().connect([this](Context* ctx) {
        OnUpdate(ctx);
      });

  extended_update_connection_ =
      context->update_notifier().connect([this](Context* ctx) {
        OnExtendedUpdate(ctx);
      });

  property_update_connection_ =
      context->property_update_notifier().connect(
          [this](Context* ctx, const std::string& property) {
            OnPropertyUpdate(ctx, property);
          });

  unhandled_key_connection_ =
      context->unhandled_key_notifier().connect(
          [this](Context* ctx, const KeyEvent& key_event) {
            OnUnhandledKey(ctx, key_event);
          });
}

void SmartCursorProcessor::UpdateCurrentConfig(Config*) {
  previous_is_composing_.reset();
}

ProcessResult SmartCursorProcessor::ProcessKeyEvent(
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

  Config* config = CurrentConfig();
  const std::string key_repr = key_event.repr();

  const std::string current_app = context->get_property("client_app");
  if (!current_app.empty() && config) {
    UpdateAsciiModeFromVimState(current_app, context, config);
  }

  if (!context->IsComposing()) {
    return kNoop;
  }

  Composition* composition = &context->composition();

  if (HandleSearchMode(key_repr, context, config, composition)) {
    return kAccepted;
  }

  const std::string move_next_punct =
      GetConfigString("key_binder/move_next_punct");
  const std::string move_prev_punct =
      GetConfigString("key_binder/move_prev_punct");
  const std::string paste_to_input =
      GetConfigString("key_binder/paste_to_input");
  const std::string search_move_cursor =
      GetConfigString("key_binder/search_move_cursor");

  if (key_repr == "Tab") {
    if (spans_manager::GetSpans(context)) {
      if (MoveBySpans(context, true)) {
        return kAccepted;
      }
    }
    return kNoop;
  }

  if (key_repr == "Left") {
    if (spans_manager::GetSpans(context)) {
      if (MoveBySpans(context, false)) {
        return kAccepted;
      }
    }
    return kNoop;
  }

  if (!move_prev_punct.empty() && key_repr == move_prev_punct) {
    if (MoveToPrevPunctuation(context)) {
      return kAccepted;
    }
  }

  if (!move_next_punct.empty() && key_repr == move_next_punct) {
    if (MoveToNextPunctuation(context)) {
      return kAccepted;
    }
  }

  if (key_repr == "Escape") {
    if (GetConfigBool("translator/keep_input_uncommit", false)) {
      context->set_property("input_string", "");
      context->Clear();
      return kAccepted;
    }
  }

  if (!paste_to_input.empty() && key_repr == paste_to_input) {
    if (tcp_socket_sync_) {
      tcp_socket_sync_->UpdateProperty("command", "get_clipboard");
      SyncWithServer(context, true);
    }
    return kAccepted;
  }

  if (!search_move_cursor.empty() && key_repr == search_move_cursor) {
    Segment* segment = (composition && !composition->empty())
                           ? &composition->back()
                           : nullptr;
    if (!context->get_option("search_move")) {
      context->set_option("search_move", true);
      context->set_property("search_move_str", "");
      if (segment) {
        segment->prompt = std::string(kSearchMovePrompt);
      }
    } else {
      ExitSearchMode(context, segment);
    }
    return kAccepted;
  }

  return kNoop;
}

void SmartCursorProcessor::OnSelect(Context* context) {
  if (!context) {
    return;
  }

  if (context->get_option("search_move")) {
    context->set_option("search_move", false);
    context->set_property("search_move_str", "");
  }

  spans_manager::ClearSpans(context, "选词完成", &logger_);
}

void SmartCursorProcessor::OnCommit(Context* context) {
  if (!context) {
    return;
  }

  context->set_property("input_string", "");
  if (!tcp_socket_sync_) {
    return;
  }

  const std::string send_key = context->get_property("send_key");
  if (!send_key.empty()) {
    tcp_socket_sync_->UpdateProperty("send_key", send_key);
    context->set_property("send_key", "");
  }

  SyncWithServer(context, true);
}

void SmartCursorProcessor::OnUpdate(Context* context) {
  if (!context || context->IsComposing()) {
    return;
  }

  if (context->get_option("search_move")) {
    context->set_option("search_move", false);
    context->set_property("search_move_str", "");
  }

  if (context->get_property("cloud_convert_flag") == "1") {
    context->set_property("cloud_convert_flag", "0");
  }

  if (context->get_property("cloud_convert") == "1") {
    context->set_property("cloud_convert", "0");
  }

  if (context->get_property("rawenglish_prompt") == "1") {
    context->set_property("rawenglish_prompt", "0");
  }

  if (context->get_property("intercept_select_key") == "1") {
    context->set_property("intercept_select_key", "0");
  }

  if (context->get_property("get_cloud_stream") != "idle") {
    context->set_property("get_cloud_stream", "idle");
  }

  const std::string get_ai_stream = context->get_property("get_ai_stream");
  if (get_ai_stream != "idle") {
    const std::string ai_replay_stream =
        context->get_property("ai_replay_stream");
    if (!ai_replay_stream.empty() && ai_replay_stream != u8"等待回复...") {
      context->set_property("get_ai_stream", "idle");
    }
  }
}

void SmartCursorProcessor::OnExtendedUpdate(Context* context) {
  if (!context) {
    return;
  }

  SyncWithServer(context, true);

  const bool current_is_composing = context->IsComposing();
  const bool previous_state = previous_is_composing_.value_or(current_is_composing);

  if (!previous_is_composing_.has_value() ||
      current_is_composing != previous_state) {
    context->set_property("previous_is_composing",
                          current_is_composing ? "true" : "false");
    previous_is_composing_ = current_is_composing;
  }

  if (current_is_composing && !previous_state) {
    const std::string input = context->input();
    const std::string cached_input = context->get_property("input_string");
    const bool keep_input_uncommit =
        GetConfigBool("translator/keep_input_uncommit", false);

    if (keep_input_uncommit && !cached_input.empty() && input.size() == 1) {
      context->set_input(cached_input + input);
      return;
    }

    const std::string keepon_chat_trigger =
        context->get_property("keepon_chat_trigger");
    if (!keepon_chat_trigger.empty()) {
      auto chat_triggers = LoadChatTriggers(CurrentConfig());
      auto it = chat_triggers.find(keepon_chat_trigger);
      if (it != chat_triggers.end() && input.size() == 1) {
        context->set_input(it->second + input);
      }
    }
  }
}

void SmartCursorProcessor::OnPropertyUpdate(Context* context,
                                            const std::string& property) {
  if (!context) {
    return;
  }

  Config* config = CurrentConfig();

  if (property == "client_app") {
    const std::string current_app = context->get_property("client_app");
    if (previous_client_app_.empty() && !current_app.empty()) {
      previous_client_app_ = current_app;
    } else if (!current_app.empty() && current_app != previous_client_app_) {
      previous_client_app_ = current_app;
      ApplyGlobalOptions(context);
      ApplyAppOptions(current_app, context, config);
    }
  } else if (property == "config_update_flag") {
    if (context->get_property("config_update_flag") == "1") {
      ApplyGlobalOptions(context);
      ApplyAppOptions(context->get_property("client_app"), context, config);
      context->set_property("config_update_flag", "0");
    }
  }
}

void SmartCursorProcessor::OnUnhandledKey(Context* context,
                                          const KeyEvent& key_event) {
  if (!context) {
    return;
  }

  AIPARA_LOG_DEBUG(logger_, "unhandled_key_notifier触发： sync_with_server和服务端同步信息");
  SyncWithServer(context, true);

  const std::string key_repr = key_event.repr();
  const auto& send_chars = text_formatting::handle_keys();
  auto it = send_chars.find(key_repr);
  if (it != send_chars.end() && tcp_socket_sync_) {
    tcp_socket_sync_->UpdateProperty("last_unhandled_char", it->second);
  }
}

bool SmartCursorProcessor::HandleSearchMode(const std::string& key_repr,
                                            Context* context,
                                            Config*,
                                            Composition* composition) {
  if (!context->get_option("search_move")) {
    return false;
  }

  Segment* segment = (composition && !composition->empty())
                         ? &composition->back()
                         : nullptr;

  const bool is_valid_char =
      key_repr == "Tab" || IsAsciiAlpha(key_repr) || IsAsciiPunctChar(key_repr);

  std::string search_move_str = context->get_property("search_move_str");

  if (is_valid_char) {
    std::string add_search_move_str;
    if (key_repr == "Tab") {
      add_search_move_str = search_move_str;
    } else {
      add_search_move_str = search_move_str + key_repr;
      context->set_property("search_move_str", add_search_move_str);
    }

    if (segment) {
      segment->prompt = MakeSearchPrompt(add_search_move_str);
    }

    const std::string& input = context->input();
    const Segmentation& segmentation = context->composition();
    const size_t confirmed_pos = segmentation.GetConfirmedPosition();
    const std::string confirmed_input =
        confirmed_pos < input.size() ? input.substr(confirmed_pos) : std::string();
    const size_t caret_pos = context->caret_pos();
    const size_t caret_relative =
        caret_pos > confirmed_pos ? caret_pos - confirmed_pos : 0;

    size_t search_start_pos = 0;
    if (key_repr == "Tab") {
      search_start_pos = caret_relative;
    }

    auto found = text_formatting::FindTextSkipRawEnglishWithWrap(
        confirmed_input, add_search_move_str, search_start_pos, &logger_);
    if (found) {
      const size_t move_pos = confirmed_pos + *found + add_search_move_str.size();
      context->set_caret_pos(move_pos);
    } else if (segment) {
      segment->prompt = MakeSearchPrompt(add_search_move_str);
    }

    return true;
  }

  if (key_repr == "BackSpace") {
    if (!search_move_str.empty()) {
      search_move_str.pop_back();
      context->set_property("search_move_str", search_move_str);
    }
    if (segment) {
      segment->prompt = MakeSearchPrompt(search_move_str);
    }
    return true;
  }

  if (key_repr == "Escape" || key_repr == "Return") {
    ExitSearchMode(context, segment);
    return true;
  }

  return false;
}

void SmartCursorProcessor::ExitSearchMode(Context* context, Segment* segment) {
  if (!context) {
    return;
  }

  context->set_option("search_move", false);
  context->set_property("search_move_str", "");
  if (segment) {
    segment->prompt = std::string(kSearchMovePrompt);
  }
}

bool SmartCursorProcessor::MoveToNextPunctuation(Context* context) {
  if (!context) {
    return false;
  }

  Composition& composition = context->composition();
  if (composition.empty()) {
    return false;
  }

  const std::string& input = context->input();
  const size_t input_length = input.length();
  const size_t current_start = composition.GetCurrentStartPosition();
  size_t caret_pos = context->caret_pos();

  if (caret_pos >= input_length) {
    caret_pos = current_start;
  }

  for (size_t i = caret_pos; i < input_length; ++i) {
    if (punctuation_chars_.count(input[i])) {
      context->set_caret_pos(i);
      return true;
    }
  }

  context->set_caret_pos(input_length);
  return true;
}

bool SmartCursorProcessor::MoveToPrevPunctuation(Context* context) {
  if (!context) {
    return false;
  }

  Composition& composition = context->composition();
  if (composition.empty()) {
    return false;
  }

  const std::string& input = context->input();
  if (input.empty()) {
    return false;
  }

  const size_t current_start = composition.GetCurrentStartPosition();
  size_t caret_pos = context->caret_pos();

  if (caret_pos <= current_start) {
    context->set_caret_pos(input.length());
    return true;
  }

  for (size_t i = caret_pos; i > current_start; --i) {
    const size_t index = i - 1;
    if (punctuation_chars_.count(input[index])) {
      context->set_caret_pos(index);
      return true;
    }
  }

  context->set_caret_pos(current_start);
  return true;
}

bool SmartCursorProcessor::MoveBySpans(Context* context, bool move_next) {
  if (!context) {
    return false;
  }
  const size_t caret = context->caret_pos();
  std::optional<std::size_t> target =
      move_next ? spans_manager::GetNextCursorPosition(context, caret)
                : spans_manager::GetPrevCursorPosition(context, caret);
  if (!target) {
    return false;
  }
  context->set_caret_pos(*target);
  return true;
}

void SmartCursorProcessor::ApplyGlobalOptions(Context* context) {
  if (!tcp_socket_sync_ || !context) {
    return;
  }
  const int applied = tcp_socket_sync_->ApplyGlobalOptionsToContext(context);
  if (applied > 0) {
    AIPARA_LOG_INFO(
        logger_, "应用全局开关数量: " + std::to_string(applied));
  }
}

void SmartCursorProcessor::ApplyAppOptions(const std::string& current_app,
                                           Context* context,
                                           Config* config) {
  if (!context || !config || current_app.empty()) {
    return;
  }

  an<ConfigMap> app_options = config->GetMap("app_options");
  if (!app_options) {
    return;
  }

  const std::string sanitized = SanitizeAppKey(current_app);
  if (!app_options->HasKey(sanitized)) {
    return;
  }

  const std::string base_path = "app_options/" + sanitized;
  if (auto entry = app_options->Get(sanitized)) {
    if (auto map = As<ConfigMap>(entry)) {
      for (auto it = map->begin(); it != map->end(); ++it) {
        const std::string& key = it->first;
        if (key == "__label__") {
          continue;
        }
        bool value = false;
        if (config->GetBool(base_path + "/" + key, &value) &&
            context->get_option(key) != value) {
          context->set_option(key, value);
          AIPARA_LOG_DEBUG(
              logger_, "set_option " + key + " = " + (value ? "true" : "false"));
        }
      }
    }
  }
}

void SmartCursorProcessor::UpdateAsciiModeFromVimState(
    const std::string& app_key,
    Context* context,
    Config* config) {
  if (!context || !config || app_key.empty()) {
    return;
  }

  const std::string sanitized = SanitizeAppKey(app_key);
  bool vim_mode_enabled = false;
  if (!config->GetBool("app_options/" + sanitized + "/vim_mode",
                       &vim_mode_enabled) ||
      !vim_mode_enabled) {
    return;
  }

  const auto& user_dir = Service::instance().deployer().user_data_dir;
  if (user_dir.empty()) {
    return;
  }
  std::filesystem::path path(user_dir.string());
  path /= "log";
  path /= "." + sanitized + "_vim_mode";

  std::ifstream stream(path);
  if (!stream.is_open()) {
    return;
  }

  std::string current_mode;
  std::getline(stream, current_mode);
  stream.close();

  const std::string previous_mode = app_vim_mode_state_[sanitized];
  if (previous_mode == current_mode) {
    return;
  }

  app_vim_mode_state_[sanitized] = current_mode;

  if (current_mode == "normal_mode") {
    if (!context->get_option("ascii_mode")) {
      context->set_option("ascii_mode", true);
    }
  } else if (current_mode == "insert_mode") {
    if (context->get_option("ascii_mode")) {
      context->set_option("ascii_mode", false);
    }
  }
}

std::string SmartCursorProcessor::SanitizeAppKey(
    const std::string& app_name) const {
  std::string sanitized = app_name;
  std::replace(sanitized.begin(), sanitized.end(), '.', '_');
  return sanitized;
}

Config* SmartCursorProcessor::CurrentConfig() const {
  if (!engine_) {
    return nullptr;
  }
  if (auto* schema = engine_->schema()) {
    return schema->config();
  }
  return nullptr;
}

std::string SmartCursorProcessor::GetConfigString(
    const std::string& path,
    const std::string& fallback) const {
  Config* config = CurrentConfig();
  if (!config) {
    return fallback;
  }
  std::string value;
  if (config->GetString(path, &value)) {
    return value;
  }
  return fallback;
}

bool SmartCursorProcessor::GetConfigBool(const std::string& path,
                                         bool fallback) const {
  Config* config = CurrentConfig();
  if (!config) {
    return fallback;
  }
  bool value = fallback;
  if (config->GetBool(path, &value)) {
    return value;
  }
  return fallback;
}

std::unordered_map<std::string, std::string>
SmartCursorProcessor::LoadChatTriggers(Config* config) const {
  std::unordered_map<std::string, std::string> triggers;
  if (!config) {
    return triggers;
  }
  if (auto prompts = config->GetMap("ai_assistant/ai_prompts")) {
    for (auto it = prompts->begin(); it != prompts->end(); ++it) {
      const std::string& trigger_name = it->first;
      const std::string base_path =
          "ai_assistant/ai_prompts/" + trigger_name + "/chat_triggers";
      std::string value;
      if (config->GetString(base_path, &value) && !value.empty()) {
        triggers.emplace(trigger_name, value);
      }
    }
  }
  return triggers;
}

void SmartCursorProcessor::SyncWithServer(Context* context,
                                          bool include_config) const {
  if (!tcp_socket_sync_) {
    return;
  }
  if (include_config) {
    tcp_socket_sync_->SyncWithServer(context, CurrentConfig());
  } else if (context) {
    tcp_socket_sync_->SyncWithServer(context);
  } else {
    tcp_socket_sync_->SyncWithServer();
  }
}

}  // namespace rime::aipara
