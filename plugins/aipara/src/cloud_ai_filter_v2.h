#ifndef PLUGINS_AIPARA_SRC_CLOUD_AI_FILTER_V2_H_
#define PLUGINS_AIPARA_SRC_CLOUD_AI_FILTER_V2_H_

#include <rime/common.h>
#include <rime/filter.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rapidjson/fwd.h>

#include "common/logger.h"

namespace rime {
class Config;
class Candidate;
class Context;
}  // namespace rime

namespace rime::aipara {

class TcpZmq;

class CloudAiFilterV2 : public Filter {
 public:
  explicit CloudAiFilterV2(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

  void UpdateCurrentConfig(Config* config);
  void AttachTcpZmq(TcpZmq* client);

 private:
  struct CandidateCache {
    std::string last_input;
    std::vector<std::string> cloud_candidates;
    std::vector<std::pair<std::string, std::string>> ai_candidates;
    double timestamp = 0.0;
    double cache_timeout = 60.0;
  };

  struct ParsedResult {
    std::vector<std::string> cloud_candidates;
    std::vector<std::pair<std::string, std::string>> ai_candidates;
  };

  CandidateCache cache_;
  TcpZmq* tcp_zmq_ = nullptr;

  Logger logger_;

  Config* ResolveConfig() const;
  void ClearCache();
  void SaveCache(const std::string& input,
                 const ParsedResult& parsed);
  std::optional<ParsedResult> GetCache(const std::string& input) const;

  ParsedResult ParseConvertResult(const rapidjson::Document& doc) const;
  std::vector<an<Candidate>> BuildCandidatesFromResult(
      const ParsedResult& result,
      const Candidate* reference,
      size_t segment_start,
      size_t segment_end,
      int max_cloud_candidates,
      int max_ai_candidates,
      bool from_cache = false) const;
  std::vector<std::string> CollectLongCandidateTexts(
      const CandidateList& originals,
      size_t segment_end) const;

  void SetCloudConvertFlag(const Candidate* candidate,
                           Context* context,
                           const std::string& delimiter) const;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_CLOUD_AI_FILTER_V2_H_
