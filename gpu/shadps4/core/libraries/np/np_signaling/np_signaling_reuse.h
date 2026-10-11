// Decision logic for a signaling connection that already exists when a new activation arrives
// (pure and standalone: no project includes, so a unit test can include it).
#pragma once

#include <cstdint>

namespace Libraries::Np::NpSignaling {

struct ConnView {
    bool exists = false;
    bool inactive = false;
    bool established = false;
    bool dead_fired = false;
    bool lingering = false;
};

enum class ReuseAction {
    CreateFresh,
    RetireThenCreateFresh,
    ReuseExisting,
};

// A lingering (locally deactivated) connection is never revived in place: it is retired and a
// fresh connection runs the whole first-summon path (fresh flags, events, keepalive, timeouts).
inline ReuseAction DecideReuse(const ConnView& c) {
    if (!c.exists || c.inactive || c.dead_fired) {
        return ReuseAction::CreateFresh;
    }
    if (c.established && c.lingering) {
        return ReuseAction::RetireThenCreateFresh;
    }
    return ReuseAction::ReuseExisting;
}

// An Offer arriving while we are Established (the peer restarted its side) is still answered with
// an Accept; our state is unchanged. A lingering connection is not answered (it gets retired by the
// ActivationRequest that precedes the peer's new handshake).
inline bool AnswerOfferWhileEstablished(const ConnView& c) {
    return c.exists && c.established && !c.lingering && !c.dead_fired;
}

} // namespace Libraries::Np::NpSignaling
