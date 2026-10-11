// Unit test of the NP signaling reuse decision (np_signaling_reuse.h).
#include <cstdio>

#include "core/libraries/np/np_signaling/np_signaling_reuse.h"

using namespace Libraries::Np::NpSignaling;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

static ConnView V(bool exists, bool inactive, bool established, bool dead, bool lingering) {
    ConnView v;
    v.exists = exists;
    v.inactive = inactive;
    v.established = established;
    v.dead_fired = dead;
    v.lingering = lingering;
    return v;
}

int main() {
    // no connection at all
    CHECK(DecideReuse(V(false, false, false, false, false)) == ReuseAction::CreateFresh);
    CHECK(!AnswerOfferWhileEstablished(V(false, false, true, false, false)));
    // inactive (also with stale flags)
    CHECK(DecideReuse(V(true, true, false, false, false)) == ReuseAction::CreateFresh);
    CHECK(DecideReuse(V(true, true, true, false, true)) == ReuseAction::CreateFresh);
    // dead
    CHECK(DecideReuse(V(true, false, true, true, true)) == ReuseAction::CreateFresh);
    CHECK(DecideReuse(V(true, false, false, true, false)) == ReuseAction::CreateFresh);
    CHECK(!AnswerOfferWhileEstablished(V(true, false, true, true, false)));
    // established and active: reuse in place, answer a restarted peer
    CHECK(DecideReuse(V(true, false, true, false, false)) == ReuseAction::ReuseExisting);
    CHECK(AnswerOfferWhileEstablished(V(true, false, true, false, false)));
    // established and lingering: retire, do not answer an Offer
    CHECK(DecideReuse(V(true, false, true, false, true)) == ReuseAction::RetireThenCreateFresh);
    CHECK(!AnswerOfferWhileEstablished(V(true, false, true, false, true)));
    // handshaking (not established): reused, never answered as established; the lingering flag
    // alone (stale) does not retire a connection that is not established
    CHECK(DecideReuse(V(true, false, false, false, false)) == ReuseAction::ReuseExisting);
    CHECK(DecideReuse(V(true, false, false, false, true)) == ReuseAction::ReuseExisting);
    CHECK(!AnswerOfferWhileEstablished(V(true, false, false, false, false)));
    CHECK(!AnswerOfferWhileEstablished(V(true, false, false, false, true)));
    // default-constructed view is "does not exist"
    CHECK(DecideReuse(ConnView{}) == ReuseAction::CreateFresh);
    CHECK(!AnswerOfferWhileEstablished(ConnView{}));

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
