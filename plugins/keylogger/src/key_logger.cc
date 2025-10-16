#include <boost/date_time/posix_time/posix_time.hpp>
#include "key_logger.h"


#include <rime/common.h>
#include <rime/key_event.h>

namespace rime{

using namespace boost::posix_time;

// struct Log{
//     ptime timestamp;
//     KeyEvent key_event;
// }

// void KeyLogger::StartLogging(){

// }

// void KeyLogger:EndLogging(){

// }

ProcessResult KeyLogger::ProcessKeyEvent(const KeyEvent& key_event) {
    if (engine_->context()->get_option("key_logger")){
        ptime now = microsec_clock::local_time();
        // LOG(INFO) << " time: " << to_iso_string(now) << " key: " << key_event;
        // logs_.push_back(Log {now, key_event});
        engine_->context()->set_property("cpp_test", "cpp_plugin");
        
    }
    return kNoop;
}
}  // namespace rime
