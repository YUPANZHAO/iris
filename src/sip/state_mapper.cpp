#include "sip/state_mapper.h"

namespace iris::sip {

CallState map_call_state(pjsip_inv_state state) {
    switch (state) {
    case PJSIP_INV_STATE_CALLING:
        return CallState::Outgoing;
    case PJSIP_INV_STATE_INCOMING:
        return CallState::Incoming;
    case PJSIP_INV_STATE_EARLY:
    case PJSIP_INV_STATE_CONNECTING:
        return CallState::Early;
    case PJSIP_INV_STATE_CONFIRMED:
        return CallState::Confirmed;
    case PJSIP_INV_STATE_DISCONNECTED:
        return CallState::Disconnected;
    case PJSIP_INV_STATE_NULL:
    default:
        return CallState::Outgoing;
    }
}

} // namespace iris::sip

