// SPDX-License-Identifier: GPL-3.0-or-later
#include "m0.hpp"

namespace hackrfduplex {

bool m0_took_up_start(const M0Reading& before, const M0Reading& now) {
    if (!m0_idle(now)) {
        return true;
    }
    // Idle and unchanged: still the stream before. Idle on other counts: the
    // start reset them and the M0 has stopped itself since.
    return now.bytes != before.bytes || now.shortfalls != before.shortfalls ||
           now.error != before.error;
}

bool m0_idle(const M0Reading& now) { return now.active_mode == kM0ModeIdle; }

}  // namespace hackrfduplex
