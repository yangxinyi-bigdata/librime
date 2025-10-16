#include "cloud_ai_filter_v2.h"

#include <rime/config.h>
#include <rime/translation.h>

#include "tcp_socket_sync.h"

namespace rime::aipara {

CloudAiFilterV2::CloudAiFilterV2(const Ticket& ticket)
    : Filter(ticket) {}

an<Translation> CloudAiFilterV2::Apply(an<Translation> translation,
                                       CandidateList* candidates) {
  // TODO: merge cloud and AI candidates.
  return translation;
}

void CloudAiFilterV2::UpdateCurrentConfig(Config* config) {
  chat_triggers_.clear();
  chat_names_.clear();

  if (!config) {
    return;
  }

  config->GetString("ai_assistant/behavior/prompt_chat",
                    &behavior_.prompt_chat);
  config->GetString("schema/name", &schema_name_);
  config->GetString("schema/my_shuru_schema", &shuru_schema_);

  config->GetInt("cloud_ai_filter/max_cloud_candidates",
                 &max_cloud_candidates_);
  config->GetInt("cloud_ai_filter/max_ai_candidates", &max_ai_candidates_);

  std::string delimiter;
  if (config->GetString("speller/delimiter", &delimiter) && !delimiter.empty()) {
    delimiter_ = delimiter.substr(0, 1);
  } else {
    delimiter_ = " ";
  }

  config->GetString("cloud_ai_filter/rawenglish_delimiter_before",
                    &rawenglish_delimiter_before_);
  config->GetString("cloud_ai_filter/rawenglish_delimiter_after",
                    &rawenglish_delimiter_after_);

  cache_.cloud_candidates.clear();
  cache_.ai_candidates.clear();
  cache_.last_input.clear();
  cache_.timestamp = 0.0;
}

void CloudAiFilterV2::AttachTcpSocketSync(TcpSocketSync* sync) {
  tcp_socket_sync_ = sync;
}

}  // namespace rime::aipara
