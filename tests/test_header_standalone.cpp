// PLAN.md Stage 1 step 1 verify clause: the message header compiles as a standalone
// translation unit -- this file includes impact/message.hpp and nothing else.
#include "impact/message.hpp"

int impact_message_header_standalone_anchor() {
    return static_cast<int>(impact::MsgType::SnapshotReset);
}
