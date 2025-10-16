#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "ai_assistant_segmentor.h"

using namespace rime;

static void rime_ai_assistant_segmentor_initialize() {
  Registry& registry = Registry::instance();
  registry.Register("ai_assistant_segmentor",
                    new Component<AiAssistantSegmentor>);
}

static void rime_ai_assistant_segmentor_finalize() {
}

RIME_REGISTER_MODULE(ai_assistant_segmentor)
