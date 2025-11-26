#include "aux_code_filter_v3.h"

#include <utf8.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/translation.h>
#include <rime_api.h>

namespace rime::aipara {
namespace {

constexpr std::string_view kLoggerName = "aux_code_filter_v3";
constexpr std::string_view kPunctuationChars =
    ",.!?;:()[]<>/_=+*&^%$#@~|-\'\"`";

std::string TrimTrailingSpaces(std::string text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  return text;
}

std::string RemoveLastToken(const std::string& text) {
  std::string trimmed = TrimTrailingSpaces(text);
  auto pos = trimmed.find_last_of(" \t");
  if (pos == std::string::npos) {
    return std::string();
  }
  return trimmed.substr(0, pos);
}

bool ContainsAny(const std::string& text, std::string_view chars) {
  return text.find_first_of(chars) != std::string::npos;
}

std::string RemoveCharacters(const std::string& text, std::string_view chars) {
  std::string result;
  result.reserve(text.size());
  for (char ch : text) {
    if (chars.find(ch) == std::string::npos) {
      result.push_back(ch);
    }
  }
  return result;
}

std::string Utf8First(const std::string& text) {
  if (text.empty()) {
    return std::string();
  }
  auto it = text.begin();
  auto next = it;
  utf8::next(next, text.end());
  return std::string(it, next);
}

std::string Utf8Last(const std::string& text) {
  if (text.empty()) {
    return std::string();
  }
  auto it = text.end();
  auto prev = it;
  utf8::prior(prev, text.begin());
  return std::string(prev, it);
}

std::string Utf8RemoveLast(const std::string& text) {
  if (text.empty()) {
    return std::string();
  }
  auto it = text.end();
  auto prev = it;
  utf8::prior(prev, text.begin());
  return std::string(text.begin(), prev);
}

class AuxRewrittenCandidate : public ShadowCandidate {
 public:
  AuxRewrittenCandidate(const an<Candidate>& original,
                        std::optional<std::string> new_text,
                        std::optional<std::string> new_preedit,
                        size_t new_end)
      : ShadowCandidate(original,
                        original->type(),
                        new_text ? *new_text : std::string(),
                        std::string()),
        has_custom_preedit_(new_preedit.has_value()),
        custom_preedit_(new_preedit.value_or(std::string())) {
    set_end(new_end);
    set_quality(original->quality());
  }

  string preedit() const override {
    if (has_custom_preedit_) {
      return custom_preedit_;
    }
    return ShadowCandidate::preedit();
  }

 private:
  bool has_custom_preedit_;
  std::string custom_preedit_;
};

std::filesystem::path ResolveAuxCodePath(const std::string& basename) {
  const auto* api = rime_get_api();
  if (!api || !api->get_user_data_dir) {
    return std::filesystem::path();
  }
  const std::string user_dir = api->get_user_data_dir();
  auto base = std::filesystem::path(user_dir) / "lua" / "aux_code";
  return base / (basename + ".txt");
}

}  // namespace

AuxCodeFilterV3::AuxCodeFilterV3(const Ticket& ticket)
    : Filter(ticket), logger_(MakeLogger(std::string(kLoggerName))) {
  logger_.Clear();
  if (engine_) {
    AttachContextHooks(engine_->context());
  }
}

AuxCodeFilterV3::~AuxCodeFilterV3() {
  DetachContextHooks();
}

void AuxCodeFilterV3::AttachContextHooks(Context* context) {
  DetachContextHooks();
  if (!context) {
    return;
  }
  select_connection_ =
      context->select_notifier().connect([this](Context* ctx) {
        OnSelect(ctx);
      });
}

void AuxCodeFilterV3::DetachContextHooks() {
  if (select_connection_.connected()) {
    select_connection_.disconnect();
  }
}

void AuxCodeFilterV3::OnSelect(Context* context) {
  if (!context) {
    return;
  }

  const std::string punctuation_chars(kPunctuationChars);

  const std::string& input = context->input();
  Composition& composition = context->composition();
  size_t current_start = composition.GetCurrentStartPosition();
  size_t current_end = composition.GetCurrentEndPosition();
  if (current_end > input.size()) {
    current_end = input.size();
  }

  std::string segment_input;
  if (current_end > current_start) {
    segment_input = input.substr(current_start, current_end - current_start);
  }

  if (segment_input.size() == 1 &&
      punctuation_chars.find(segment_input[0]) != std::string::npos) {
    context->ConfirmCurrentSelection();
    return;
  }

  if (!set_fuzhuma_) {
    return;
  }

  if (!input.empty()) {
    context->PopInput(1);
    set_fuzhuma_ = false;
  }

  const std::string& new_input = context->input();
  Composition& new_composition = context->composition();
  size_t confirmed_position = new_composition.GetConfirmedPosition();
  if (new_input.size() <= confirmed_position) {
    context->Commit();
  }
}

bool AuxCodeFilterV3::EnsureAuxTables(const std::string& txt_name) {
  if (txt_name.empty()) {
    AIPARA_LOG_ERROR(logger_, "Auxiliary code file is not configured.");
    aux_hanzi_code_.clear();
    aux_code_hanzi_.clear();
    cached_aux_code_file_.clear();
    return false;
  }

  if (txt_name == cached_aux_code_file_ && !aux_hanzi_code_.empty()) {
    return true;
  }

  std::filesystem::path target = ResolveAuxCodePath(txt_name);

  if (target.empty()) {
    AIPARA_LOG_ERROR(
        logger_, "Unable to determine auxiliary code directory path.");
    aux_hanzi_code_.clear();
    aux_code_hanzi_.clear();
    cached_aux_code_file_.clear();
    return false;
  }

  if (!std::filesystem::exists(target)) {
    AIPARA_LOG_ERROR(
        logger_, "Auxiliary code file not found: " + target.string());
    aux_hanzi_code_.clear();
    aux_code_hanzi_.clear();
    cached_aux_code_file_.clear();
    return false;
  }

  std::ifstream file(target);
  if (!file.is_open()) {
    AIPARA_LOG_ERROR(logger_,
                     "Unable to open auxiliary code file: " + target.string());
    aux_hanzi_code_.clear();
    aux_code_hanzi_.clear();
    cached_aux_code_file_.clear();
    return false;
  }

  std::unordered_map<std::string, std::vector<std::string>> aux_hanzi_code;
  std::unordered_map<std::string, std::vector<std::string>> aux_code_hanzi;

  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    if (key.empty() || value.empty()) {
      continue;
    }
    aux_hanzi_code[key].push_back(value);
    aux_code_hanzi[value].push_back(key);
  }

  aux_hanzi_code_ = std::move(aux_hanzi_code);
  aux_code_hanzi_ = std::move(aux_code_hanzi);
  cached_aux_code_file_ = txt_name;
  return true;
}

int AuxCodeFilterV3::MatchAuxiliaryCode(const std::string& character,
                                        const std::string& match_code) const {
  auto it = aux_hanzi_code_.find(character);
  if (it == aux_hanzi_code_.end()) {
    return 0;
  }
  for (const auto& code : it->second) {
    if (code.size() >= 3) {
      if (!match_code.empty() && code[2] == match_code[0]) {
        return 1;
      }
    } else {
      return 2;
    }
  }
  return 0;
}

an<Translation> AuxCodeFilterV3::Apply(an<Translation> translation,
                                       CandidateList* /*candidates*/) {
  if (!translation || !engine_) {
    return translation;
  }

  Context* context = engine_->context();
  if (!context) {
    return translation;
  }
  Schema* schema = engine_->schema();
  Config* config = schema ? schema->config() : nullptr;
  if (!config) {
    return translation;
  }

  bool single_fuzhu = false;
  config->GetBool("aux_code/single_fuzhu", &single_fuzhu);
  if (!single_fuzhu) {
    set_fuzhuma_ = false;
    return translation;
  }

  const std::string& input = context->input();
  if (input.size() <= 2) {
    set_fuzhuma_ = false;
    return translation;
  }

  if (context->get_property("rawenglish_prompt") == "1") {
    set_fuzhuma_ = false;
    return translation;
  }

  std::string fuzhu_mode;
  config->GetString("aux_code/fuzhu_mode", &fuzhu_mode);

  std::string shuangpin_txt;
  config->GetString("aux_code/shuangpin_zrm_txt", &shuangpin_txt);

  std::string english_mode_symbol = "`";
  config->GetString("translator/english_mode_symbol", &english_mode_symbol);

  Composition& composition = context->composition();
  size_t current_start = composition.GetCurrentStartPosition();
  size_t current_end = composition.GetCurrentEndPosition();
  if (current_end > input.size()) {
    current_end = input.size();
  }

  std::string segment_input;
  if (current_end > current_start) {
    segment_input = input.substr(current_start, current_end - current_start);
  }

  if (current_start == 0 && !composition.empty()) {
    const Segment& first_segment = composition.front();
    if (first_segment.HasTag("ai_talk")) {
      size_t ai_len = first_segment.length;
      if (ai_len >= segment_input.size()) {
        set_fuzhuma_ = false;
        return translation;
      }
      segment_input.erase(0, std::min(ai_len, segment_input.size()));
    } else if (first_segment.HasTag("ai_reply")) {
      set_fuzhuma_ = false;
      return translation;
    }
  }

  if (segment_input.size() <= 1) {
    return translation;
  }

  if (!english_mode_symbol.empty() &&
      segment_input.find(english_mode_symbol) != std::string::npos) {
    if (english_mode_symbol.size() == 1 && segment_input.size() >= 2 &&
        segment_input[segment_input.size() - 2] == english_mode_symbol[0]) {
      set_fuzhuma_ = false;
      return translation;
    }
    const size_t symbol_len = english_mode_symbol.size();
    size_t search_start = 0;
    while (true) {
      size_t start = segment_input.find(english_mode_symbol, search_start);
      if (start == std::string::npos) {
        break;
      }
      size_t end = segment_input.find(english_mode_symbol, start + symbol_len);
      if (end == std::string::npos) {
        break;
      }
      segment_input.erase(start, (end + symbol_len) - start);
      search_start = start;
    }
    size_t last_symbol = segment_input.rfind(english_mode_symbol);
    if (last_symbol != std::string::npos) {
      segment_input.erase(last_symbol);
    }
  }

  bool last_three_has_punctuation = false;
  if (ContainsAny(segment_input, kPunctuationChars)) {
    std::string tail = segment_input.size() <= 3
                           ? segment_input
                           : segment_input.substr(segment_input.size() - 3);
    last_three_has_punctuation = ContainsAny(tail, kPunctuationChars);
    segment_input = RemoveCharacters(segment_input, kPunctuationChars);
  }

  if (segment_input.size() % 2 == 0 || segment_input.size() == 1) {
    set_fuzhuma_ = false;
    return translation;
  }

  if (fuzhu_mode == "single") {
    return translation;
  }

  if (!EnsureAuxTables(shuangpin_txt)) {
    set_fuzhuma_ = false;
    return translation;
  }

  if (segment_input.empty()) {
    set_fuzhuma_ = false;
    return translation;
  }

  std::string last_three_code =
      segment_input.size() >= 3 ? segment_input.substr(segment_input.size() - 3)
                                : segment_input;
  std::string last_code = Utf8Last(segment_input);

  if (fuzhu_mode == "all") {
    if (last_three_has_punctuation) {
      set_fuzhuma_ = false;
      return translation;
    }
    set_fuzhuma_ = true;
    return HandleAllMode(segment_input, last_code, last_three_code,
                         current_end, translation);
  }

  if (fuzhu_mode == "before") {
    set_fuzhuma_ = true;
    return HandleBeforeMode(last_code, translation);
  }

  if (fuzhu_mode == "after") {
    set_fuzhuma_ = true;
    return HandleAfterMode(last_code, translation);
  }

  set_fuzhuma_ = false;
  return translation;
}

an<Translation> AuxCodeFilterV3::HandleAllMode(
    const std::string& segment_input,
    const std::string& last_code,
    const std::string& last_three_code,
    size_t current_end,
    an<Translation> translation) {
  CandidateList direct_output;
  std::map<size_t, CandidateList> matched_by_position;
  CandidateList insert_last;
  bool last_replace_done = false;
  std::optional<std::string> base_preedit;

  while (!translation->exhausted()) {
    an<Candidate> cand = translation->Peek();
    translation->Next();

    size_t cand_end = cand->end();
    long left_position =
        static_cast<long>(current_end) - static_cast<long>(cand_end);

    if (left_position == 0) {
      if (!base_preedit.has_value()) {
        std::string preedit = cand->preedit();
        std::string trimmed = RemoveLastToken(preedit);
        if (!trimmed.empty()) {
          base_preedit = trimmed;
        }
      }
      continue;
    }

    if (left_position == 1) {
      if (last_replace_done) {
        insert_last.push_back(cand);
        continue;
      }
      last_replace_done = true;

      auto it = aux_code_hanzi_.find(last_three_code);
      if (it != aux_code_hanzi_.end() && !it->second.empty()) {
        std::string prefix = base_preedit.has_value()
                                 ? *base_preedit
                                 : cand->preedit();
        std::string text_without_last = Utf8RemoveLast(cand->text());
        for (const auto& chinese_char : it->second) {
          std::string new_text = text_without_last + chinese_char;
          auto rewritten = New<AuxRewrittenCandidate>(
              cand, new_text, prefix, cand->end());
          direct_output.push_back(rewritten);
        }
      } else {
        std::string prefix = base_preedit.has_value()
                                 ? *base_preedit
                                 : cand->preedit();
        if (!base_preedit.has_value()) {
          base_preedit = prefix;
        }
        auto rewritten = New<AuxRewrittenCandidate>(
            cand, std::nullopt, prefix, cand->end());
        direct_output.push_back(rewritten);
      }
      continue;
    }

    size_t matched_position = 0;
    size_t char_index = 0;
    for (auto it = cand->text().begin(); it != cand->text().end();) {
      auto next = it;
      utf8::next(next, cand->text().end());
      std::string character(it, next);
      ++char_index;
      if (MatchAuxiliaryCode(character, last_code) == 1) {
        matched_position = char_index;
        break;
      }
      it = next;
    }

    if (matched_position == 0) {
      insert_last.push_back(cand);
    } else {
      matched_by_position[matched_position].push_back(cand);
    }
  }

  if (direct_output.empty() && matched_by_position.empty() &&
      insert_last.empty()) {
    return translation;
  }

  auto fifo = New<FifoTranslation>();
  for (auto& cand : direct_output) {
    fifo->Append(cand);
  }
  for (auto& [pos, list] : matched_by_position) {
    for (auto& cand : list) {
      fifo->Append(cand);
    }
  }
  for (auto& cand : insert_last) {
    fifo->Append(cand);
  }
  return fifo;
}

an<Translation> AuxCodeFilterV3::HandleBeforeMode(
    const std::string& last_code,
    an<Translation> translation) {
  CandidateList head;
  CandidateList tail;
  size_t index = 0;

  while (!translation->exhausted()) {
    an<Candidate> cand = translation->Peek();
    translation->Next();
    ++index;

    if (index == 1) {
      head.push_back(cand);
      continue;
    }

    std::string first_char = Utf8First(cand->text());
    if (MatchAuxiliaryCode(first_char, last_code) > 0) {
      std::string new_preedit = cand->preedit();
      new_preedit.append(last_code);
      auto rewritten = New<AuxRewrittenCandidate>(
          cand, std::nullopt, new_preedit, cand->end() + 1);
      head.push_back(rewritten);
    } else {
      tail.push_back(cand);
    }
  }

  if (head.empty() && tail.empty()) {
    return translation;
  }

  auto fifo = New<FifoTranslation>();
  for (auto& cand : head) {
    fifo->Append(cand);
  }
  for (auto& cand : tail) {
    fifo->Append(cand);
  }
  return fifo;
}

an<Translation> AuxCodeFilterV3::HandleAfterMode(
    const std::string& last_code,
    an<Translation> translation) {
  CandidateList head;
  CandidateList tail;
  size_t index = 0;

  while (!translation->exhausted()) {
    an<Candidate> cand = translation->Peek();
    translation->Next();
    ++index;

    if (index == 1) {
      head.push_back(cand);
      continue;
    }

    std::string last_char = Utf8Last(cand->text());
    if (MatchAuxiliaryCode(last_char, last_code) > 0) {
      head.push_back(cand);
    } else {
      tail.push_back(cand);
    }
  }

  if (head.empty() && tail.empty()) {
    return translation;
  }

  auto fifo = New<FifoTranslation>();
  for (auto& cand : head) {
    fifo->Append(cand);
  }
  for (auto& cand : tail) {
    fifo->Append(cand);
  }
  return fifo;
}

}  // namespace rime::aipara
