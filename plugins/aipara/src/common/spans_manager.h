#ifndef PLUGINS_AIPARA_SRC_COMMON_SPANS_MANAGER_H_
#define PLUGINS_AIPARA_SRC_COMMON_SPANS_MANAGER_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "logger.h"

namespace rime {
class Candidate;
class Context;
}  // namespace rime

namespace rime::aipara::spans_manager {

struct SpansInfo {
  std::string vertices_str;
  std::vector<std::size_t> vertices;
  std::string input;
  std::string source;
  std::string timestamp;
};

bool SaveSpans(Context* context,
               const std::vector<std::size_t>& vertices,
               const std::string& input,
               const std::string& source = "unknown",
               Logger* logger = nullptr);

std::optional<SpansInfo> GetSpans(Context* context);

std::vector<std::size_t> ParseVerticesString(const std::string& vertices_str);

void ClearSpans(Context* context,
                const std::string& reason = "未指定原因",
                Logger* logger = nullptr);

std::pair<bool, std::string> ShouldClear(
    Context* context,
    const std::optional<std::string>& current_input = std::nullopt);

bool AutoClearCheck(Context* context,
                    const std::optional<std::string>& current_input = std::nullopt,
                    Logger* logger = nullptr);

bool ExtractAndSaveFromCandidate(Context* context,
                                 const Candidate* candidate,
                                 const std::string& input,
                                 const std::string& source,
                                 Logger* logger = nullptr);

std::optional<std::size_t> GetNextCursorPosition(Context* context,
                                                  std::size_t current_pos);

std::optional<std::size_t> GetPrevCursorPosition(Context* context,
                                                  std::size_t current_pos);

void DebugInfo(Context* context, Logger* logger = nullptr);

}  // namespace rime::aipara::spans_manager

#endif  // PLUGINS_AIPARA_SRC_COMMON_SPANS_MANAGER_H_
