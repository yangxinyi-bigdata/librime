// 说明：这是一个头文件（header）。在 C/C++ 里，头文件通常用来声明
// 类型（struct/class/enum）、函数、常量等，供其他 .cc/.cpp 源文件引用。
//
// 新手要点：
// - “声明”与“定义”的区别：头文件里通常只“声明”（告诉编译器有这个东西），
//   具体的实现放在 .cc 文件里（“定义”）。
// - include guard（包含保护）：防止同一个头文件被多次包含导致重复定义错误。
//   下面的宏对就是 include guard 的常见写法。
#ifndef PLUGINS_AIPARA_SRC_COMMON_TEXT_FORMATTING_H_
#define PLUGINS_AIPARA_SRC_COMMON_TEXT_FORMATTING_H_

// C++ 标准库头文件：
// - <cstddef> 提供 size_t 等基本类型别名（无符号整数，表示大小/索引）。
// - <optional> 提供 std::optional，可表示“可能没有值”的返回（类似 Python 的 None）。
// - <string> 字符串类型。
// - <unordered_map> 哈希表（键值对集合），类似 Python 的 dict。
// - <vector> 动态数组，类似 Python 的 list（但是定类型的、连续内存）。
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.h"

// 前向声明（forward declaration）：告诉编译器“存在一个 rime::Config 类型”，
// 但此处不需要其完整定义。可以减少编译依赖、加快编译速度。
namespace rime {
class Config;
}  // namespace rime

// 命名空间（namespace）：用来避免名字冲突。类似 Python 的模块命名空间。
// 这里使用了 C++17 的嵌套命名空间简写：rime::aipara::text_formatting。
namespace rime::aipara::text_formatting {

// enum class 是“作用域枚举”，比传统 enum 更安全：枚举值不会隐式转换为整数，
// 需要显式转换。这里描述文本片段（segment）的语义类别。
// 小贴士：Python 中通常会用字符串或 Enum 来表达类似含义。
enum class SegmentKind {
  kAbc,
  kPunct,
  kRawEnglishCombo,
};

// TextSegment 结构体：用于描述一段文本的“片段”。结构体（struct）是将多个字段
// 打包在一起的自定义类型，类似 Python 的 dataclass 或简单对象。
// 这里是插件中对 Lua segment 表（table）的 C++ 表示。
struct TextSegment {
  // 字段初始化：C++11 起可在此给出默认值。
  // kind：枚举类型，表示片段的类别；默认是 kAbc。
  SegmentKind kind = SegmentKind::kAbc;

  // type：和 kind 对应的人类可读字符串，比如 "abc"、"punct" 等。
  std::string type;       // "abc", "punct", "rawenglish_combo"

  // content：此片段经格式化后的“输出内容”（将被下游组件消费）。
  std::string content;    // formatted content that downstream code consumes

  // original：从原始输入字符串截取的未改动内容。
  std::string original;   // original slice from the input string

  // 注意：start/end/length 都是“字节”单位，而不是“字符”数量。
  // UTF-8 下，一个汉字通常 3 个字节，emoji 可能 4 个字节。
  // Python 的 len(s) 返回的是码点数量，而这里 length 是字节长度，务必区分。
  std::size_t start = 0;  // 字节索引（含）
  std::size_t end = 0;    // 字节索引（不含）
  std::size_t length = 0; // 片段字节长度（end - start）
};

// 获取/更新全局配置的函数。这里用“指针”（Config*）传参：
// - 指针可以为 nullptr，表示“无效/无对象”，调用方需要确保非空或被调方检查。
// - 在 Python 里通常直接传对象引用；C++ 用指针或引用（T&）来表达“可修改传入对象”。
void UpdateCurrentConfig(Config* config);
void SetEnglishModeSymbol(const std::string& symbol);
const std::string& english_mode_symbol();

// 供 Lua/其他模块调用的辅助函数：
// - 返回值、参数尽量使用 std::string 和 STL 容器，易于管理内存且行为明确。
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

// 文本切分与转换函数：和 Lua 模块中的功能一一对应。
// 返回 std::vector<TextSegment>：一组有序片段。
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

// 带日志版本的包装函数：相比上面多了 Logger* 参数，便于调试与排错。
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

// 搜索相关函数：在包含“英文模式符号”的文本里做跳过区域的查找。
// 注意返回类型是 std::optional<std::size_t>：
// - 有值时等价 Python 中返回索引整数；
// - 无值（std::nullopt）等价 Python 中返回 None。
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
