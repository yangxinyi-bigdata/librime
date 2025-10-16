#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "key_logger.h"

using namespace rime;

static void rime_keylogger_initialize() {
  Registry &r = Registry::instance();
  r.Register("key_logger", new Component<KeyLogger>);
}

static void rime_keylogger_finalize() {
}

RIME_REGISTER_MODULE(keylogger)
