#pragma once

#include "iris/types.h"

#include <pjsip-ua/sip_inv.h>

namespace iris::sip {

CallState map_call_state(pjsip_inv_state state);

} // namespace iris::sip

