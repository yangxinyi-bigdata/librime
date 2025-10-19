#ifndef PLUGINS_AIPARA_SRC_COMMON_TEXT_FORMATTING_H_
#define PLUGINS_AIPARA_SRC_COMMON_TEXT_FORMATTING_H_

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.h"

namespace rime {
class Config;
}  // namespace rime

namespace rime::aipara::text_formatting {

// Enumeration describing the semantic kind of a text segment.
enum class SegmentKind {
  kAbc,
  kPunct,
  kRawEnglishCombo,
};

// C++ representation of the Lua segment table used across the plugin.
struct TextSegment {
  SegmentKind kind = SegmentKind::kAbc;
  std::string type;       // "abc", "punct", "rawenglish_combo"
  std::string content;    // formatted content that downstream code consumes
  std::string original;   // original slice from the input string
  std::size_t start = 0;  // byte index (inclusive)
  std::size_t end = 0;    // byte index (exclusive)
  std::size_t length = 0; // byte length of the slice
};

// Accessors for static configuration shared across helpers.
void UpdateCurrentConfig(Config* config);
void SetEnglishModeSymbol(const std::string& symbol);
const std::string& english_mode_symbol();

// Lua exposed helper tables and primitives.
const std::unordered_map<std::string, std::string>& handle_keys();
std::string Utf8Substr(const std::string& str,
                       int start_char = 1,
                       int end_char = -1);
std::pair<std::string, bool> ReplaceQuotesRecordSingle(
    const std::string& text,
    bool double_quote_open);
std::string ReplaceQuotes(const std::string& text);
std::string ReplacePunct(const std::string& text);
std::optional<std::string> ReplacePunctSkipPos(
    const std::string& text,
    const std::string& chinese_pos,
    Logger* logger);
std::string ReplacePunctSkipRawEnglish(const std::string& text,
                                       Logger* logger);
std::string ReplacePunctOriginal(const std::string& text);
bool HasPunctuation(const std::string& text, Logger* logger);
bool HasPunctuationNoRawEnglish(const std::string& text,
                                Logger* logger);

// Splitting and conversion helpers mirroring the Lua module.
std::vector<TextSegment> SplitAndConvertInput(
    const std::string& input,
    bool replace_punct_enabled = false);
std::vector<TextSegment> SplitAndConvertInputWithDelimiter(
    const std::string& input,
    const std::string& rawenglish_delimiter_before,
    const std::string& rawenglish_delimiter_after,
    bool replace_punct_enabled = false);
std::vector<TextSegment> SplitByRawEnglish(
    const std::string& input,
    std::size_t seg_start = 0,
    std::size_t seg_end = 0,
    const std::string& delimiter_before = std::string{},
    const std::string& delimiter_after = std::string{});

// Logging enabled wrappers.
std::vector<TextSegment> SplitAndConvertInputWithLog(
    const std::string& input,
    Logger* logger,
    bool replace_punct_enabled = false);
std::vector<TextSegment> SplitAndConvertInputWithLogAndDelimiter(
    const std::string& input,
    Logger* logger,
    const std::string& rawenglish_delimiter_before,
    const std::string& rawenglish_delimiter_after,
    bool replace_punct_enabled = false);
std::vector<TextSegment> SplitByRawEnglishWithLog(
    const std::string& input,
    std::size_t seg_start,
    std::size_t seg_end,
    const std::string& delimiter_before,
    const std::string& delimiter_after,
    Logger* logger);

// Search helpers.
std::optional<std::size_t> FindTextSkipRawEnglish(
    const std::string& input,
    const std::string& search_str,
    std::size_t start_pos,
    Logger* logger);
std::optional<std::size_t> FindTextSkipRawEnglishWithWrap(
    const std::string& input,
    const std::string& search_str,
    std::size_t start_pos,
    Logger* logger);
bool IsPositionInRawEnglish(const std::string& input, std::size_t pos);

}  // namespace rime::aipara::text_formatting

#endif  // PLUGINS_AIPARA_SRC_COMMON_TEXT_FORMATTING_H_
