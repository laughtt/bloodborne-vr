/* Replays a first-person trace (BB_VR_FP_TRACE, or out/vr_fp_trace.csv with BB_VR_FP_LOG=1;
 * format: docs/vr/findings/16-first-person.md section 9.6) through the turn model the game and the
 * VR sandbox share (src/vr_turn_math.h), and compares it with what the game computed:
 *
 *     vr-turn-replay <trace.csv> [key=value ...]
 *
 * The keys are vr_turn_math.h's (turn= snap= turn_speed= deadzone= expo= turn_accel= snap_on=
 * snap_off= snap_repeat= move= recenter= lock= lock_auto=): give the settings the game ran with
 * (its control file); the trace's turn and move columns override those two on every frame.
 * Each first-person frame is checked on its own (the model starts from the trace's previous psi,
 * so rounding never accumulates) and the whole run is replayed freely (the drift is reported).
 * As in the game, the model restarts from the game camera's heading when first person comes back
 * after more than 0.5 s, and turn=game frames take psi from the game camera. The sandbox can feed a
 * real game session through the same code this way. Exit status 1 when a frame differs by more
 * than 1e-3 rad. Build: ninja -C out/gpu vr-turn-replay. */
#include "vr_turn_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { C_T, C_ACTIVE, C_HEADING, C_OWN, C_PSI, C_DT, C_STICK, C_HEAD, C_REF, C_VIEW, C_MOVE, C_DELTA, C_SYNC,
       C_LOCKED, C_TVALID, C_TYAW, C_TURN, C_MOVENAME, C_COUNT };
static const char *const names[C_COUNT] = {"t", "active", "heading", "game_own", "psi", "dt", "stick_x", "head_yaw",
                                           "ref_yaw", "view_yaw", "move_yaw", "turn_delta", "sync", "locked",
                                           "target_valid", "target_yaw", "turn", "move"};

/* Splits a CSV line in place; returns the number of fields. */
static int split(char *line, char **fields, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = 0;
        p = comma + 1;
    }
    char *end = fields[n - 1] + strcspn(fields[n - 1], "\r\n");
    *end = 0;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: vr-turn-replay <trace.csv> [key=value ...]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    VrTurnConfig base;
    vrt_config_default(&base);
    for (int i = 2; i < argc; ++i) {
        char key[64];
        const char *eq = strchr(argv[i], '=');
        if (!eq || (size_t)(eq - argv[i]) >= sizeof(key)) continue;
        memcpy(key, argv[i], (size_t)(eq - argv[i]));
        key[eq - argv[i]] = 0;
        if (!vrt_config_set(&base, key, eq + 1)) fprintf(stderr, "ignored: %s\n", argv[i]);
    }
    static char line[8192];
    char *fields[96];
    int col[C_COUNT];
    if (!fgets(line, sizeof(line), f)) return 2;
    const int nh = split(line, fields, 96);
    for (int c = 0; c < C_COUNT; ++c) {
        col[c] = -1;
        for (int i = 0; i < nh; ++i) if (!strcmp(fields[i], names[c])) col[c] = i;
        if (col[c] < 0) {
            fprintf(stderr, "%s: no column %s (an older trace?)\n", argv[1], names[c]);
            return 2;
        }
    }
    VrTurnState step, run;
    memset(&step, 0, sizeof(step));
    memset(&run, 0, sizeof(run));
    int was_active = 0, prev_sync = 0, frames = 0, bad = 0;
    double inactive_since = -1.0, worst = 0.0, worst_t = 0.0, drift = 0.0;
    float prev_psi = 0.0f;
    while (fgets(line, sizeof(line), f)) {
        const int n = split(line, fields, 96);
        if (n < nh) continue;
        double v[C_COUNT];
        for (int c = 0; c < C_COUNT; ++c) v[c] = atof(fields[col[c]]);
        const int active = (int)v[C_ACTIVE];
        if (!active) {
            if (was_active) inactive_since = v[C_T];
            was_active = 0;
            prev_sync = 0;
            prev_psi = (float)v[C_PSI];
            continue;
        }
        VrTurnConfig cfg = base;
        const int game = !strcmp(fields[col[C_TURN]], "game");
        if (!game) vrt_config_set(&cfg, "turn", fields[col[C_TURN]]);
        vrt_config_set(&cfg, "move", fields[col[C_MOVENAME]]);
        if (!was_active && (!run.valid || inactive_since < 0.0 || v[C_T] - inactive_since > 0.5)) {
            vrt_reset(&run, (float)v[C_HEADING]);
            vrt_reset(&step, (float)v[C_HEADING]);
            prev_psi = step.psi;
        }
        if (game) {
            const float own = prev_sync ? (float)v[C_OWN] : (float)v[C_HEADING];
            run.psi = step.psi = vrt_wrap(own);
            cfg.turn = VRT_TURN_OFF;
        } else {
            step.psi = prev_psi;
        }
        VrTurnInput in;
        vrt_input_clear(&in);
        in.dt = (float)v[C_DT];
        in.stick_x = (float)v[C_STICK];
        in.head_yaw = (float)v[C_HEAD];
        in.ref_yaw = (float)v[C_REF];
        in.locked = (int)v[C_LOCKED];
        in.target_valid = (int)v[C_TVALID];
        in.target_yaw = (float)v[C_TYAW];
        VrTurnOutput a, b;
        vrt_update(&step, &cfg, &in, &a);
        vrt_update(&run, &cfg, &in, &b);
        const double e[3] = {fabs(vrt_wrap(a.psi - (float)v[C_PSI])), fabs(vrt_wrap(a.view_yaw - (float)v[C_VIEW])),
                             fabs(vrt_wrap(a.move_yaw - (float)v[C_MOVE]))};
        for (int k = 0; k < 3; ++k) {
            if (e[k] > worst) { worst = e[k]; worst_t = v[C_T]; }
            if (e[k] > 1e-3) ++bad;
        }
        drift = fabs(vrt_wrap(b.psi - (float)v[C_PSI]));
        prev_psi = (float)v[C_PSI];
        prev_sync = (int)v[C_SYNC];
        was_active = 1;
        ++frames;
    }
    fclose(f);
    printf("%d first-person frames replayed: worst per-frame difference %.2g rad (t %.2f s), %d values over 1e-3 rad; "
           "free replay ends %.3g rad from the game's psi\n", frames, worst, worst_t, bad, drift);
    return bad ? 1 : 0;
}
