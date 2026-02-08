#include "autocap_filter.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/translation.h>

#include <cctype>
#include <cstddef>
#include <string>

namespace rime::aipara {
namespace {

bool IsAsciiLower(char ch) {
  return std::islower(static_cast<unsigned char>(ch)) != 0;
}

bool IsAsciiUpper(char ch) {
  return std::isupper(static_cast<unsigned char>(ch)) != 0;
}

bool IsAsciiAlpha(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0;
}

bool IsAsciiPunct(char ch) {
  return std::ispunct(static_cast<unsigned char>(ch)) != 0;
}

bool IsAsciiSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

bool IsAsciiWord(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool ContainsWhitespace(const std::string& text) {
  for (char ch : text) {
    if (IsAsciiSpace(ch)) {
      return true;
    }
  }
  return false;
}

bool ContainsNonWordPunctSpace(const std::string& text) {
  for (char ch : text) {
    if (!IsAsciiWord(ch) && !IsAsciiPunct(ch) && !IsAsciiSpace(ch)) {
      return true;
    }
  }
  return false;
}

std::string RemovePunctAndSpace(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    if (!IsAsciiPunct(ch) && !IsAsciiSpace(ch)) {
      out.push_back(ch);
    }
  }
  return out;
}

std::string ToLowerAscii(const std::string& text) {
  std::string out(text);
  for (char& ch : out) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

std::string ToUpperAscii(const std::string& text) {
  std::string out(text);
  for (char& ch : out) {
    ch = static_cast<char>(
        std::toupper(static_cast<unsigned char>(ch)));
  }
  return out;
}

std::string UppercaseFirstAsciiAlpha(const std::string& text) {
  if (text.empty()) {
    return text;
  }
  std::string out(text);
  if (IsAsciiAlpha(out[0])) {
    out[0] = static_cast<char>(
        std::toupper(static_cast<unsigned char>(out[0])));
  }
  return out;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
  if (prefix.size() > text.size()) {
    return false;
  }
  return text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

AutoCapFilter::AutoCapFilter(const Ticket& ticket) : Filter(ticket) {}

an<Translation> AutoCapFilter::Apply(an<Translation> translation,
                                     CandidateList* /*candidates*/) {
  if (!translation || !engine_) {
    return translation;
  }

  Context* context = engine_->context();
  if (!context) {
    return translation;
  }

  const std::string& code = context->input();
  if (code.empty()) {
    return translation;
  }

  const char first_char = code[0];
  if (code.size() == 1 || IsAsciiLower(first_char) || IsAsciiPunct(first_char)) {
    return translation;
  }

  bool code_all_ucase = false;
  bool code_ucase = false;
  if (code.size() >= 2 && IsAsciiUpper(code[0]) && IsAsciiUpper(code[1])) {
    code_all_ucase = true;
  } else if (IsAsciiUpper(code[0])) {
    code_ucase = true;
  }

  if (!code_all_ucase && !code_ucase) {
    return translation;
  }

  const std::string pure_code = RemovePunctAndSpace(code);
  const std::string pure_code_lower = ToLowerAscii(pure_code);

  auto fifo = New<FifoTranslation>();
  while (!translation->exhausted()) {
    an<Candidate> cand = translation->Peek();
    translation->Next();

    const std::string cand_type = cand->type();
    const std::string text = cand->text();
    const std::string pure_text = RemovePunctAndSpace(text);

    if (ContainsNonWordPunctSpace(text) || ContainsWhitespace(text) ||
        StartsWith(pure_text, code) ||
        (cand_type != "completion" &&
         ToLowerAscii(pure_text) != pure_code_lower)) {
      fifo->Append(cand);
      continue;
    }

    std::string new_text = text;
    if (code_all_ucase) {
      new_text = ToUpperAscii(text);
    } else if (code_ucase) {
      new_text = UppercaseFirstAsciiAlpha(text);
    }

    if (new_text == text) {
      fifo->Append(cand);
      continue;
    }

    auto rewritten = New<SimpleCandidate>(
        cand->type(),
        cand->start(),
        cand->end(),
        new_text,
        cand->comment(),
        cand->preedit());
    rewritten->set_quality(cand->quality());
    fifo->Append(rewritten);
  }

  return fifo;
}

}  // namespace rime::aipara
