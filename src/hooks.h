/* Host-call hook infrastructure (VR PLAN.md Phase 2): let a compiled patch divert a guest
 * instruction into our own x86-64 code, run a host function that can read and modify the full
 * guest CPU state, then continue the original code unchanged.
 *
 * The guest (Bloodborne's decrypted eboot) is PS4 System V AMD64 code. Host code built by clang
 * for Windows is MS x64 by default, so the host function is declared with the sysv_abi attribute
 * (ABI, below) to match the rest of the port (src/runtime.h). The generated trampoline saves the
 * full context, switches to the SysV convention, and calls BbHookFn(ctx).
 *
 * This header is the only interface src/probe.c needs. The implementation (and every ABI
 * assumption) lives in src/runtime_hooks.c, which the existing `src/runtime*.c` build globs pick
 * up automatically. */
#ifndef BB_HOOKS_H
#define BB_HOOKS_H
#include <stdint.h>
#include <stddef.h>

#ifndef ABI
#define ABI __attribute__((sysv_abi))
#endif

/* Saved guest CPU state passed to a hook. The trampoline fills this from the live registers at
 * the hook site and writes it back afterwards, so a hook may modify any field and the new value
 * takes effect in the guest (e.g. read the camera object pointer from ->rdi, write matrices into
 * guest memory, or change ->rflags to alter a following branch). The byte layout is fixed and
 * mirrored exactly by the generated trampoline; the _Static_asserts in runtime_hooks.c lock it. */
typedef struct BbHookContext {
    /* 16 general-purpose registers, in encoding order (rsp is the guest rsp at the hook site,
     * i.e. with the SysV 128-byte red zone still below it; writing rsp is not supported and is
     * ignored by the trampoline on the way out). Offsets 0x00..0x78. */
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags;      /* 0x80: RFLAGS as saved by pushfq */
    uint64_t rip;         /* 0x88: absolute address of the hook site (informational) */
    uint32_t mxcsr;       /* 0x90: SSE control/status word */
    uint32_t _pad;        /* 0x94: keep the vector area 8-byte aligned */
    /* 0x98: YMM0..YMM15, 256 bits each. The guest is AVX code (PS4 Jaguar) and the SysV ABI makes
     * all of YMM0-15 caller-saved, so the host hook function may clobber them; the trampoline
     * saves and restores the FULL 256-bit registers (VEX vmovups) to keep the guest's vector state
     * intact. ymm[n][0..15] is XMM_n (the part a hook normally reads/edits, e.g. a matrix row);
     * ymm[n][16..31] is the upper half, preserved but rarely edited. */
    uint8_t  ymm[16][32]; /* 0x98 */
} BbHookContext;

typedef void (ABI *BbHookFn)(BbHookContext *ctx);

/* Registry: resolve a hook name (as written in the patch XML / BBPATCH3 file) to a host function.
 * `len` lets probe.c pass a non-terminated slice straight from the patch file. Returns NULL when
 * the name is unknown. Only names in this built-in table can ever be installed; third-party XML
 * cannot introduce new host code. */
BbHookFn bb_hook_lookup(const char *name, size_t len);

/* Install a trampoline that diverts the `orig_len` bytes at image+site_offset into `fn`.
 *
 * `image` is the loader's mapped image base; `site_offset` the image offset of the hook site;
 * `orig_len` the number of displaced original bytes (already verified by the caller to match the
 * expected bytes and to be free of relocations). The displaced bytes must end on an instruction
 * boundary and be at least 5 bytes (a rel32 jump); 14+ bytes is recommended so the placement-
 * independent absolute jump form is used. Displaced instructions with RIP-relative operands are
 * relocated correctly; relative branches and encodings outside the supported subset are rejected.
 *
 * `replay_before` chooses when the displaced bytes run relative to the handler call (findings/08):
 *   0 (replay-after):  save context -> call fn -> restore -> run displaced -> resume. The handler
 *                      sees the register state at the site; the displaced bytes see the handler's
 *                      changes. Used by H1 (its displaced bytes do not touch the camera).
 *   1 (replay-before): run displaced -> save context -> call fn -> restore -> resume. The handler
 *                      sees the register state the displaced bytes produced (e.g. a freshly built
 *                      camera pointer). Used by H2 (its displaced bytes build the camera, and
 *                      include an indirect `call`, which replays verbatim).
 *
 * On success, if `trampoline` is non-NULL it receives the address of the generated stub (a
 * diagnostic the loader logs). Returns NULL on success, or a static human-readable error string
 * on failure (the caller aborts loudly: the user's eboot may come from a modded package, so a
 * surprise is never ignored). */
const char *bb_hook_install(unsigned char *image, uint64_t site_offset, uint64_t orig_len,
                            BbHookFn fn, int replay_before, void **trampoline);

/* Executable-memory bump allocator used for trampolines. Exposed so tests can place synthetic
 * guest code next to the trampolines (keeping RIP-relative and rel32 fixups in range). Returns
 * NULL on failure. Memory is never freed (hooks live for the whole process). */
void *bb_hook_alloc_exec(size_t size);

/* The camera hooks' handlers ("bb_hook_camera_inject" = H1, "bb_hook_camera_inject_fmtb" = H2,
 * "bb_hook_vr_lod" = the LOD fix in SetCamera): src/runtime_vr_head.c registers them at startup;
 * unregistered camera hooks do nothing. */
void bb_hook_set_camera_impl(BbHookFn h1, BbHookFn h2, BbHookFn lod);

/* First person (src/runtime_vr_fp.c) registers the game-camera heading hook's handler
 * ("bb_hook_vr_fp_cam", in the in-game step at 0x1d3be5a after the camera director); unregistered,
 * it does nothing. */
void bb_hook_set_fp_impl(BbHookFn cam);

/* "Misses are free" stamina (src/runtime_vr_stamina.c, findings/20 6.2 G3) registers the handlers of
 * "bb_hook_vr_stamina_cost" (the attack cost, 0x1e297cb), "bb_hook_vr_hit_chr" / "_map" / "_obj"
 * (the hit handlers 0x1cb79e0, 0x1cb8730, 0x1cb8430) and "bb_hook_vr_stamina_tick" (the per-frame
 * stamina update, 0x1cbdb20); unregistered, they do nothing. */
void bb_hook_set_stamina_impl(BbHookFn cost, BbHookFn hit_chr, BbHookFn hit_map, BbHookFn hit_obj,
                              BbHookFn tick);

/* Alternate-eye stereo (src/runtime_vr_stereo.c, findings/22 section 6) registers the off-axis
 * projection hooks' handlers: "bb_hook_vr_proj_setcamera" (0x25aa933, the epilogue of SetCamera's
 * builder 0x25aa6c0, out = r15) and "bb_hook_vr_proj_pass" (0x25aa69c, the epilogue of the passes'
 * builder 0x25aa290, out = r14); unregistered, they do nothing. */
void bb_hook_set_stereo_impl(BbHookFn proj_setcamera, BbHookFn proj_pass);

/* VR arms (src/runtime_vr_arms.c, findings/24) registers the AnimationToModel hooks' handlers:
 * "bb_hook_vr_arms_a1" (0x1c37650) and "bb_hook_vr_arms_a2" (0x1e19bfe), right after each copy's
 * getSyncedPoseModelSpace (rax = &modelPose); unregistered, they do nothing. */
/* Also "bb_hook_vr_arms_a3" (0x1c377c6, the i16-remap copy 0x1c37770, rax = &modelPose) and
 * "bb_hook_vr_arms_sync" (0xac4f0e, inside hkaPose::getSyncedPoseModelSpace after the sync: rbx = the
 * pose, every caller; diagnostics and the arm_at=sync edit). */
/* And "bb_hook_vr_arms_player" (0x1cf845f, the PlayerIns pose update 0x1cf83f0 right after its
 * hkaPose::syncModelSpace: r14 = the player's pose, the model pose final for the frame). */
void bb_hook_set_arms_impl(BbHookFn a1, BbHookFn a2, BbHookFn a3, BbHookFn sync, BbHookFn player);

/* The VR UI (src/runtime_vr_ui.c, findings/26) registers "bb_hook_vr_ui_menu" (0x1a5b970, the entry
 * of the HUD input handler, the game thread's frame phase where the game itself acts on menu input):
 * there it calls the game's own equip-from-menu command 0x1f63e00 for a dashboard request. */
void bb_hook_set_ui_impl(BbHookFn menu);
/* A second handler for "bb_hook_vr_fp_cam" (the in-game step 0x1d3be5a, the game thread, every frame
 * of gameplay), called after first person's: the VR UI runs its equip requests there, since the HUD
 * input handler (above) only runs while a game menu is up (findings/26). */
void bb_hook_set_step_extra(BbHookFn step);

/* VR gun aim (src/runtime_vr_gun.c, findings/27) registers "bb_hook_vr_gun_aim" (0x1df6ca0, the entry of
 * SprjBulletIns's launch init: rsi = the emitter descriptor, rdx = the target, rcx = the launch matrix,
 * [rsp] = the return address): it turns the player's shots onto the controller's ray. */
void bb_hook_set_gun_impl(BbHookFn launch);

/* VR hit scale (src/runtime_vr_hitscale.c, findings/32) registers "bb_hook_vr_hit_scale" (0x1c5fa55, in
 * the attack update 0x1c5ed50 after the capsule's radius store and before its ctor: rbx = the
 * hkpCapsuleShape, r12 = the attack entry, [rbp-0x210]/[rbp-0x220] = the local ends): it scales the
 * player's weapon capsules for the game's hit test. */
void bb_hook_set_hitscale_impl(BbHookFn shape);

/* VR attack speed (src/runtime_vr_atkspeed.c) registers "bb_hook_vr_atk_speed" (0x1cbd711 in the ChrIns
 * update 0x1cbd6a0: r13 = the ChrIns, xmm0 = the character's time step, stored next at ChrIns+0xE0): for
 * the player during an attack it scales xmm0. */
void bb_hook_set_atkspeed_impl(BbHookFn dt);

/* Test hook: increments a counter and, as a demonstration of register write-back, copies ->rdi
 * into ->rsi. Registered as "bb_hook_test_counter". bb_hook_test_counter_value() reads the count
 * (for tests). Never installed by the default patch set. */
uint64_t bb_hook_test_counter_value(void);

#endif
