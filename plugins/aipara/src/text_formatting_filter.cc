#include "text_formatting_filter.h"

#include <rime/candidate.h>
#include <rime/translation.h>

#include <cstddef>
#include <string>

namespace rime::aipara {
namespace {

bool HasEscapedSequence(const std::string& text) {
  if (text.size() < 2) {
    return false;
  }
  for (std::size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] != '\\') {
      continue;
    }
    const char next = text[i + 1];
    if (next == 'n' || next == 't' || next == 'r' || next == 's' ||
        next == 'd' || next == '\\') {
      return true;
    }
  }
  return false;
}

std::string ReplaceEscapedSequence(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      const char next = text[i + 1];
      switch (next) {
        case 'n':
          out.push_back('\n');
          ++i;
          continue;
        case 't':
          out.push_back('\t');
          ++i;
          continue;
        case 'r':
          out.push_back('\r');
          ++i;
          continue;
        case 's':
          out.push_back(' ');
          ++i;
          continue;
        case 'd':
          out.push_back('-');
          ++i;
          continue;
        case '\\':
          out.push_back('\\');
          ++i;
          continue;
        default:
          break;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

}  // namespace

TextFormattingFilter::TextFormattingFilter(const Ticket& ticket)
    : Filter(ticket) {}

an<Translation> TextFormattingFilter::Apply(an<Translation> translation,
                                            CandidateList* /*candidates*/) {
  if (!translation) {
    return translation;
  }

  auto fifo = New<FifoTranslation>();
  while (!translation->exhausted()) {
    an<Candidate> cand = translation->Peek();
    translation->Next();

    const std::string text = cand->text();
    if (!HasEscapedSequence(text)) {
      fifo->Append(cand);
      continue;
    }

    const std::string new_text = ReplaceEscapedSequence(text);
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
