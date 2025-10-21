// 本文件实现了与“文本格式化/切分/查找”相关的函数。
// 主题关键词：UTF-8 安全处理、中文/英文标点替换、原始英文（rawenglish）区域跳过、
//             文本分段、日志辅助。
//
// 新手总览：
// - C++ 字符串（std::string）是字节序列，并不知道“字符”的概念；
//   UTF-8 下，一个“字符”可能占 1~4 个字节。因此“按字符”截取必须先解析 UTF-8。
// - 这里通过 BuildUtf8Index 建立“字节到字符”的索引，避免在多字节字符上截断。
// - 代码广泛使用 const 引用（const T&）避免拷贝、使用 std::optional 表示可能无值，
//   使用 std::vector / unordered_map 等 STL 容器管理数据。
// - 与 Python 对比：Python 的 str 是 Unicode 码点序列（更贴近“字符”），
//   C++ 里 std::string 是“字节”，要自己管理编码边界。
#include "text_formatting.h"

#include <rime/config.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace rime::aipara::text_formatting {
namespace {
// 此匿名命名空间（anonymous namespace）中的成员仅在本翻译单元内可见，
// 相当于“私有”实现细节，避免与其他 .cc 文件中的同名符号冲突。

// Utf8CharInfo 用来描述一个 UTF-8 字符（码点）在原字符串中的信息：
// - byte_index：此字符起始的“字节索引”（不是字符索引）。
// - byte_length：此字符占用的字节数（1~4）。
// - codepoint：Unicode 码点（32 位无符号整数）。
struct Utf8CharInfo {
  std::size_t byte_index;
  std::size_t byte_length;
  std::uint32_t codepoint;
};

constexpr std::string_view kChinesePosPrefix{"chinese_pos:"};

// 根据 UTF-8 领先字节（lead byte）判断一个字符占用的字节数。
// 坑点：不要把 char 直接当成有符号数参与位运算，这里用 unsigned char 更安全。
std::size_t Utf8SequenceLength(unsigned char lead) {
  if ((lead & 0x80) == 0) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    return 2;
  }
  if ((lead & 0xF0) == 0xE0) {
    return 3;
  }
  if ((lead & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

// 从给定字节偏移（offset）和长度（length）解码出一个 Unicode 码点。
// 假设传入的位置和长度是合法的 UTF-8 序列（调用方负责确保）。
// 返回 0 作为“无效/占位”。
std::uint32_t DecodeCodepoint(const std::string& text,
                              std::size_t offset,
                              std::size_t length) {
  if (offset >= text.size() || length == 0) {
    return 0;
  }
  const unsigned char* data = reinterpret_cast<const unsigned char*>(text.data());
  switch (length) {
    case 1:
      return data[offset];
    case 2:
      return ((data[offset] & 0x1F) << 6) |
             (data[offset + 1] & 0x3F);
    case 3:
      return ((data[offset] & 0x0F) << 12) |
             ((data[offset + 1] & 0x3F) << 6) |
             (data[offset + 2] & 0x3F);
    case 4:
      return ((data[offset] & 0x07) << 18) |
             ((data[offset + 1] & 0x3F) << 12) |
             ((data[offset + 2] & 0x3F) << 6) |
             (data[offset + 3] & 0x3F);
    default:
      return data[offset];
  }
}

// 将 Unicode 码点编码回 UTF-8 字节序列（std::string）。
// 常用于构造单字符的 UTF-8 字符串。
std::string EncodeCodepoint(std::uint32_t codepoint) {
  std::string result;
  if (codepoint <= 0x7F) {
    result.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return result;
}

// 构建 UTF-8 索引：线性扫描字符串，记录每个字符的起始字节位置、字节数、码点。
// 有了这个索引，就可以“按字符”安全地截取子串，而不是误按字节截断。
std::vector<Utf8CharInfo> BuildUtf8Index(const std::string& text) {
  std::vector<Utf8CharInfo> index;
  index.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t len =
        std::min<std::size_t>(Utf8SequenceLength(
                                   static_cast<unsigned char>(text[i])),
                               text.size() - i);
    index.push_back({i, len, DecodeCodepoint(text, i, len)});
    i += len;
  }
  return index;
}

// 将枚举值映射为对应的字符串。std::string_view 是“只读字符串视图”，
// 不拥有内存，避免不必要拷贝。类似 Python 的切片视图但不可变。
std::string_view SegmentTypeString(SegmentKind kind) {
  switch (kind) {
    case SegmentKind::kAbc:
      return "abc";
    case SegmentKind::kPunct:
      return "punct";
    case SegmentKind::kRawEnglishCombo:
      return "rawenglish_combo";
  }
  return "abc";
}

// 工厂函数：根据入参构造一个 TextSegment。
// 这里用值传参 + std::move 将临时字符串（content/original）高效移动到结构体里，
// 避免拷贝。C++11 的移动语义可显著减少内存分配与复制。
TextSegment MakeSegment(SegmentKind kind,
                        std::string content,
                        std::string original,
                        std::size_t start,
                        std::size_t end) {
  TextSegment segment;
  segment.kind = kind;
  segment.type = std::string(SegmentTypeString(kind));
  segment.content = std::move(content);
  segment.original = std::move(original);
  segment.start = start;
  segment.end = end;
  segment.length = end >= start ? end - start : 0;
  return segment;
}

std::string& EnglishModeSymbolStorage() {
  static std::string symbol = "`";
  return symbol;
}

// PunctMap：ASCII -> 中文标点 映射表。使用函数内静态指针+lambda 延迟初始化，
// 确保只构造一次（线程安全从 C++11 起保证）。
const std::vector<std::pair<std::string, std::string>>& PunctMap() {
  static const std::vector<std::pair<std::string, std::string>>* map = [] {
    auto* punct = new std::vector<std::pair<std::string, std::string>>{
        {",", u8"，"},
        {".", u8"。"},
        {"?", u8"？"},
        {"!", u8"！"},
        {":", u8"："},
        {";", u8"；"},
        {"(", u8"（"},
        {")", u8"）"},
        {"{", u8"｛"},
        {"}", u8"｝"},
        {"<", u8"《"},
        {">", u8"》"},
    };
    return punct;
  }();
  return *map;
}

// 中文标点集合，用于快速判断某个 UTF-8 字符是否中文标点。
const std::unordered_set<std::string>& ChinesePunctSet() {
  static const std::unordered_set<std::string>* set = [] {
    auto* punct = new std::unordered_set<std::string>{
        std::string(u8"“"), std::string(u8"”"), std::string(u8"‘"),
        std::string(u8"’"), std::string(u8"，"), std::string(u8"。"),
        std::string(u8"？"), std::string(u8"！"), std::string(u8"："),
        std::string(u8"；"), std::string(u8"（"), std::string(u8"）"),
        std::string(u8"【"), std::string(u8"】"), std::string(u8"｛"),
        std::string(u8"｝"), std::string(u8"《"), std::string(u8"》"),
        std::string(u8"、"), std::string(u8"…"), std::string(u8"……"),
        std::string(u8"—"), std::string(u8"·"), std::string(u8"〈"),
        std::string(u8"〉"), std::string(u8"「"), std::string(u8"」"),
        std::string(u8"『"), std::string(u8"』"), std::string(u8"〔"),
        std::string(u8"〕"), std::string(u8"〖"), std::string(u8"〗"),
    };
    return punct;
  }();
  return *set;
}

// handle_keys 表：把按键名（如 "comma"、"Shift+1"）映射为实际字符。
// SmartCursorProcessor 的未处理键监控会用它把按键转换成可回传服务端的字符。
const std::unordered_map<std::string, std::string>& HandleKeysStorage() {
  static const std::unordered_map<std::string, std::string>* map = [] {
    auto* handle = new std::unordered_map<std::string, std::string>{
        {"space", " "},
        {"1", "1"},
        {"2", "2"},
        {"3", "3"},
        {"4", "4"},
        {"5", "5"},
        {"6", "6"},
        {"7", "7"},
        {"8", "8"},
        {"9", "9"},
        {"0", "0"},
        {"Shift+1", "!"},
        {"Shift+2", "@"},
        {"Shift+3", "#"},
        {"Shift+4", "$"},
        {"Shift+5", "%"},
        {"Shift+6", "^"},
        {"Shift+7", "&"},
        {"Shift+8", "*"},
        {"Shift+9", "("},
        {"Shift+0", ")"},
        {"period", "."},
        {"comma", ","},
        {"semicolon", ";"},
        {"apostrophe", "'"},
        {"bracketleft", "["},
        {"bracketright", "]"},
        {"hyphen", "-"},
        {"equal", "="},
        {"slash", "/"},
        {"backslash", "\\"},
        {"grave", "`"},
        {"Shift+semicolon", ":"},
        {"Shift+apostrophe", "\""},
        {"Shift+bracketleft", "{"},
        {"Shift+bracketright", "}"},
        {"Shift+hyphen", "_"},
        {"Shift+equal", "+"},
        {"Shift+slash", "?"},
        {"Shift+backslash", "|"},
        {"Shift+grave", "~"},
        {"minus", "-"},
        {"colon", ":"},
        {"question", "?"},
        {"exclam", "!"},
        {"quotedbl", "\""},
        {"parenleft", "("},
        {"parenright", ")"},
        {"braceleft", "{"},
        {"braceright", "}"},
        {"underscore", "_"},
        {"plus", "+"},
        {"asterisk", "*"},
        {"at", "@"},
        {"numbersign", "#"},
        {"dollar", "$"},
        {"percent", "%"},
        {"ampersand", "&"},
        {"less", "<"},
        {"greater", ">"},
        {"asciitilde", "~"},
        {"asciicircum", "^"},
        {"bar", "|"},
        {"Shift+colon", ":"},
        {"Shift+question", "?"},
        {"Shift+exclam", "!"},
        {"Shift+quotedbl", "\""},
        {"Shift+parenleft", "("},
        {"Shift+parenright", ")"},
        {"Shift+braceleft", "{"},
        {"Shift+braceright", "}"},
        {"Shift+underscore", "_"},
        {"Shift+plus", "+"},
        {"Shift+asterisk", "*"},
        {"Shift+at", "@"},
        {"Shift+numbersign", "#"},
        {"Shift+dollar", "$"},
        {"Shift+percent", "%"},
        {"Shift+ampersand", "&"},
        {"Shift+less", "<"},
        {"Shift+greater", ">"},
        {"Shift+asciitilde", "~"},
        {"Shift+asciicircum", "^"},
        {"Shift+bar", "|"},
    };
    return handle;
  }();
  return *map;
}

// 字符串批量替换：把 text 中的所有 from 子串替换为 to。
// 传入指针 std::string* 是为了“原地修改”字符串；
// Python 类比：函数内直接修改传入的 list/dict。
void ReplaceAll(std::string* text,
                const std::string& from,
                const std::string& to) {
  if (!text || from.empty()) {
    return;
  }
  std::size_t pos = 0;
  while ((pos = text->find(from, pos)) != std::string::npos) {
    text->replace(pos, from.size(), to);
    pos += to.size();
  }
}

// 判断是否包含 ASCII 标点。include_backtick 控制是否把反引号 ` 也算进标点集合。
bool ContainsAsciiPunctuation(const std::string& text,
                              bool include_backtick) {
  static const std::string kAscii =
      ",.!?;:()[]<>/_=+*&^%$#@~`|\\-'\"";
  static const std::string kAsciiNoBacktick =
      ",.!?;:()[]<>/_=+*&^%$#@~|\\-'\"";
  const std::string& table = include_backtick ? kAscii : kAsciiNoBacktick;
  for (unsigned char ch : text) {
    if (table.find(static_cast<char>(ch)) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// 判断是否包含中文标点（需要按 UTF-8 字符粒度检查，而不是逐字节）。
bool ContainsChinesePunctuation(const std::string& text) {
  const auto index = BuildUtf8Index(text);
  const auto& set = ChinesePunctSet();
  for (const auto& entry : index) {
    std::string ch = text.substr(entry.byte_index, entry.byte_length);
    if (set.find(ch) != set.end()) {
      return true;
    }
  }
  return false;
}

// 是否为“切分”标点字符：用于把连续英文/数字与标点分段。
bool IsSplitterPunctuation(unsigned char ch) {
  static const std::string kSplitterChars =
      ",.!?;:()[]<>/_=+*&^%$#@~|%-`'\"";
  return kSplitterChars.find(static_cast<char>(ch)) != std::string::npos;
}

// 判断某个字节位置 pos 是否落在某个范围对 [first, second) 内。
bool PositionInRanges(std::size_t pos,
                      const std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
  for (const auto& range : ranges) {
    if (pos >= range.first && pos < range.second) {
      return true;
    }
  }
  return false;
}

// 根据“英文模式符号”（如反引号）构建需要跳过处理的范围列表：
// 范围包括成对符号包裹的内容，若存在单个未闭合的开始符号，则范围延伸到字符串末尾。
std::vector<std::pair<std::size_t, std::size_t>> BuildRawEnglishRanges(
    const std::string& input) {
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  const std::string& symbol = english_mode_symbol();
  if (symbol.empty()) {
    return ranges;
  }
  const std::size_t symbol_len = symbol.size();
  std::vector<std::size_t> positions;
  for (std::size_t pos = input.find(symbol);
       pos != std::string::npos;
       pos = input.find(symbol, pos + symbol_len)) {
    positions.push_back(pos);
  }
  if (positions.empty()) {
    return ranges;
  }
  const bool has_unpaired = positions.size() % 2 == 1;
  for (std::size_t i = 0; i + 1 < positions.size(); i += 2) {
    ranges.emplace_back(positions[i], positions[i + 1] + symbol_len);
  }
  if (has_unpaired) {
    ranges.emplace_back(positions.back(), input.size());
  }
  return ranges;
}

// 打印片段列表到日志，便于调试。
void LogSegments(Logger* logger,
                 const std::vector<TextSegment>& segments) {
  if (!logger) {
    return;
  }
  AIPARA_LOG_INFO(*logger, "切分结果:");
  for (std::size_t i = 0; i < segments.size(); ++i) {
    std::ostringstream stream;
    stream << "  片段" << (i + 1) << ": 类型=" << segments[i].type
           << ", 内容='" << segments[i].content << "'";
    AIPARA_LOG_INFO(*logger, stream.str());
  }
}

}  // namespace

// 根据 Rime Config 更新当前模块使用的英文模式标识符（如反引号）。
// config 可能为 nullptr，因此调用前后都要判空。
void UpdateCurrentConfig(Config* config) {
  if (!config) {
    return;
  }
  std::string symbol;
  if (config->GetString("translator/english_mode_symbol", &symbol) &&
      !symbol.empty()) {
    SetEnglishModeSymbol(symbol);
  }
}

// 设置英文模式符号。传引用（const std::string&）避免拷贝；内部保存时直接赋值。
void SetEnglishModeSymbol(const std::string& symbol) {
  if (!symbol.empty()) {
    EnglishModeSymbolStorage() = symbol;
  }
}

// 提供英文模式符号的只读引用，避免额外拷贝。
const std::string& english_mode_symbol() {
  return EnglishModeSymbolStorage();
}

const std::unordered_map<std::string, std::string>& handle_keys() {
  return HandleKeysStorage();
}

// 按“字符”而非字节截取子串：
// - start_char/end_char 以“1”为起始（类似 Lua），支持负数（从末尾算起）。
// - 与 Python 切片不同：这里是包含式下标，最终会换算为字节范围做 substr。
// 常见坑：直接用 substr(字节偏移) 会把多字节字符截断，导致乱码。
std::string Utf8Substr(const std::string& str,
                       int start_char,
                       int end_char) {
  const auto index = BuildUtf8Index(str);
  const int char_len = static_cast<int>(index.size());
  if (char_len == 0) {
    return std::string();
  }
  if (start_char == 0) {
    start_char = 1;
  }
  if (end_char < 0) {
    end_char = char_len + end_char + 1;
  }
  if (start_char < 0) {
    start_char = char_len + start_char + 1;
  }
  if (end_char == 0) {
    end_char = char_len;
  }
  start_char = std::max(start_char, 1);
  end_char = std::min(end_char, char_len);
  if (start_char > end_char) {
    return std::string();
  }
  const std::size_t start_index = static_cast<std::size_t>(start_char - 1);
  const std::size_t end_index = static_cast<std::size_t>(end_char - 1);
  const std::size_t start_byte = index[start_index].byte_index;
  const std::size_t end_byte =
      index[end_index].byte_index + index[end_index].byte_length;
  return str.substr(start_byte, end_byte - start_byte);
}

// 替换英文双引号为中文引号，并记录“当前是否处于开引号状态”。
// 这样可以跨多段文本连续处理成对引号。返回 pair 的第二个布尔值就是状态位。
std::pair<std::string, bool> ReplaceQuotesRecordSingle(
    const std::string& text,
    bool double_quote_open) {
  if (text.empty()) {
    return {text, double_quote_open};
  }
  std::string result;
  result.reserve(text.size());
  for (char ch : text) {
    if (ch == '"') {
      if (double_quote_open) {
        result.append(u8"“");
        double_quote_open = false;
      } else {
        result.append(u8"”");
        double_quote_open = true;
      }
    } else {
      result.push_back(ch);
    }
  }
  return {result, double_quote_open};
}

// 简化版本：从“开引号”状态为 true 开始替换。
std::string ReplaceQuotes(const std::string& text) {
  return ReplaceQuotesRecordSingle(text, true).first;
}

// 将英文标点替换成中文（含引号替换）。
// 处理顺序：先引号，再其它标点，避免冲突。
std::string ReplacePunct(const std::string& text) {
  if (text.empty()) {
    return text;
  }
  std::string result = ReplaceQuotes(text);
  for (const auto& entry : PunctMap()) {
    ReplaceAll(&result, entry.first, entry.second);
  }
  return result;
}

// 根据“中文文本位置列表”进行选择性替换：
// - chinese_pos 形如 "chinese_pos:start,end,..."，单位为“字符序号”（非字节）。
// - 只对这些区间对应的文本执行标点替换；其他部分保持原样。
// - 返回 std::nullopt 表示坐标串非法或为空。
std::optional<std::string> ReplacePunctSkipPos(
    const std::string& text,
    const std::string& chinese_pos,
    Logger* logger) {
  if (chinese_pos.rfind(kChinesePosPrefix, 0) != 0) {
    if (logger) {
      AIPARA_LOG_INFO(*logger, "坐标字符串格式不正确或为空，不进行替换");
    }
    return std::nullopt;
  }

  std::vector<std::pair<int, int>> ranges;
  std::string payload = chinese_pos.substr(kChinesePosPrefix.size());
  std::size_t pos = 0;
  while (pos < payload.size()) {
    const std::size_t comma = payload.find(',', pos);
    if (comma == std::string::npos) {
      break;
    }
    const std::string start_str = payload.substr(pos, comma - pos);
    pos = comma + 1;
    if (pos >= payload.size()) {
      break;
    }
    const std::size_t comma2 = payload.find(',', pos);
    if (comma2 == std::string::npos) {
      break;
    }
    const std::string end_str = payload.substr(pos, comma2 - pos);
    pos = comma2 + 1;
    try {
      int start_num = std::stoi(start_str);
      int end_num = std::stoi(end_str);
      ranges.emplace_back(start_num, end_num);
    } catch (...) {
      // ignore invalid entries
    }
  }

  if (ranges.empty()) {
    return std::nullopt;
  }

  std::string final_text;
  bool chinese_first = false;
  int last_end_num = 0;
  bool double_quote_open = true;

  for (const auto& range : ranges) {
    const int start_num = range.first;
    const int end_num = range.second;
    if (logger) {
      std::ostringstream stream;
      stream << "start_num: " << start_num << " end_num: " << end_num;
      AIPARA_LOG_INFO(*logger, stream.str());
    }

    if (start_num == 1) {
      chinese_first = true;
    } else {
      std::string english_str;
      if (!chinese_first) {
        english_str = Utf8Substr(text, 1, start_num - 1);
      } else {
        english_str = Utf8Substr(text, last_end_num + 1, start_num - 1);
      }
      final_text += english_str;
    }

    std::string chinese_str = Utf8Substr(text, start_num, end_num);
    if (HasPunctuationNoRawEnglish(chinese_str, logger)) {
      for (const auto& entry : PunctMap()) {
        ReplaceAll(&chinese_str, entry.first, entry.second);
      }
      auto replaced = ReplaceQuotesRecordSingle(chinese_str, double_quote_open);
      chinese_str = std::move(replaced.first);
      double_quote_open = replaced.second;
    }

    if (logger) {
      AIPARA_LOG_DEBUG(*logger, std::string("chinese_str: ") + chinese_str);
    }

    final_text += chinese_str;
    last_end_num = end_num;
  }

  const int total_chars = static_cast<int>(BuildUtf8Index(text).size());
  if (last_end_num < total_chars) {
    final_text += Utf8Substr(text, last_end_num + 1, -1);
  }

  return final_text;
}

// 跳过“英文模式”区域的标点替换：
// - 若文本中不含英文模式符号，则退化为普通 ReplacePunct。
// - 若含有，则在符号包裹的片段内不做替换，保持原样。
std::string ReplacePunctSkipRawEnglish(const std::string& text,
                                       Logger* logger) {
  if (text.empty()) {
    return text;
  }
  const std::string& symbol = english_mode_symbol();
  if (text.find(symbol) == std::string::npos) {
    if (logger) {
      AIPARA_LOG_INFO(*logger, "未发现英文模式符号, 使用原来的标点符号替换模式");
    }
    return ReplacePunct(text);
  }

  if (logger) {
    AIPARA_LOG_INFO(*logger, "发现反引号, 使用跳过反引号的标点符号替换模式");
  }

  const std::size_t symbol_len = symbol.size();
  std::string result;
  std::size_t index = 0;
  bool in_rawenglish = false;
  std::size_t raw_start = 0;

  while (index < text.size()) {
    const std::size_t next_symbol = text.find(symbol, index);
    if (!in_rawenglish) {
      if (next_symbol == std::string::npos) {
        result += ReplacePunct(text.substr(index));
        break;
      }
      if (next_symbol > index) {
        result += ReplacePunct(text.substr(index, next_symbol - index));
      }
      raw_start = next_symbol;
      index = next_symbol + symbol_len;
      in_rawenglish = true;
    } else {
      if (next_symbol == std::string::npos) {
        result += text.substr(raw_start);
        break;
      }
      const std::size_t raw_end = next_symbol + symbol_len;
      result += text.substr(raw_start, raw_end - raw_start);
      index = raw_end;
      in_rawenglish = false;
    }
  }

  return result;
}

// 原始版本的替换：不处理引号开合，仅用映射表逐个替换。
std::string ReplacePunctOriginal(const std::string& text) {
  if (text.empty()) {
    return text;
  }
  std::string result = text;
  for (const auto& entry : PunctMap()) {
    ReplaceAll(&result, entry.first, entry.second);
  }
  return result;
}

// 快速检测字符串是否包含任何 ASCII 标点（包括 `）。
bool HasPunctuation(const std::string& text, Logger* logger) {
  if (text.empty()) {
    return false;
  }
  if (logger) {
    AIPARA_LOG_INFO(*logger,
                    std::string("检测输入内容是否包含标点符号: ") + text);
  }
  const bool has_punct = ContainsAsciiPunctuation(text, true);
  if (logger) {
    AIPARA_LOG_INFO(*logger,
                    std::string("has_punct: ") + (has_punct ? "true" : "false"));
  }
  return has_punct;
}

// 检测是否包含标点（不计入反引号），若没有 ASCII 标点再检查中文标点集合。
bool HasPunctuationNoRawEnglish(const std::string& text, Logger* logger) {
  if (text.empty()) {
    return false;
  }
  if (logger) {
    AIPARA_LOG_INFO(*logger, std::string("检测输入内容是否包含标点符号(不含反引号): ") +
                                  text);
  }
  bool has_punct = ContainsAsciiPunctuation(text, false);
  if (!has_punct) {
    has_punct = ContainsChinesePunctuation(text);
  }
  if (logger) {
    AIPARA_LOG_INFO(*logger, std::string("has_punct(no rawenglish): ") +
                                  (has_punct ? "true" : "false"));
  }
  return has_punct;
}

std::vector<TextSegment> SplitAndConvertInput(
    const std::string& input,
    bool replace_punct_enabled) {
  return SplitAndConvertInputWithDelimiter(
      input, std::string(), std::string(), replace_punct_enabled);
}

std::vector<TextSegment> SplitAndConvertInputWithDelimiter(
    const std::string& input,
    const std::string& rawenglish_delimiter_before,
    const std::string& rawenglish_delimiter_after,
    bool replace_punct_enabled) {
  const std::string& symbol = english_mode_symbol();
  const std::size_t symbol_len = symbol.size();

  std::vector<std::size_t> rawenglish_positions;
  for (std::size_t pos = input.find(symbol);
       pos != std::string::npos;
       pos = input.find(symbol, pos + symbol_len)) {
    rawenglish_positions.push_back(pos);
  }
  const bool has_unpaired = rawenglish_positions.size() % 2 == 1;

  std::vector<TextSegment> segments;
  std::string current_segment;
  bool in_rawenglish = false;
  std::string rawenglish_content;
  std::size_t rawenglish_start = 0;
  std::size_t rawenglish_pair_index = 0;

  std::size_t i = 0;
  while (i < input.size()) {
    const bool at_symbol =
        !symbol.empty() && input.compare(i, symbol_len, symbol) == 0;

    if (has_unpaired &&
        rawenglish_pair_index == rawenglish_positions.size() - 1 &&
        at_symbol) {
      if (!current_segment.empty()) {
        const std::size_t start = i - current_segment.size();
        segments.push_back(MakeSegment(SegmentKind::kAbc,
                                       current_segment,
                                       current_segment,
                                       start,
                                       i));
        current_segment.clear();
      }
      const std::string remaining = input.substr(i + symbol_len);
      const std::string processed = rawenglish_delimiter_before + remaining +
                                    rawenglish_delimiter_after;
      const std::string original = symbol + remaining;
      segments.push_back(MakeSegment(SegmentKind::kRawEnglishCombo,
                                     processed,
                                     original,
                                     i,
                                     input.size()));
      break;
    }

    if (at_symbol) {
      ++rawenglish_pair_index;
      if (!in_rawenglish) {
        if (!current_segment.empty()) {
          const std::size_t start = i - current_segment.size();
          segments.push_back(MakeSegment(SegmentKind::kAbc,
                                         current_segment,
                                         current_segment,
                                         start,
                                         i));
          current_segment.clear();
        }
        rawenglish_start = i;
        rawenglish_content.clear();
        in_rawenglish = true;
      } else {
        const std::string processed = rawenglish_delimiter_before +
                                      rawenglish_content +
                                      rawenglish_delimiter_after;
        const std::size_t end = i + symbol_len;
        const std::string original =
            input.substr(rawenglish_start, end - rawenglish_start);
        segments.push_back(MakeSegment(SegmentKind::kRawEnglishCombo,
                                       processed,
                                       original,
                                       rawenglish_start,
                                       end));
        in_rawenglish = false;
        rawenglish_content.clear();
      }
      i += symbol_len;
      continue;
    }

    if (in_rawenglish) {
      rawenglish_content.push_back(input[i]);
    } else {
      const unsigned char byte = static_cast<unsigned char>(input[i]);
      if (IsSplitterPunctuation(byte)) {
        if (!current_segment.empty()) {
          const std::size_t start = i - current_segment.size();
          segments.push_back(MakeSegment(SegmentKind::kAbc,
                                         current_segment,
                                         current_segment,
                                         start,
                                         i));
          current_segment.clear();
        }
        const std::string punct_original = input.substr(i, 1);
        const std::string punct_content =
            replace_punct_enabled ? ReplacePunct(punct_original)
                                  : punct_original;
        segments.push_back(MakeSegment(SegmentKind::kPunct,
                                       punct_content,
                                       punct_original,
                                       i,
                                       i + 1));
      } else {
        current_segment.push_back(input[i]);
      }
    }

    ++i;
  }

  if (in_rawenglish) {
    const std::string processed = rawenglish_delimiter_before +
                                  rawenglish_content +
                                  rawenglish_delimiter_after;
    const std::string original = input.substr(rawenglish_start);
    segments.push_back(MakeSegment(SegmentKind::kRawEnglishCombo,
                                   processed,
                                   original,
                                   rawenglish_start,
                                   input.size()));
  } else if (!current_segment.empty()) {
    const std::size_t start = input.size() - current_segment.size();
    segments.push_back(MakeSegment(SegmentKind::kAbc,
                                   current_segment,
                                   current_segment,
                                   start,
                                   input.size()));
  }

  return segments;
}

std::vector<TextSegment> SplitByRawEnglish(
    const std::string& input,
    std::size_t seg_start,
    std::size_t /*seg_end*/,  // 未使用，但保持接口一致
    const std::string& delimiter_before,
    const std::string& delimiter_after) {
  const std::string& symbol = english_mode_symbol();
  const std::size_t symbol_len = symbol.size();

  std::vector<std::size_t> rawenglish_positions;
  for (std::size_t pos = input.find(symbol);
       pos != std::string::npos;
       pos = input.find(symbol, pos + symbol_len)) {
    rawenglish_positions.push_back(pos);
  }
  const bool has_unpaired = rawenglish_positions.size() % 2 == 1;

  std::vector<TextSegment> segments;
  std::string current_segment;
  bool in_rawenglish = false;
  std::string rawenglish_content;
  std::size_t rawenglish_start = 0;
  std::size_t rawenglish_pair_index = 0;

  std::size_t i = 0;
  while (i < input.size()) {
    const bool at_symbol =
        !symbol.empty() && input.compare(i, symbol_len, symbol) == 0;

    if (has_unpaired &&
        rawenglish_pair_index == rawenglish_positions.size() - 1 &&
        at_symbol) {
      if (!current_segment.empty()) {
        const std::size_t start = seg_start + i - current_segment.size();
        segments.push_back(MakeSegment(SegmentKind::kAbc,
                                       current_segment,
                                       current_segment,
                                       start,
                                       seg_start + i));
        current_segment.clear();
      }
      const std::string remaining = input.substr(i + symbol_len);
      const std::string processed = delimiter_before + remaining +
                                    delimiter_after;
      const std::string original = symbol + remaining;
      segments.push_back(MakeSegment(SegmentKind::kRawEnglishCombo,
                                     processed,
                                     original,
                                     seg_start + i,
                                     seg_start + input.size()));
      break;
    }

    if (at_symbol) {
      ++rawenglish_pair_index;
      if (!in_rawenglish) {
        if (!current_segment.empty()) {
          const std::size_t start = seg_start + i - current_segment.size();
          segments.push_back(MakeSegment(SegmentKind::kAbc,
                                         current_segment,
                                         current_segment,
                                         start,
                                         seg_start + i));
          current_segment.clear();
        }
        rawenglish_start = i;
        rawenglish_content.clear();
        in_rawenglish = true;
      } else {
        const std::string processed = delimiter_before +
                                      rawenglish_content +
                                      delimiter_after;
        const std::size_t end = i + symbol_len;
        const std::string original =
            input.substr(rawenglish_start, end - rawenglish_start);
        segments.push_back(MakeSegment(SegmentKind::kRawEnglishCombo,
                                       processed,
                                       original,
                                       seg_start + rawenglish_start,
                                       seg_start + end));
        in_rawenglish = false;
        rawenglish_content.clear();
      }
      i += symbol_len;
      continue;
    }

    if (in_rawenglish) {
      rawenglish_content.push_back(input[i]);
    } else {
      current_segment.push_back(input[i]);
    }

    ++i;
  }

  if (in_rawenglish) {
    const std::string processed = delimiter_before +
                                  rawenglish_content +
                                  delimiter_after;
    const std::string original = input.substr(rawenglish_start);
    segments.push_back(MakeSegment(SegmentKind::kRawEnglishCombo,
                                   processed,
                                   original,
                                   seg_start + rawenglish_start,
                                   seg_start + input.size()));
  } else if (!current_segment.empty()) {
    const std::size_t start = seg_start + input.size() - current_segment.size();
    segments.push_back(MakeSegment(SegmentKind::kAbc,
                                   current_segment,
                                   current_segment,
                                   start,
                                   seg_start + input.size()));
  }

  return segments;
}

std::vector<TextSegment> SplitAndConvertInputWithLog(
    const std::string& input,
    Logger* logger,
    bool replace_punct_enabled) {
  if (logger) {
    AIPARA_LOG_INFO(*logger, std::string("开始处理输入: ") + input);
  }
  auto segments = SplitAndConvertInput(input, replace_punct_enabled);
  LogSegments(logger, segments);
  return segments;
}

std::vector<TextSegment> SplitAndConvertInputWithLogAndDelimiter(
    const std::string& input,
    Logger* logger,
    const std::string& rawenglish_delimiter_before,
    const std::string& rawenglish_delimiter_after,
    bool replace_punct_enabled) {
  if (logger) {
    std::ostringstream stream;
    stream << "开始处理输入: " << input << "，英文模式符号分隔符: '"
           << rawenglish_delimiter_before << "' '"
           << rawenglish_delimiter_after << "'";
    AIPARA_LOG_INFO(*logger, stream.str());
    AIPARA_LOG_INFO(*logger, std::string("标点符号替换开关: ") +
                                (replace_punct_enabled ? "true" : "false"));
  }
  auto segments = SplitAndConvertInputWithDelimiter(
      input,
      rawenglish_delimiter_before,
      rawenglish_delimiter_after,
      replace_punct_enabled);
  LogSegments(logger, segments);
  return segments;
}

std::vector<TextSegment> SplitByRawEnglishWithLog(
    const std::string& input,
    std::size_t seg_start,
    std::size_t seg_end,
    const std::string& delimiter_before,
    const std::string& delimiter_after,
    Logger* logger) {
  if (logger) {
    std::ostringstream stream;
    stream << "开始使用split_by_rawenglish处理输入: " << input
           << "，分隔符: '" << delimiter_before << "' '"
           << delimiter_after << "'";
    AIPARA_LOG_INFO(*logger, stream.str());
  }
  auto segments = SplitByRawEnglish(
      input, seg_start, seg_end, delimiter_before, delimiter_after);
  LogSegments(logger, segments);
  return segments;
}

std::optional<std::size_t> FindTextSkipRawEnglish(
    const std::string& input,
    const std::string& search_str,
    std::size_t start_pos,
    Logger* logger) {
  if (logger) {
    std::ostringstream stream;
    stream << "开始搜索: 输入='" << input << "', 搜索字符串='"
           << search_str << "', 起始位置=" << start_pos;
    AIPARA_LOG_INFO(*logger, stream.str());
  }

  const std::string& symbol = english_mode_symbol();
  if (input.find(symbol) == std::string::npos) {
    if (logger) {
      AIPARA_LOG_INFO(*logger, "未发现英文模式符号，使用原来的搜索方式");
    }
    const std::size_t found = input.find(search_str, start_pos);
    if (found != std::string::npos) {
      if (logger) {
        std::ostringstream stream;
        stream << "找到匹配: 位置=" << found;
        AIPARA_LOG_INFO(*logger, stream.str());
      }
      return found;
    }
    if (logger) {
      AIPARA_LOG_INFO(*logger, "未找到匹配");
    }
    return std::nullopt;
  }

  const auto ranges = BuildRawEnglishRanges(input);
  std::size_t current_pos = start_pos;
  while (current_pos <= input.size()) {
    const std::size_t found = input.find(search_str, current_pos);
    if (found == std::string::npos) {
      if (logger) {
        AIPARA_LOG_INFO(*logger, "未找到匹配");
      }
      return std::nullopt;
    }
    if (logger) {
      std::ostringstream stream;
      stream << "string.find找到候选位置: " << found;
      AIPARA_LOG_INFO(*logger, stream.str());
    }
    if (!PositionInRanges(found, ranges)) {
      if (logger) {
        std::ostringstream stream;
        stream << "找到有效匹配: 位置=" << found;
        AIPARA_LOG_INFO(*logger, stream.str());
      }
      return found;
    }
    if (logger) {
      std::ostringstream stream;
      stream << "位置" << found << "处于英文模式符号区域内，继续搜索";
      AIPARA_LOG_INFO(*logger, stream.str());
    }
    current_pos = found + 1;
  }
  if (logger) {
    AIPARA_LOG_INFO(*logger, "未找到匹配");
  }
  return std::nullopt;
}

std::optional<std::size_t> FindTextSkipRawEnglishWithWrap(
    const std::string& input,
    const std::string& search_str,
    std::size_t start_pos,
    Logger* logger) {
  auto found = FindTextSkipRawEnglish(input, search_str, start_pos, logger);
  if (found) {
    return found;
  }
  if (start_pos > 0) {
    if (logger) {
      AIPARA_LOG_INFO(*logger, "从指定位置未找到，从头开始搜索");
    }
    return FindTextSkipRawEnglish(input, search_str, 0, logger);
  }
  return std::nullopt;
}

bool IsPositionInRawEnglish(const std::string& input, std::size_t pos) {
  const auto ranges = BuildRawEnglishRanges(input);
  return PositionInRanges(pos, ranges);
}

}  // namespace rime::aipara::text_formatting
