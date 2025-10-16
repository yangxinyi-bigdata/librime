#ifndef PLUGINS_AIPARA_SRC_CLOUD_AI_FILTER_V2_H_
#define PLUGINS_AIPARA_SRC_CLOUD_AI_FILTER_V2_H_

#include <rime/common.h>
#include <rime/filter.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rime {
class Config;
}  // namespace rime

namespace rime::aipara {

class TcpSocketSync;

struct CloudAiBehavior {
  std::string prompt_chat;
};

struct CloudResultCache {
  std::string last_input;
  std::vector<std::string> cloud_candidates;
  std::vector<std::string> ai_candidates;
  double timestamp = 0.0;
  double cache_timeout = 60.0;
};

class CloudAiFilterV2 : public Filter {
 public:
  explicit CloudAiFilterV2(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

  void UpdateCurrentConfig(Config* config);
  void AttachTcpSocketSync(TcpSocketSync* sync);

 private:
  CloudAiBehavior behavior_;
  std::unordered_map<std::string, std::string> chat_triggers_;
  std::unordered_map<std::string, std::string> chat_names_;

  std::string schema_name_;
  std::string shuru_schema_;
  int max_cloud_candidates_ = 2;
  int max_ai_candidates_ = 1;
  std::string delimiter_ = " ";
  std::string rawenglish_delimiter_before_;
  std::string rawenglish_delimiter_after_;

  CloudResultCache cache_;
  TcpSocketSync* tcp_socket_sync_ = nullptr;
};

}  // namespace rime::aipara

#endif  // PLUGINS_AIPARA_SRC_CLOUD_AI_FILTER_V2_H_
