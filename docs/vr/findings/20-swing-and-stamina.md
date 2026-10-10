# 20: Swing speed picks the attack, stamina is paid on hit

Date: 2026-10-09. Track: VR melee on top of the game's own attacks (doc 14 option A/B): a real swing of the
right controller presses R1 (slow) or R2 (fast); stamina is spent only by swings that connect; the dash
(dodge, quickstep, backstep) costs stamina; at 0 stamina nothing starts.

Inputs:
- The user's dump `C:/games/CUSA03173` (read only): `gameparam.parambnd.dcx`, `paramdef.paramdefbnd.dcx`,
  `chr/c0000.anibnd.dcx` (the 80 player TAE files).
- The prepared eboot (`out/eboot.elf`, VA = file offset + 0x3FC000) and the relocated image and xref index
  from 08 (`scratchpad/re/`), queried with capstone. Scratch scripts: `scratchpad/stam/` (not in the repo).
- `out/vr_assets/hitbox/` (doc 14) for per-weapon costs and windows; docs 13, 14, 16, 17; the working tree
  (`gpu/xr_bridge/`, `sandbox3d/`, `src/runtime_*.c`, `patches/Bloodborne.xml`).

Outputs (nothing committed, nothing run against the game):
- `gpu/xr_bridge/swing_logic.h` (new, MIT, header-only, no OpenXR): `SwingClassifier`, `AttackScheduler`,
  `StaminaModel`.
- `gpu/xr_bridge/swing_logic_test.cpp` (new) and one target in `gpu/xr_bridge/CMakeLists.txt`:
  `out/xr-bridge/bbport_xr_swing_test.exe`, **88 checks, 0 failed** (113 since the sandbox
  integration, 6.3).
- `tools/vr_stamina_probe.py` (new, read-only live probe; `--check-elf` and `--self-test` pass offline).

Legend (as 13/14): **[C]** confirmed here from the eboot or the game data. **[M]** measured in the running
game (by another track). **[L]** likely. **[S]** speculation or a design target. Addresses are PS4 VAs, the
patch-XML space (ELF vaddr + 0x400000), as in 08/13/14.

---

## 0. Summary

1. **Stamina lives in the player's `SprjChrDataModule` [C].**
   `stamina = [[[[0x593E878]+0x60]+0x3B0]+0x20]+0x134` (s32), maximum at `+0x138` (s32). Same module: HP
   `+0xF8`/`+0xFC`, debug flags `+0x200` (bit 0x80 "No Stamina Consume"). The player pointer is [M] (doc 16);
   the module's vtable is 0x5735810 (`SprjChrDataModule`, FD4 runtime class).
2. **Every writer clamps to [-50, max] [C]**: stamina can go negative, down to -50, never below. The debug
   prints show it as `Stamina : [%d / %d]`.
3. **Attack cost = `BehaviorParam_PC.stamina` (+0x14) x the product of active SpEffects'
   `consumeStaminaRate` (+0x1B8) [C].** Not EquipParamWeapon (`attackBaseStamina` is stamina *damage*), not
   AtkParam (`atkStam` is stamina damage too). Saw Cleaver: R1 20, R2 tap 35, charged R2 50, form-B R1 24.
4. **It is deducted when the attack's TAE window opens, not at the press [C code, L timing].** The attack
   event handler 0x1e267a0 lets `RequestBehavior` 0x1cc7530 build a request only when the event's "begin"
   byte is set; the request carries `+0x78 = -cost`, and 0x1cb61c0 applies it at **0x1cb644d** (`mov
   eax,[r14+0x78]` ... clamp ... `mov [r15+0x134],eax`). Saw Cleaver R1: the cost should land 0.37 s into
   `a112_030000`; the probe (section 7) measures it.
5. **Regen [C]:** points/s = `pct x 0.01 x ChrIns+0x16C x (45 + sum SpEffect.staminaRecoverChangeSpeed)`, base
   **45.0** at 0x5528318, in `ChrIns` vfunc 0x1cbdb20 (called every frame from `PlayerIns` 0x1cf4ff0). It
   stops when (a) TAE event **225 `SetSPRegenRatePercent`** sets `pct` to 0 (every attack until its move-cancel
   point, ~0.7-1.5 s; every dodge until ~0.8-0.9 s) or (b) the behaviour script's act **41 "スタミナ回復不可を
   設定"** ("set stamina recovery disabled") sets bit 1 of `[[ChrIns+0x58]+0x38]+0x5C`.
6. **Dodges are charged by the behaviour script [C mechanism, L that dodges use it]:** HKS act **36
   "スタミナ追加"** ("add stamina", int argument) at 0x1e11ce7 with the same clamp; the rolling behaviors in
   BehaviorParam_PC cost 0. The HKS also reads stamina (env 30 "スタミナ取得") to gate actions. So the game
   already charges the dash and refuses it at 0 **[L]**; the probe procedure (section 7) confirms both.
7. **Hits are dispatched by 0x1c5b200 [C]**: one call per hit result; a character attacker (type 1) hitting a
   character goes to **0x1cb79e0** `(rdi = hit record, rsi = victim, rdx = attacker)`, the world to 0x1cb8730,
   an object to 0x1cb8430. The record carries the AtkParam id (`+0x3C`) and isPc (`+0x40`).
8. **Recommended "misses are free": defer [C hook sites, L behaviour]:** hook 0x1cb644d (replay-after), and for
   the player's attack behaviors move the cost out of the request (`[r14+0x78] = 0`) into a ticket keyed by the
   AtkParam id; charge it at the first hit (0x1cb79e0 / 0x1cb8730 entry with `rdx == player`), drop it when the
   window ends. No HUD flicker, no refund race, the game's own gates stay intact. Refund is the fallback.
9. **Classifier [tested]:** a probe point 0.25 m along the blade; head-relative velocity; a body-turn debit;
   onset 1.3 m/s, light >= 2.2 m/s, heavy >= 4.6 m/s (decided the moment it is reached), min arc 0.2 m,
   wind-ups (rising strokes) ignored, a held raised wind-up charges R2. Detection: heavy 56-144 ms after the
   hand starts moving, light ~100 ms after onset (210-245 ms after a slow start). The scheduler queues a swing
   that comes before the game's input window (`ChrActionFlag 87`) and fires it when the window opens.

---

## 1. Stamina in memory (question A)

### 1.1 Where it is [C]

```
W   = [0x593E878]                WorldChrMan (FD4 singleton)
P   = [W + 0x60]                 PlayerIns, vtable 0x578F310 ([M] doc 16)
bag = [P + 0x3B0]                module bag (13 3.1)
D   = [bag + 0x20]               SprjChrDataModule, vtable 0x5735810, ctor 0x1e560b0, [D+8] = P
```

| Field | Offset | Type | Evidence |
|---|---|---|---|
| HP / max HP | D+0xF8 / +0xFC | s32 | debug print 0x1e57f20: `"HP : [%d / %d] ..."` reads +0xF8/+0xFC; ctor sets both to 1 |
| MP / max MP | D+0x128 / +0x12C | s32 | same printer, `"MP : [%d / %d]"` |
| **Stamina / max stamina** | **D+0x134 / +0x138** | **s32** | same printer: `"Stamina : [%d / %d] (%0.2f％)"` at 0x1e58135 reads `+0x134`, `+0x138` |
| Debug flags | D+0x200 | u16 | printer: bit 0x4 No Dead, 0x8 No Damage, 0x10 No Hit, 0x20 No Attack, 0x40 No Move, **0x80 No Stamina Consume**, 0x100 No MP Consume, 0x200 No Goods Consume, 0x400 No Update |
| "All No Stamina Consume" | byte 0x593E88A | u8 | WorldChrManDbg menu 0x1d20510 (`"All No Stamina Consume 【%s】"`); every stamina writer tests it |
| Regen points/s (last computed) | P+0x39C | s32 | written by 0x1cbdb20; printed by 0x1ccf620 as `"[Stamina Recover (point/s)] %d"`; stale while the block bit is set |
| Regen remainder | P+0x168 | f32 | fractional part carried between frames (0x1cbdc28-0x1cbdc3c) |
| Regen multiplier | P+0x16C | f32 | 1.0 (set to 1.0 in 0x1cb9ae0, the module creation, and in the `PlayerIns` update 0x1cf4ff0) |
| Frame time | P+0xE0 | f32 | the regen's dt (0x1cbdb42) |
| Regen percent | `[[P+0x58]+0x38]+0x34` | u8 | written by TAE event 225 (0x1e2220c); reset to 100 (`mov word [x+0x34],0x6464` in 0x190a480/0x1911bd0, the character controller; every frame [L]) |
| Regen blocked | `[[P+0x58]+0x38]+0x5C` bit 1 | u16 | set/cleared by HKS act 41 (0x1e11e35 → 0x1e12370 `and ecx,0xfffd`), tested at 0x1cbdbcc |

The player's maximum stamina comes from Endurance through the stats (CharaInitParam has no stamina for the
player classes); read it live from +0x138. The live value is **not** in the player's game data: `[[0x593B130]+8]`
(the GameDataMan-style singleton's player entry [L], also what `PlayerIns` vfunc +0x1C8 returns) is what the
attack gate compares for its `+0x84` counter and bullets (1.4); none of the writers in 1.2 touches it.

### 1.2 Who changes it [C]

Every writer has the same tail: skip when the change is negative and the debug flag (D+0x200 bit 0x80) or
the global 0x593E88A is set; then `x < -50 ? -50 : min(x, max)` into D+0x134.

| Writer | What | Where the amount comes from |
|---|---|---|
| **0x1cb61c0 at 0x1cb644d** | attack behaviors (TAE `InvokeAttackBehavior` → `RequestBehavior` 0x1cc7530) | request `+0x78` = `-(int)(BehaviorParam.stamina x prod consumeStaminaRate)` built at 0x1cc765c-0x1cc7733 |
| 0x1e293b0 (0x1e297e6) | bullet behaviors with a cost (Simon's Bowblade shots, Kos Parasite, a few others; guns cost 0) | `PlayerIns` vfunc +0x200 = 0x1d03020: BehaviorParam_PC stamina x the same rates |
| 0x1e29890 (0x1e29b1d) | a TAE-driven bullet aimed at the throw module's target (dispatcher type 0x89) [L] | same vfunc +0x200 |
| **0x1e11ce7** (HKS act 36 "スタミナ追加") | the behaviour script: dodges, backsteps, sprint **[L]** | the script's integer argument |
| **0x1cbdb20** | regen, every frame | section 1.3 |
| 0x1d15320 (a WorldChrMan debug damage), 0x1a9a810, 0x2013d00, 0x224b830, 0x19bed10, 0x18f54c0, 0x1cc9d90, 0x1cfd460, 0x1e2a770 | not followed; likely SpEffect `changeStaminaPoint`/`Rate`, guard stamina damage, items **[S]** | — |
| 0x1e57190 / 0x1e56ae0 | out-of-line `AddStamina(delta)` / `SetStamina(value)` (no callers: inlined everywhere) | — |

### 1.3 Regen and when it stops [C]

0x1cbdb20 (rbx = ChrIns; the player's 0x1cf4ff0 calls it at 0x1cf6ba3):

```
flags = [[rbx+0x58]+0x38]
if (!(flags[+0x5C] & 2)) {                                    // HKS act 41 blocks it
    rate = flags[+0x34] * 0.01 * rbx[+0x16C] * vfunc_0x210()  // vfunc: 45.0 (0x5528318) + sum SpEffect +0xB8
    rbx[+0x39C] = (int)rate
    acc = rate * dt(rbx[+0xE0]) + rbx[+0x168];  add = (int)acc;  rbx[+0x168] = acc - add
    stamina = clamp(stamina + add, -50, max)
}
```

- **Base 45 points/s** (`PlayerIns` vfunc +0x210 = 0x1d03430; the debug menu 0x1d04da0 edits the same float as
  "スタミナ基準回復速度 Stamina Recover Base Vellocity"). SpEffectParam `staminaRecoverChangeSpeed` (+0xB8) adds.
- **TAE event 225 `SetSPRegenRatePercent` {u8}** (DSAnimStudio BB template) overrides the percent for its
  interval. In the player's TAE files it is 0 in every attack and dodge, from 0 to the animation's move cancel:

  | Animation | Regen 0 % until |
  |---|---|
  | Saw Cleaver R1 #1 `a112_030000` | 0.73 s |
  | Hunter Axe R1 `a103_030000` / Logarius R1 `a110_030000` | 1.07 s / 1.53 s |
  | R2 charge hold `a1xx_032600` | 2.63 s (the whole hold) |
  | Quickstep `a00_007100..7107` / backstep `a00_007000` | 0.80 s |
  | a00 10000s (20 %), a00 1000/2000/000001 (50 %) | partial rates in other states (guarding?) [L] |

- **HKS act 41** sets the block bit (the regen block is skipped entirely, so `+0x39C` keeps its last value).
  Which states use it (sprint is the usual suspect) is open; the probe shows it live (section 7).

### 1.4 The attack cost: what, where, when [C]

**What.** `RequestBehavior` 0x1cc7530 looks up the BehaviorParam row (0x23133c0), then:

```
0x1cc766a  xmm0 = (float)row.stamina(+0x14)
0x1cc76c0  for each active SpEffect (list [ChrIns+0x1C8]+8, next +0x58, skipping flags 0x800c0003):
               if condition(0x1beae90) && row'.consumeStaminaRate(+0x1B8) != 1: rate *= it
0x1cc772d  request+0x78 = -(int)(stamina * rate)        (request = rbp-0xC0)
```

BehaviorParam layout (the game's own paramdef): `refType` u8 +0x09 (0 attack, 1 bullet, 2 SpEffect), `refId`
s32 +0x0C (the AtkParam id), `stamina` s32 +0x14, `mp` s32 +0x18 (Quicksilver Bullets), `category` u8 +0x1C,
`heroPoint` u8 +0x1D. EquipParamWeapon has no attack cost; `attackBaseStamina` (+0xCC) and AtkParam `atkStam`
(+0x58) are stamina **damage** dealt to the target's guard.

**Costs** (BehaviorParam_PC via `out/vr_assets/hitbox/wp_*.json`; judge 0 = R1 #1, 150 = R2 tap, 170 = charged
R2, 400 = form-B R1 #1, 390/790 = transform attacks):

| Weapon | R1 | R2 tap | R2 charged | B R1 | A→B / B→A |
|---|---|---|---|---|---|
| Saw Cleaver | 20 | 35 | 50 | 24 | 23 / 30 |
| Saw Spear | 20 | 35 | 50 | 25 | 23 / 30 |
| Hunter Axe | 25 | 45 | 75 | 27 | 30 / 30 |
| Threaded Cane | 17 | 30 | 45 | 17 | 25 / 25 |
| Kirkhammer | 16 | 55 | 75 | 30 | 40 / 20 |
| Ludwig's Holy Blade | 16 | 55 | 75 | 36 | 30 / 30 |
| Blade of Mercy | 15 | 30 | 50 | 11 | 15 / 15 |
| Chikage | 23 | 40 | 60 | 25 | 20 / 35 |
| Beast Cutter | 25 | 40 | 70 | 30 | 45 / 50 |
| Holy Moonlight Sword | 25 | 40 | 60 | 35 | 40 / 60 |
| Logarius' Wheel | 30 | 45 | 65 | 32 | 30 / 0 |
| Bare Fists | 15 | 30 | 45 | 20 | — |

**When.** The TAE dispatcher 0x1e249d0 sends the attack event to 0x1e267a0, which reads a byte of the
dispatched event (`[rsi+0x18]`, 0x1e267ff; the same byte gates the "on enter" actions of ChrActionFlag events,
so it is the event's begin flag **[L]**) and passes it as `RequestBehavior`'s first stack argument;
`RequestBehavior` builds and executes the request only when it is set (0x1cc75ac `test cl,cl; je`), and
otherwise only marks the behavior active (`ChrIns+0x188 = 1`; the request queue is `ChrIns+0x178`). So the
cost is taken **once, when the attack window opens** (Saw Cleaver R1: 0.367 s into `a112_030000`; the press is
~1-2 game frames before the animation starts), and the window then stays open for the event's length.

**No stamina gate at this level [C].** Before requesting, 0x1e267a0 checks hero points (`row+0x1D` against the
game data's `+0x84`) and bullets (`row.mp` against vfunc +0x3B8), never stamina. Starting an attack at <= 0 is
refused earlier, by the behaviour script (it reads stamina through env 30 "スタミナ取得", table entry 0x57349E8)
**[L]**. Once an attack is running, its cost is always taken and may push stamina below 0 (to -50).

### 1.5 Hook sites (BBPATCH3 host calls; all verified position-independent, no branch into the window)

| Purpose | Site | Expect (displaced bytes) | Replay | Registers at the site |
|---|---|---|---|---|
| **Attack cost (defer/refund)** | `0x01cb644d` | `418b4678498b8c24b00300004c8b7920` (16) | after | r14 = request (`+0x08` behavior id, `+0x78` stamina delta), r12 = ChrIns, `[rbp-0x48]` = BehaviorParam row |
| **Hit: character** | `0x01cb79e0` | `554889e54157415641554154534881ec58040000` (20) | after | rdi = hit record (`+0x3C` AtkParam id, `+0x40` isPc), rsi = victim, rdx = attacker |
| Hit: world (clang) | `0x01cb8730` | `554889e54157415641554154534883ec28` (17) | after | rdi = record, rsi = map hit, rdx = attacker |
| Hit: object | `0x01cb8430` | `554889e54157415641554154534883ec58` (17) | after | rdi = record, rsi = object, rdx = attacker |
| Per-frame player tick (refunds, publishing) | `0x01cbdb20` | `554889e54157415641554154534883ec68` (17) | after | rdi = ChrIns (filter `== player`), before this frame's regen |
| Attack start (logging, the game's timing) | `0x01cc7530` | `554889e54157415641554154534881ecc8000000` (20) | after | rdi = ChrIns, esi = behavior id, `[rsp+8]` = begin flag |

Proposed patch entries (disabled by default, like the camera hooks):

```xml
<Metadata Title="Bloodborne" Name="VR Stamina Cost (behavior)" Note="Doc 20: misses are free; the player's attack cost is deferred to the first hit (BB_VR_STAMINA=defer) or refunded (=refund). Replay-after." Author="bloodborne_vr" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="false">
    <PatchList>
        <Line Type="hook" Address="0x01cb644d" Value="bb_hook_vr_stamina_cost" Expect="418b4678498b8c24b00300004c8b7920" Replay="after" />
    </PatchList>
</Metadata>
<Metadata Title="Bloodborne" Name="VR Stamina Hits" Note="Doc 20: the player's attack connected (character, world, object). Replay-after." Author="bloodborne_vr" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="false">
    <PatchList>
        <Line Type="hook" Address="0x01cb79e0" Value="bb_hook_vr_hit_chr" Expect="554889e54157415641554154534881ec58040000" Replay="after" />
        <Line Type="hook" Address="0x01cb8730" Value="bb_hook_vr_hit_map" Expect="554889e54157415641554154534883ec28" Replay="after" />
        <Line Type="hook" Address="0x01cb8430" Value="bb_hook_vr_hit_obj" Expect="554889e54157415641554154534883ec58" Replay="after" />
    </PatchList>
</Metadata>
```

---

## 2. Detecting a hit [C]

**The dispatcher.** 0x1c5b200 (called from the hit processor 0x1c60fb0) takes one hit record: `A = [rec+0x1E0]`
(the source), `B = [rec+0x1E8]` (what it touched), each with a virtual `GetType` (+0x18; `ChrIns` returns 1,
0x1cd6ff0). The two jump tables (0x1c5b3d8, 0x1c5b3e8) give:

| Source A | Target B | Handler |
|---|---|---|
| 1 character | 1 character | **0x1cb79e0** (damage: checks the victim's "No Hit" flag D+0x200 bit 0x10 and 0x593E892, reads AtkParam `[rec+0x3C]`, isPc `[rec+0x40]`) |
| 1 character | 0, the world [L] | 0x1cb8730 (reads the hit's material, calls the attacker's bag+0x98 module: the clang) |
| 1 character | 2, objects [L] | 0x1cb8430 |
| 1 character | 3 | 0x1cb8940 |
| 2, bullets [L] | 0 / 1 / 2 | 0x1c83770 (marks the bullet as having hit) / 0x199ec60 / 0x199ee20 |
| 3 | 0 / 1 / 2 | 0x1ded7d0 / 0x1ded020 / 0x1ded3d0 |

Type 1 = character is [C] (`ChrIns::GetType`); the other type names are inferred from the handlers.

- **One event per target per attack [L]:** FromSoft's per-attack hit list (doc 14 1.1 step 7). A 3-enemy sweep
  gives three events with the same AtkParam id: charge once.
- **Which attack:** match `[rec+0x3C]` (AtkParam id) to the ticket opened at 0x1cb644d (`row.refId`, row
  +0x0C). Multi-event attacks (one animation with two type-1 events) open two tickets with their own costs.
- **Weaker signals, not used:** the game vibrates the pad on hitstop (WorldChrManDbg "ヒットストップ振動ID";
  `scePadSetVibration` reaches `pad_vibration` in `runtime_pad.c`) but also when the player is hit; the
  ActionFlag module (bag+0x08, vtable 0x57342C0) holds "whiff possible" (HKS act 35 sets `+0x1B0` bit 8; TAE
  flag 42) for the game's own miss transitions, which needs more RE than the hit hooks.
- **Open:** whether 0x1cb79e0 also runs for a target in i-frames (the shape test may skip them earlier), and
  whether parried/guarded hits come through it (they should: they still connect). The probe procedure
  measures both (section 7).

---

## 3. "Misses are free" (question B)

### 3.1 The options

| | How | Pros | Cons |
|---|---|---|---|
| **(a) Refund** | Let 0x1cb644d deduct. Open a ticket (cost, AtkParam id, window end). A hit closes it. At the window end with no hit, add the cost back in the per-frame player tick (0x1cbdb20 entry, same thread as the game's own writes) | Game logic untouched while the hook is absent; the deduction keeps the vanilla order (an exhausted player cannot chain one more attack because of a late hit) | The bar visibly dips and refills on every miss (~0.1-0.3 s); while the refund is pending, stamina may sit at <= 0 and a buffered dodge in that gap is refused; needs a third hook site and a write outside the deduction |
| **(b) Defer** (recommended) | At 0x1cb644d, for the player's attack behaviors (`row.refType == 0`, cost > 0): set `[r14+0x78] = 0` (the game then adds 0) and open the ticket. At the first hit event for that AtkParam id, subtract the cost with the game's own clamp and debug-flag checks. Drop the ticket when the window ends | Misses never touch stamina (no flicker); two hook kinds; the charge happens in the hit handler on the game's thread; the -50 floor and the HKS gate keep working | A hit that lands late (end of a long window) charges after the next input was already accepted, so a deferred player can sometimes start one more attack than vanilla at very low stamina; a hit path we did not hook (e.g. type-3 targets) would be free |
| **(c) Bridge only** | Gate and time presses in `xr_input.cpp`; no game memory | No RE risk | **Not possible:** the cost is deducted inside the game at the window start and the bridge never sees hits. Without memory access it cannot refund, defer or even know stamina |

Bullet behaviors (the few with a cost) and throws are charged by other writers (1.2) and stay vanilla.

### 3.2 Low stamina, as the game does it [C/L]

- The game lets an action **start** while stamina > 0 **[L]** (the HKS gate), and the cost is then taken
  whole: stamina goes negative, clamped at **-50** **[C]**. Regen starts from there after the animation's
  regen-0 interval, so a big negative costs time.
- At <= 0 the HKS refuses attacks and dodges **[L]**; our synthesized presses must not be buffered across that
  moment, or an attack fires late when regen brings stamina back above 0. So the bridge gates them:
  `AttackScheduler::Update(..., can_act)` drops a swing with `Drop::Tired` (haptic "tired" buzz) when
  stamina <= 0, and the same for the dodge press.
- The gate uses the game's current stamina, not stamina minus the pending tickets (the next combo input opens
  before the current window closes, section 4.4).
- **Getting stamina to the bridge:** the bridge has no game memory. The host reads `D+0x134/+0x138` in the pad
  sampler (`runtime_pad.c` `sample_vr`, on the thread that polls the pad **[L]**: the game's) or in the
  per-frame player tick, and hands it over
  (planned ABI 6 `SetGameState`, section 6). Until then, gating can be done in `runtime_pad.c` itself: drop
  `BBXR_BTN_R1|R2|CIRCLE` from the VR sample when stamina <= 0.

---

## 4. Swing classification (question C)

### 4.1 Signals

`swing::SwingClassifier` (`swing_logic.h`) per frame, from the bridge's right grip pose (72-90 Hz) and the
head pose:
- **Probe point** 0.25 m along the grip's -Z (OpenXR: little finger → thumb, where a held blade points). Its
  velocity mixes arm travel and wrist turn: a wrist flick alone barely moves the grip but does move a blade.
  The probe is weapon independent on purpose: the controller weighs the same whatever the game shows.
- **Velocity:** differentiated probe positions, or the runtime's linear + angular velocity (`v + w x r`) when
  given.
- **Head compensation:** the head's linear velocity (EMA) is subtracted, so walking, crouching and leaning do
  not count; a **turn debit** of `|yaw rate| x horizontal head-probe distance` is subtracted from the speed, so
  a body turn that carries the hand around does not count.
- **Speed** (EMA 0.6), **acceleration**, **arc** (probe path length since onset), direction.

### 4.2 The state machine and its defaults

| Phase | Rule |
|---|---|
| Armed | speed >= `onset_speed` (1.3 m/s) → Swing. Raised and still (below) → Charging |
| Swing | **heavy** the moment the peak >= `heavy_speed` (4.6) with arc >= 0.1 m; **light** when peak >= `light_speed` (2.2) and arc >= `min_arc` (0.20 m) and either `decide_s` (90 ms) passed since onset or the speed fell below 0.7 x peak; **dies** (no attack) after `max_swing_s` (0.45 s) or below `rearm_speed`. A stroke whose direction rises more than `up_limit` (y > 0.7) when it first reaches light speed is a **wind-up**: never an attack |
| Rearm | after an attack (or a dead stroke): speed < 0.9 m/s for 40 ms → Armed; or a stroke back the other way (cos < -0.3 against the follow-through's latest fast direction, speed >= onset) starts the next swing at once (forehand → backhand) |
| Charging | entered when the hand is at or above head height - 0.10 m, not more than 0.05 m in front of the face plane, and slower than 0.8 m/s for 0.35 s: **ChargeStart** (hold R2). The next swing out of it (same light/heavy rules) is **ChargeRelease**; lowered without a swing for 0.25 s, or 2.5 s held: ChargeRelease |
| any | a probe jump > 0.30 m in one sample or > 15 m/s, a tracking loss, or a > 100 ms gap: forget the swing, wait for calm |

All of it is in `SwingTuning` (hot-reloadable in the sandbox, section 6).

**The follow-through (`reverse_follow`, added with the sandbox, 6.3) [tested].** "Back the other
way" first compared the stroke with the direction at the moment the attack was decided. A heavy is
decided early in its arc, and a long circular arc keeps turning: past 107° from that direction
it counted as a stroke back, so one slash became two attacks. On synthetic one-way slashes of 90°
to 150° in 0.22 to 0.55 s (quick class, 2 mm noise, 4 seeds), 41 of 64 were one attack and 23 were
two. While the hand still moves at onset speed or faster the reference now follows the stroke's
own direction: a smooth arc never turns back against it, and a real turnaround (the hand slows,
the direction flips) still does. Result: 64 of 64 one attack, forehand then backhand still two
(12 of 12). `reverse_follow: false` restores the old rule.

### 4.3 Weapon classes and trick forms

`TuningFor(WeaponClass)` gives starting points; the class follows the **current form** (EquipParamWeapon id +
the form from `[P+0x4B8]+0x08`, doc 13 7.2):

| Class | Weapons / forms | light / heavy (m/s) | min arc | decide |
|---|---|---|---|---|
| Quick | Saw Cleaver A, Threaded Cane A, Blade of Mercy A/B, Chikage A, Reiterpallasch, Rakuyo, Beast Claw | 2.0 / 4.3 | 0.18 m | 80 ms |
| Medium | Saw Spear, Hunter Axe A, Kirkhammer A, Burial Blade, Ludwig's A, Church Pick, Whirligig A, Saw Cleaver B, Threaded Cane B (whip) | 2.2 / 4.6 | 0.20 m | 90 ms |
| Heavy | Hunter Axe B, Kirkhammer B, Ludwig's B, Beast Cutter, Boom Hammer, Holy Moonlight B, Amygdalan Arm, Logarius' Wheel | 2.0 / 4.2 | 0.28 m | 110 ms |

Heavy weapons get a lower heavy threshold (their R2 is the natural big swing) and a longer arc (no flicks).
Transform (L1) stays a button; doc 13's wrist-flick gesture can come later.

### 4.4 Combo timing (the scheduler)

`swing::AttackScheduler` turns events into presses. The game buffers the next attack from `ChrActionFlag 87`
("Cancel - Common Input Check") and starts it at flag 4 ("Cancel - RH Attack"); flag 11 frees movement and ends
the regen-0 interval. A swing before the buffer opens waits for it (up to `queue_s`, 0.35 s), a newer swing
replaces a waiting one, a swing much too early is dropped (`TooEarly`); presses are 100 ms taps with a 50 ms
release between taps of one button; `ChargeStart`/`Release` hold and release R2; `AttackEnded()` (the game left
the attack early: stagger, roll) clears the timing. First R1 of every moveset (seconds after the press; from
the player's TAE, `scratchpad/stam/combo.py`):

| Moveset | Hit window | Buffer (87) | Chain (4) | Dodge (26) | Move (11) |
|---|---|---|---|---|---|
| a112 Saw Cleaver | 0.37-0.47 | 0.33 | 0.57 | 0.63 | 0.73 |
| a122 Saw Spear, form B R1 #1 (`040000`; its form-A R1 has no attack event in a122) | 0.60-0.70 | 0.57 | 0.80 | 0.90 | 1.03 |
| a103 Hunter Axe | 0.53-0.67 | 0.50 | 0.77 | 1.00 | 1.07 |
| a127 Threaded Cane | 0.43-0.53 | 0.40 | 0.57 | 0.70 | 0.80 |
| a105 Kirkhammer | 0.47-0.57 | 0.43 | 0.67 | 0.77 | 0.90 |
| a113 Blade of Mercy | 0.37-0.47 | 0.27 | 0.50 | 0.67 | 0.80 |
| a114 Chikage | 0.47-0.57 | 0.43 | 0.63 | 0.77 | 0.97 |
| a121 Burial Blade | 0.47-0.57 | 0.43 | 0.67 | 0.80 | 0.93 |
| a104 Rifle Spear | 0.43-0.53 | 0.40 | 0.67 | 0.77 | 0.87 |
| a124 Reiterpallasch | 0.43-0.53 | 0.40 | 0.60 | 0.73 | 0.83 |
| a106 Beast Claw | 0.40-0.50 | 0.37 | 0.57 | 0.67 | 0.80 |
| a116 Stake Driver | 0.47-0.57 | 0.43 | 0.60 | 0.73 | 0.87 |
| a109 Tonitrus | 0.50-0.60 | 0.47 | 0.73 | 0.80 | 0.93 |
| a110 Logarius' Wheel | 0.70-0.80 | 0.67 | 1.17 | 1.40 | 1.53 |
| a140 Beasthunter Saif | 0.63-0.73 | 0.53 | 0.80 | 1.03 | 1.20 |
| a141 Beast Cutter | 0.53-0.67 | 0.50 | 0.77 | 0.97 | 1.17 |
| a142 Amygdalan Arm | 0.53-0.67 | 0.50 | 0.97 | 1.10 | 1.30 |
| a143 Holy Moonlight Sword | 0.57-0.70 | 0.53 | 0.97 | 1.10 | 1.30 |
| a144 Rakuyo | 0.47-0.60 | 0.43 | 0.67 | 0.80 | 0.90 |
| a145 Boom Hammer | 0.60-0.70 | 0.53 | 0.90 | 1.13 | 1.30 |
| a146 Bloodletter | 0.47-0.57 | 0.43 | 0.73 | 0.77 | 0.90 |
| a147 Church Pick | 0.43-0.53 | 0.40 | 0.63 | 0.73 | 0.90 |
| a148 Whirligig Saw | 0.50-0.60 | 0.47 | 0.67 | 0.93 | 1.10 |
| a149 Simon's Bowblade | 0.43-0.57 | 0.43 | 0.63 | 0.80 | 0.93 |

Saw Cleaver R1 #2 (`030100`): window 0.33-0.43, buffer 0.30, chain 0.57. R2 tap (`032620`, after the
`032600` hold): window 0.30-0.40, buffer 0.27, chain 0.87, move 1.03; charged (`032630`): 0.27-0.37 / 0.23 /
0.93 / 1.10. Form B R1 (`040000`): 0.57-0.70 / 0.53 / 0.77 / 1.03. The pattern is the same everywhere: the
buffer opens 0.03-0.1 s before the hit window, the chain ~0.1-0.2 s after it. These go into the scheduler's
`ComboTiming` per weapon and form; the real values should come from a generated table (`combo_timing.json`,
section 6 G2) or live from the TimeAct ring.

### 4.5 Rejecting what is not a swing (tested)

| Case (synthetic, 90 Hz, 2-3 mm noise) | Result |
|---|---|
| A minute of standing with tracking noise, slow drift and laser-pointing wrist turns (+-20° at 1.5 Hz) | no event |
| 8 s jogging at 1.6 m/s with head bob and arm sway | no event; a slash made while jogging still counts once |
| Head yawing +-70° (looking around) with still hands | no event |
| A quick 90° body turn carrying the hand at ~3 m/s | no event (it is a light attack without the turn debit) |
| A one-sample 0.6 m tracking jump; a 0.3 s tracking loss | no event |
| A fast overhead wind-up (rising stroke) | no event; held still overhead it charges |
| The hand held up in front of the face (adjusting the headset) | no charge |
| Menus | the host passes `blocked` (menu or cutscene detection, doc 10): every swing is dropped |

### 4.6 Latency

Measured on the synthetic minimum-jerk swings (`scratchpad/stam/lat.cpp`; "start" = the hand starts moving):

| Swing | Result | Decided after start | after onset |
|---|---|---|---|
| 80°, 0.60 s (peak ~2.7 m/s) | light | 233-244 ms | 100 ms |
| 80°, 0.55 s | light | 211-222 ms | 100 ms |
| 110°, 0.40 s | heavy | 144 ms | 78 ms |
| 110°, 0.30 s | heavy | 89 ms | 44 ms |
| 120°, 0.22 s | heavy | 56 ms | 22 ms |

Then the press reaches the game at its next pad read (<= 1 game frame, 17-33 ms) and the animation starts
~1-2 frames later. Budget **[S]**: swing start → animation start **<= 150 ms** for fast swings (they are
decided early) and ~250 ms for slow ones; the in-game hit then lands at the moveset's window (0.27-0.70 s
later). A light decision can be made earlier (`decide_s` 60 ms) at the price of heavy swings being read as
light more often; tune it in the sandbox with recorded swings.

**What is acceptable [S].** A button-like action feels immediate under ~100 ms and laggy past ~200-300 ms;
canned-animation VR melee (Luke Ross mods, ERVR before its physics mode) is accepted because the player sees
the avatar's own swing. Here, in third person the swing is a *trigger* and only the trigger latency matters
(keep it under 150 ms; haptic tick at the press). In first person the game's hit arrives 0.3-0.7 s after the
real strike: that is the real mismatch.

### 4.7 The mismatch between your swing and the game's animation; who decides hits

- **The game's capsules decide hits.** They carry the damage, the hit list, poise, hitstop and the AI's
  reactions; the stamina charge follows the same event, so you never pay for a hit that did no damage or get
  a free one that did. The sandbox's physics weapon (collision track) is the target *feel*, not the arbiter
  in the game.
- **Third person:** the avatar swings on its own timeline; the player reads the controller swing as "go".
  Reduce the gap: decide early (onset rules above), tick the haptics at the press, pulse hard at the game's
  hit event (the hit hook), keep the moveset's own pace.
- **First person:** the weapon follows the hand outside attacks (doc 13 arm IK); during an attack the blend
  policy (13 5.4) hands the weapon to the animation, so the blade leaves the hand for ~0.5-1 s. Options, in
  order: (1) fade the weapon to a ghost while the animation owns it; (2) doc 14 option C: keep the weapon on
  the hand and move the game's attack capsules with it inside the TAE window (needs the dummy-poly query,
  14 3.4), so the game's hit test happens where the player swung; (3) never raise animation speed: it
  desyncs AI and timing.
  Opt-in experiment, off by default: `atk_speed=<mult>` (out/fp.txt) / `BB_VR_ATK_SPEED`, hook "VR Attack Speed (chr dt)"
  (src/runtime_vr_atkspeed.c). The ChrIns update 0x1cbd6a0 stores the character's time step (frame time through its
  hit-stop module) at ChrIns+0xE0, which the stamina regen and the ChrCtrl/behaviour stages read as dt; the hook
  multiplies it for the player while the newest TimeAct animation has a sub-id of 30000-49999 (attack and recovery).
  Unverified in play: with `BB_VR_FP_LOG=1` it logs the clip's measured rate (should read about the multiplier).
- **Charged R2** naturally hides the delay: the hold covers the startup and the release is the strike.

---

## 5. The dash's stamina (question D)

- **The game charges it [C mechanism, L for dodges]:** no BehaviorParam row for rolling has a cost (`504
  共通攻撃【ローリング】`: 0); the only other stamina writer a dodge can reach is the behaviour script's act 36
  "スタミナ追加" (0x1e11ce7), which the HKS calls with a negative constant. The dodge animations
  (`a00_007000..7107`) also zero regen until 0.8-0.93 s (TAE 225) and carry i-frames 0-0.37 s (flag 8), dodge
  re-cancel (flag 26) at 0.6-0.8 s.
- **The game refuses it at 0 [L]:** the HKS reads stamina (env 30) before starting a dodge.
- **What is extra:** only the bridge gate (3.2): do not send the circle press when stamina <= 0, so a buffered
  dodge does not fire late. The dash track's camera filter (`vr_dash.h`, doc 19) only moves the camera; it
  costs nothing itself. In the sandbox, `StaminaModel::TryDash` charges `dash_cost` (20 until the probe
  measures the game's value) and refuses at 0.
- **Open:** the exact dodge/backstep/sprint costs (HKS constants; the probe measures them) and whether sprint
  uses act 41 (regen block) as well as act 36.

---

## 6. Plan: sandbox first, then the game

### 6.1 Sandbox (question E; the files are being edited by the collision and dash tracks: integrate after them)

- **A `combat.{h,cpp}` module** in `sandbox3d/` owning one `SwingClassifier`, one `StaminaModel` and a
  ticket per swing; the scene calls it once per frame after the rig and before the physics step.
- **Input:** `HandSample` from the stereo frame's right grip (`BbXrStereoFrame.grip[1]`, already in the eye
  poses' space) and the head (the midpoint of the two eye poses), `squeeze[1]`.
- **Swing → attack:** on Light/Heavy (or ChargeRelease): `CanAct()`? `BeginSwing(t, cost)` with the cost of the
  shown attack (`wp_<id>.json` `attacks[].stamina`: judge 0/150/170) and open the physics hit test for
  `hit_window_s` (it replaces `PhysicsConfig::hit_speed` as the gate). Refused: "TIRED" label, two short
  buzzes.
- **Hit:** the physics weapon's hit (`TestHits`, enemy hurtbox) → `Hit(t, ticket)`: the first hit of a swing
  takes the cost (the bar segment flashes), later ones are free. A wall contact counts only with
  `stamina.charge_world` (default true, like a clang in the game).
- **Dash:** the locomotion's dodge/backstep request → `TryDash(t)`: refused at 0 (no dodge, buzz). Sprint
  drain, if wanted: a per-second `TryDash`-like cost once the game's value is known.
- **Regen:** `Update(t)` every frame (45/s after 0.75 s; the game's numbers).
- **UI:** a stamina bar on the left forearm (doc 18 body-anchored UI; fallback: head-locked under the view):
  green current value, a white ghost for the pending tickets, grey when regen is paused, red flash on a
  charge, the -50..0 part drawn darker. A swing readout above the right hand: live speed needle on a scale
  with the onset/light/heavy ticks, the phase, the last label ("LIGHT 2.9", "HEAVY 6.1", "WIND-UP", "TIRED",
  "CHARGE"), peak and latency.
- **Live config** (`out/vr_assets/sandbox3d.json`, hot reload): `"swing": {class, probe, onset_speed,
  light_speed, heavy_speed, heavy_accel, min_arc, up_limit, decide_s, max_swing_s, rearm_speed, rearm_s,
  reverse_rearm, turn_comp, charge, charge_hold_s, charge_above_head, charge_behind}` and `"stamina": {max,
  regen_per_s, regen_delay_s, hit_window_s, dash_cost, floor, gate_at_zero, costs: "weapon" | [light, heavy,
  charged], charge_world}`.
- **Recording:** every swing as one JSON line (samples, decision, label, hit/miss, stamina) next to the
  collision track's log, so thresholds can be replayed through `swing_logic.h` offline (doc 12 4).

| Step | Exit test |
|---|---|
| S1 classifier readout only | 50 deliberate slow and 50 fast swings: >= 90 % labelled as intended; < 1 false event per minute standing, walking in place, looking around, using the menu laser |
| S2 stamina bar + misses free | hitting the dummy drains the bar once per swing, misses never do, dashes do, at 0 nothing starts, regen after 0.75 s |
| S3 charged R2 | 10 wind-up holds: 9 charges, 0 charges from headset adjustments |
| S4 tuning record | a recorded session replays to the same labels offline |

### 6.3 Sandbox status: S1 and S2 built, not yet tried in the headset (2026-10-09)

Built into `gpu/xr_bridge/sandbox3d/` (its README, "Swings and stamina"). It uses `swing_logic.h`
as the game will. The header gained only readouts, an overload and the follow-through rule (4.2).

| Plan (6.1) | As built |
|---|---|
| `combat.{h,cpp}`, called by the scene | `combat.{h,cpp}` (pure CPU), called by the frame loop (`main.cpp`) right after `Scene::Update`; the scene is unchanged. The HUD is `combat_view.{h,cpp}` |
| Input: the stereo frame's right grip and the eyes' midpoint | The same, taken before the locomotion moves them into the world. In this tracking space, stick walking, snap turns and dashes are not swings |
| Light / heavy / charge release → `BeginSwing` with the shown attack's cost | The held weapon's costs from `hitbox/wp_<id>.json`: judges 0 / 150 / 170 (form B: 400 / 550 / 570). The Saw Cleaver gives 20 / 35 / 50, checked against the user's dump in the test; the config can override them. At 0 or below the swing is refused: NO STAMINA, two buzzes |
| The physics hit test opened for `hit_window_s` instead of `hit_speed` | The physics is unchanged; its HIT keeps its own 2 m/s rule for the marker and the flash. Each frame `DetectSwingContacts` reads its public state: the shown attack's hit capsules (the HIT's own shapes) against the enemy's hurtboxes, and the weapon's own capsules (plus 1 cm) against the post, the wall and the floor. The physics' HIT of the frame counts as a contact too |
| The first hit of a swing pays | Any contact while a ticket is open pays it, once. A contact made while a stroke is still being measured waits for that stroke: an attack takes it (up to `pre_contact_s`, 0.15 s, before the stroke's onset), and a dropped stroke hands it to the swing still open before it. A sample pays one ticket at most |
| A wall counts with `charge_world` | The post and the wall (`charge_world`, default true); the floor only with `charge_floor` (default false) |
| The dash → `TryDash` | `Locomotion::move_gate` (new, a `std::function`) is asked before a dodge, backstep or jump starts. `StaminaModel::TryDash(t, cost)` (new overload) pays 20 each (`dodge_cost`, `backstep_cost`, `jump_cost`); at 0 or below the move is refused and nothing moves |
| Bar on the left forearm, readout above the right hand | Head-locked 24° below the view (a swinging hand is unreadable): the bar over −50..max, "SP", the label with its result and detail, the live speed scale. A second bar sits above the left wrist, facing the eyes. The cost floats up from the contact point. Drawn in the game's font through the menus' UI pipeline (a draw hook chained after theirs) |
| `"swing"` and `"stamina"` objects | One `"swing"` object: the classifier keys, `"stamina"`, `"haptics"`, `"hud"` and the logs. It is added with the defaults when missing and hot reloaded. `class` (`auto` follows the weapon and form) sets light / heavy / min_arc / decide / max_swing unless they are set |
| Every swing as a JSON line | `sandbox3d_swing.jsonl` in the run folder: swing, stroke (wind-up, too slow), hit or clang, miss, refusal, dash, stamina changes. Swing and stroke lines carry the tracking-space hand samples from 0.3 s before the onset. No replay tool yet (S4) |

Additions to the plan:
- `charge_full_s`: R2 must be held 0.8 s for the release to be the charged attack; a shorter hold
  releases as the R2 tap (HEAVY and its cost). The 0.8 s is a placeholder (not measured, 1.3).
- `replace_physics_hit`: a physics HIT that was no attack gets a soft bump instead of its 1.0
  pulse.
- A menu that uses the sticks blocks swings (BLOCKED).
- The haptics by kind: a tick on the decision (light 0.2, heavy 0.4); a hit pulse (light 0.6 for
  60 ms, heavy 1.0 for 120 ms, charged 1.0 for 200 ms) that replaces the physics' pulse that frame.

Tests:
- `bbport_xr_swing_test.exe`: 113 checks (88, plus the dropped-stroke readout,
  `TryDash(t, cost)`, and one attack per long arc).
- `bbport_xr_sandbox3d_combat_test.exe`: 147 checks. The rules run through `Combat::Update` with
  placed contacts; the same swings run through `WeaponPhysics` against a torso and a wall; the
  locomotion's gate is exercised too.
- `--offline` writes `combat_hit.png` (HEAVY into the Huntsman, −35), `combat_tired.png` (−9 after
  five dodges, NO STAMINA, the wrist bar) and `combat_charge.png`.
- `scripts/vr_sandbox3d.sh` runs both test executables.

Open:
- The 6.1 exit tests need the headset.
- The thresholds are still the synthetic ones.
- The game's attack capsules are thick (0.24 m for the Saw Cleaver's R1): close to the enemy almost
  every swing connects, as in the game.
- Sprinting costs nothing.
- Fast reaches for the body-slot menus are not blocked.
- The move costs and the full-charge time wait for the probe (7.2).

Weapon review fixes (sandbox, 2026-10-09). A swing in progress (the fastest attack shape or the
tip at `hit_speed`, until it falls under `rearm_speed`) passes through the enemy's hurtboxes
(`swing_through_enemy`; the post and wall stay solid) and its hits are tested against the unclipped
target pose, so a blocker cannot prevent them. The hand lets a blocked weapon go at an arc
(`max_separation_arc_m` 1.5 = angle x reach, replacing `max_separation_deg`). One glitch distance:
physics `teleport_m` 0.3 m / `teleport_deg` 90 on the raw controller, as `glitch_m`; a teleport or a
new weapon clears the swing (hit, overlap, slow time) and combat's classifier, charge and open
tickets. Stamina pays only for a contact that starts during a swing at `hit_speed` (the physics'
HIT rule); a resting weapon pays nothing. A stroke reversing by more than `rearm_reverse_deg` (120)
rearms at once (forehand then backhand). Combat reads the hand that holds the weapon (right, else
left). Body-sourced hit shapes follow their bone. Blocking capsules are capped at `radius_max_m` 5 cm.

### 6.2 The game

| Phase | Content | Exit test |
|---|---|---|
| **G0** probe (now, read-only) | `tools/vr_stamina_probe.py` procedure (section 7) | the offsets, the deduction moment, the dodge cost, the floor and the gate as predicted |
| G1 bridge classifier | replace the speed thresholds in `xr_input.cpp` (`SwingSpeed`/`HeavySpeed`) with `SwingClassifier` + `AttackScheduler` on the right hand; fixed per-class tuning, default timing; `blocked` while the bridge's menu laser is up; `BB_XR_GESTURES=2` selects it (1 = the old thresholds) | the S1 numbers in the game; no attack in menus |
| G2 game state to the bridge (ABI 6) | `SetGameState{stamina, stamina_max, flags (menu, attacking, can_act), anim id + time, right weapon id, style}` written by the host each game frame (pad sampler); the scheduler gates on `can_act`, calls `AttackEnded()` when the animation is not an attack, takes `ComboTiming` from `combo_timing.json` (extend `hitbox_extract.py`'s TAE pass with flags 87/4/26/11 and event 225 per animation) | no ghost attacks after exhaustion; queued swings chain on the first possible frame |
| **G3** defer-until-hit | `src/runtime_vr_stamina.c` (handlers, tickets, `BB_VR_STAMINA=off\|defer\|refund`, `BB_VR_STAMINA_LOG=1`), registry entries in `runtime_hooks.c`, the disabled XML entries of 1.5 | misses leave stamina unchanged; hits cost exactly the BehaviorParam cost once; the debug flags still suppress costs; 30 min soak without drift |
| G4 feedback | haptic pulse on the hit hook (intensity from AtkParam `dmgLevel`), "tired" buzz, the stamina bar on the HUD layer (doc 10) | — |
| G5 first person | doc 14 option C with doc 13's arm IK (hits where the hand swung) | doc 13 W6 |

G3 handler sketch (game thread; reads only through the readable-memory guard of `runtime_vr_fp.c`):

```c
cost hook (0x1cb644d):  if (ctx->r12 == player) {
                            int32_t d = *(int32_t *)(ctx->r14 + 0x78);
                            const uint8_t *row = *(const uint8_t **)(ctx->rbp - 0x48);
                            if (d < 0 && row && row[9] == 0) {                 /* refType attack */
                                ticket_open(*(int32_t *)(row + 0xC), -d, now); /* AtkParam id, cost */
                                if (mode == DEFER) *(int32_t *)(ctx->r14 + 0x78) = 0;
                            } }
hit hooks:              if (ctx->rdx == player) ticket_hit(*(int32_t *)(ctx->rdi + 0x3C), now)
                            -> DEFER: stamina = clamp(stamina - cost, -50, max) unless D+0x200&0x80 or [0x593E88A]
per-frame tick (REFUND): expired tickets without a hit -> stamina = min(max, stamina + cost)
```

Tickets expire at the TAE event's end plus 0.1 s, or 0.6 s after opening when the end is not known (most
player windows last 0.1-0.3 s; the plunge's is 0.63 s, doc 14 2.2).

### 6.4 G3 status: built, not yet run in the game (2026-10-09)

| Part | As built |
|---|---|
| Rules | `src/vr_stamina_tickets.h` (C11, header-only, also builds as C++): the sandbox's rules. A ticket per costed attack of the player. **defer**: the request's `+0x78` is written to 0, the first hit pays the cost through the game's clamp (`x < -50 ? -50 : min(x, max)`), later hits of the same ticket are free, a window that ends without a hit is free. **refund**: the game pays at the start; a missed window gives back what the game really took (after its clamp, 0 when a debug flag skipped it). The debug flags (module `+0x200` bit 0x80, the global byte 0x593E88A) suppress the charge |
| Matching | A hit takes the newest open ticket with its AtkParam id (`[rec+0x3C]` = the row's `refId`); with none, the newest open ticket (`BB_VR_STAMINA_MATCH=any`, the default and the sandbox's rule; `id` = by id only). Window `BB_VR_STAMINA_WINDOW` = 0.75 s (the plunge's 0.63 s + 0.1); 8 tickets at most |
| Handlers | `src/runtime_vr_stamina.c`: cost (`r12 == player`, `[rbp-0x48]` = the row, refType 0, delta < 0), hits (`rdx == player`), tick (`rdi == player`, closes the windows). Reads through the readable-memory guard of `runtime_vr_fp.c` (a copy, as `runtime_vr_dash.c` does); writes check the page is writable; the player's and the data module's vtables are checked. The image base comes from each hook's own rip. Registered from `probe.c` (`runtime_vr_stamina_register`) |
| Hooks | `runtime_hooks.c`: `bb_hook_vr_stamina_cost`, `bb_hook_vr_stamina_tick`, `bb_hook_vr_hit_chr`, `_map`, `_obj`. `patches/Bloodborne.xml` (disabled): **"VR Stamina Cost (behavior)"** (0x1cb644d 16 B + the tick 0x1cbdb20 17 B) and **"VR Stamina Hits"** (0x1cb79e0 20 B, 0x1cb8730 17 B, 0x1cb8430 17 B), all replay-after |
| Verified offline | Each `Expect` equals the eboot's bytes at the address (through `scripts/patches.py compile_hooks` and the ELF program headers); the windows are whole instructions (capstone) with no branch into them (0x1cb6430 `jne 0x1cb644d` lands on the first byte: fine); `[rbp-0x48]` is the BehaviorParam lookup's result pointer (0x1cb6225-0x1cb6235, the row's `+0x09` tested at 0x1cb62c0); the five windows decode and install in `hooks-test` |
| Env | `BB_VR_STAMINA=off\|defer\|refund` (default off: the handlers return at once, nothing is read), `BB_VR_STAMINA_LOG=1` |
| Tests | `ninja -C out/gpu vr-stamina-test vr-stamina-test-cpp`: **74 checks, 0 failed** in C and in C++ (miss free, one charge per attack with a 3-hit sweep, 20 attacks = 20 x cost, combo tickets, id and fallback matching, expiry and eviction, refund with the clamped amount, the floor, the debug flags, the player-only filter, the modes). `hooks-test`: the five names resolve and the windows install |
| Binary | `out/gpu/bb-probe-stamina.exe` |

**First headset run (out/vr_all_alt_1.log, defer, log 1): no ticket ever opened.** All five hooks installed. Only
`world hit` lines came (8,373: AtkParam 304 and 307 3,927 times each, ~11/s, the player's body contacts, not weapon
attacks; weapon ids 100000/100910/100790/... a few hundred), none for characters or objects, no `ticket` line.
Static re-check of 0x1cb61c0 found no wrong fact: r12 = rdx = the ChrIns (the caller 0x1cc7530 passes its `this`),
r14 = the request at the caller's rbp-0xC0 (+0x78 = the caller's rbp-0x48), `[rbp-0x48]` = the lookup result of
0x1cb6235 (refType +9 tested at 0x1cb62c0), and the world hook's `rdx == player` test passes on the same
PlayerIns. So the cause is **live-only, not settled**. Changes: the world and object hooks pay only by AtkParam id
(never "newest ticket": 304/307 would charge every swing made while walking), hits with no open ticket are counted and
summed every 10 s, a hit with an open ticket prints itself, and the cost hook (log 1) prints its first 24 calls (r12,
player, r14, delta, row, refType, refId and the verdict) plus the first 16 failing calls with delta < 0, a 10 s counter
summary (not the player / no cost / no row / not an attack / no data module / request not writable / opened) and a
`ticket N opened` line. Next session: `cost call #` lines say which test fails (r12 != player => the executor's
character is not WorldChrMan+0x60; no `cost call` at all => 0x1cb644d is never reached by the player's attacks, and the
cost is taken elsewhere).

**Second headset run (out/vr_all_alt_5.log, defer, log 1): the cost hook was never called, for anyone.** The user
attacked enemies several times. All five hooks installed (`bb_hook_vr_stamina_cost at image offset 0x18b644d`, =
0x1cb644d - 0x400000; the ELF bytes at file offset 0x18ba44d are the `Expect` bytes), the tick and the world hook fired
(28 player world hits), but no `cost call #` line: with log 1 the handler prints its first 24 calls of *any* character
before any filter (checked in the code), so 0x1cb644d did not execute once, not even for an enemy's attack. Also **0
player character hits** in that session (0x1cb79e0 with `rdx == player`). Static re-check (2026-10-09):

- 0x1cb61c0 is still reachable for an attack row: `[rbp-0x38]` and `[rbp-0x58]` come from lookups of the request's
  `+0` / `+4`, both -1 in `RequestBehavior`'s request (0x1cc75c3-0x1cc75d9), so a refType-0 row goes 0x1cb62e6 ->
  0x1cb6336 -> 0x1cb63d1 (MP) -> 0x1cb644d. Its only exits before the cost are `r12 == 0` and the three
  `0x1cb6680 == false` branches (bullet / SpEffect rows). Callers: `RequestBehavior` 0x1cc7530 (from the TAE attack
  handler 0x1e267a0 at 0x1e26a27, gated by the event's begin byte `[rsi+0x18]` and the control-type table at
  0x4b348a0: type 0 passes) and the dispatcher's direct call 0x1e26634; 0x1cc9160 (vtable 0x578e6d8/0x578efc8);
  0x1e402b0; 0x1e40780. So either the attack event never takes the begin path in this build, or the cost is
  written somewhere else.
- **Every inlined stamina writer that honours "All No Stamina Consume" (the only negative writers)** references the
  byte 0x593E88A; 21 functions do: 0x18f54c0, 0x19bed10, 0x1a9a810, **0x1cb61c0**, 0x1cbdb20 (regen), 0x1cbdf00 and
  0x1cd5c90 (out-of-line `ChrIns` AddStamina/SetStamina, no static callers), 0x1cc9d90 (a `ChrIns` vfunc,
  vtable 0x578e420: a percentage of the max, SpEffect [L]), 0x1cfd460, 0x1d15320 (debug), 0x1d20510 (debug menu),
  **0x1e112b0** (HKS act 36), 0x1e293b0 / 0x1e29890 (bullet costs), **0x1e2a770** (`stamina -= [x+0x230]` at
  0x1e2c2c0, the cost computed at 0x1e2b01c by vfunc +0x208 with an "insufficient" flag +0x214: a charged/bullet
  module [L]), 0x1e56720 / 0x1e56c00 (stat sync), 0x1e56ae0 / 0x1e57190, 0x2013d00, 0x224b830 (max changes). None
  besides 0x1cb61c0 reads a BehaviorParam row, so static analysis alone cannot name the R1 path.

**Decisive live test: `BB_VR_STAMINA_TRACE=1`** (`src/runtime_vr_stamina.c`, any `BB_VR_STAMINA` mode, Windows). A
hardware write watchpoint (DR0, 4 bytes) on the player's stamina field, armed on the game thread from the tick hook
through a private software exception whose vectored handler loads DR0/DR7 (verified standalone: Dr6.B0 is reported,
RIP is the instruction after the store, the rbp chain walks). The handler only fills a 64-entry ring and a per-writer
table (`vrst_site_note`, tested); the tick prints. Log lines:
`VR stamina trace: watching SP 91/91 at ... armed`; one line per drop,
`SP 91 -> 71 (-20) written before 0x1cb64cd, return 0x1cc77c0 <- 0x1e26a2c <- ...` (the VA after the store, then
three return addresses); every 10 s `N writes; cost hook K player calls; player hits C character, W world, O object`
and the writer table; `changed with no trapped write` if another thread writes it; `cost hook reached` for the first
8 calls of 0x1cb644d (any character). Reading it: the drop at an R1's start names the writer; `written before
0x1cb64cd` means 0x1cb644d *was* executed and the hook itself is at fault; anything else is the site to move the defer
hook to (its callers say whether the TAE event, the HKS act 36 0x1e11d91 or 0x1e2a770 pays).

**Trace result (out/vr_all_alt.log, mode off, trace on) [M]: the player's attacks pay in 0x1e293b0.** Every R1 drop
(5, -20/-22) was `written before 0x1e2984c, return 0x1e258e4 <- 0x1e2163c <- 0x1e27935`: the store 0x1e29845 in
0x1e293b0, called by the TAE dispatcher 0x1e249d0 at 0x1e258df. One -10 drop came from 0x1cc9d90 (0x1cca5fe, return
0x1cfec0f, a `ChrIns` vfunc: a roll/backstep or sprint [L]); the regen writes at 0x1cbdcc4, and 0x1e5739d writes every
frame without changing it. 0x1cb644d: 0 calls (so 1.4's "when" is wrong: `RequestBehavior` is not the player's
path). The tick ran on up to 7 threads (the character update is a job pool), so the hooks now take one spinlock.
0x1e293b0 is the **attack-behavior handler** (doc 1.2 called it "bullet behaviors"): `(rdi = handler, [rdi+8] =
ChrIns, esi = behavior id, ecx = slot 0..7)`; it looks the BehaviorParam row up (0x23133c0, `[rbp-0x38]`; refType 0
-> AtkParam id = refId), creates the attack through 0x1c63970 (handle in `[handler + slot*8 + 0x20]`), then
`cost = (int)PlayerIns vfunc +0x200(behavior id)` (BehaviorParam_PC stamina x the SpEffect rates) in r12d
(0x1e297c7), `stamina = clamp(stamina - r12d)` unless a debug flag is set (0x1e2982a-0x1e29845).

**The defer moved there:** "VR Stamina Cost (behavior)" now hooks **0x1e297cb** (`Expect`
`498b4508488b80b00300004c8b7820`, 15 bytes: `mov rax,[r13+8]; mov rax,[rax+0x3b0]; mov r15,[rax+0x20]`, no branch
into it), replay-after, same handler name. At the site r12d = the cost, `[r13+8]` = the character, r14d = the
behavior id, `[rbp-0x38]` = the row. Defer sets r12 to 0 (the game then subtracts 0 and stores the clamp of the
unchanged value); refund leaves it; the ticket key is the row's refId as before, so the hit hooks are unchanged (1
player character hit in that session = the one enemy the user hit).

**Headset run with 0x1e297cb (out/vr_all_alt_10.log) [M]:** tickets opened with the right costs (20/22/25/27/40),
but nearly every one was paid 0.00-0.07 s in by 4-26 *world* hits with the attack's own AtkParam id, swings into
the air included: the world handler 0x1cb8730 reports every map contact of the hitbox from its first frame (the
floor). Fix: the world hook moved inside that handler to **0x1cb87ff** (`Expect`
`488b108b83f001000089c6c1ee10`, 14 bytes, no RIP-relative operand: the first try at 0x1cb87f5 included a `lea
rax,[rip+...]` the installer could not reach from the trampoline in the game), just after the attacker's
module (bag +0x98, vfunc +0xb0) decided whether the weapon bounces (`al`; true makes the hit processor stop the
attack, 0x1c61420 -> 0x1c614b0; saved in r15b at 0x1cb87f5); there rbx = the record, r14 = the attacker. A world contact pays only when the
game bounced the weapon (`vrst_contact`: character, bounced wall, object pay; floor free). The HIT line names the
contact; the first 12 free floor contacts print the record's +0x180 point and +0x190 vector. **[L]** that the bounce
flag is exactly "wall/post": confirm with a swing into a wall. The session ended with a guest fault on the death
reload (a list unlink at 0x28a435e, thread 27124, none of the stamina hook threads); nothing in the stamina code
writes guest memory except the player's validated SP and the cost register, so it is not attributed to it (bisect:
`STAMINA=off`, or the arms A3 hook off).

For the probe (G0) to confirm, all **[L]** until then: the cost hook runs once per attack with `[r13+8]` = the
player and the row at `[rbp-0x38]`; the hit record's `+0x3C` equals the row's `refId` (the log says
`by id` or `newest ticket`); the hit handlers run for guarded, parried and i-frame hits or not;
0x1cbdb20 runs every frame for the player (else tickets close lazily, at the next cost or hit); all
five hooks run on the game thread (the state is unlocked).


---

## 7. Test plan

### 7.1 Offline (done)

`out/xr-bridge/bbport_xr_swing_test.exe` (built with `BB_BUILD_LOCK` = the scratchpad lock): slow → light,
fast → heavy, the light/heavy boundary, weak motion ignored, runtime velocities, forehand/backhand (slow and
fast turn), jogging, a slash while jogging, idle + menu pointing for a minute, head turns, body turns,
glitches and tracking loss, the charged heavy (charge, quick wind-up-strike = one heavy, face, slow lowering),
combo queueing/superseding/too-early/gap, charge hold, blocked, miss = free, one charge per swing, the window
end, gating at 0 and the -50 floor, regen timing, game-mode mirroring, and the full loop (5 swings, 30 each,
from 45: hit, miss, hit, hit, tired). **88 checks, 0 failed.** Since the sandbox integration (6.3):
**113 checks, 0 failed**. The new ones cover the dropped-stroke readout (too slow, wind-up), the
dash with its own cost, and one attack per long one-way arc. The sandbox's own
`bbport_xr_sandbox3d_combat_test.exe` adds 147 checks.

`python tools/vr_stamina_probe.py --check-elf out/eboot.elf` (signatures unique and at their VAs) and
`--self-test` (the reader against a fake process with a made-up chain) both pass.

### 7.2 Live game, read only (for the user: after the soak test, headset not needed)

1. Start the game as usual from the repository's `out/` with one extra variable,
   `BB_PAD_FILE=<repo>/out/stamina_pad.txt` (an absolute path; create the file empty). Load a save
   in the Hunter's Dream with the Saw Cleaver in form A, no Rune that changes stamina.
2. In a second shell: `python tools/vr_stamina_probe.py --csv out/stamina_probe.csv` and wait for
   `VA 0x400000 is mapped at ...` and a first `SP n/n ...` line (the maximum depends on Endurance).
3. Either play the steps below by hand, or add `--script --pad-file <repo>/out/stamina_pad.txt`
   (the same path; it presses R1, R2, a backstep, a forward roll, eight more dodges to exhaust stamina, R1
   twice while exhausted, then idles; ~26 s). Do not touch the pad meanwhile.
4. What to look for (each change prints the animation and its three ring floats; one of them is the time;
   the probe writes animation group 0 as `a000_`):

   | Step | Expected | Confirms |
   |---|---|---|
   | R1 | SP -20 while `a112_030000` is ~0.37 s in, not at its start; `regen pct 0` until ~0.73 s, then ~45/s back up | 1.4 when/what, 1.3 |
   | R2 tap | SP -35 during `a112_032620` (~0.30 s after the hold anim `032600`) | judge 150 cost |
   | Dodge (circle with the stick centred is a backstep) | SP drops at the start of an `a000_0070xx` (backstep) or `a000_0071xx` (quickstep/roll) animation; note the value; add a dodge with the stick held for the roll's | the HKS cost (5) |
   | Dodges until empty | SP reaches <= 0, never below -50; the next dodge does not start | floor, gate [L] |
   | R1 while <= 0 | no `a112_03xxxx` animation starts | gate [L] |
   | Sprint (hold circle while moving, by hand) | SP drains; `BLOCKED` shown or not | act 36/41 use |
   | R1 on an enemy vs into the air (Central Yharnam, by hand) | both cost 20 in vanilla | the baseline for G3 |
   | Debug menu "All No Stamina Consume" on (if the debug-menu patch is enabled) | `NoSPconsume` shown, no drops | the flag byte |

5. Send back `out/stamina_probe.csv` and the console output. The probe never writes game memory and can be
   stopped at any time (Ctrl+C); it releases the pad file on exit.

### 7.3 Live game with the hooks (G3)

From the repository (bash):

```
BB_GAME_DIR=C:/games/CUSA03173 BB_PROBE=out/gpu/bb-probe-stamina.exe BB_VR=1 BB_XR_MODE=mono BB_VR_HEAD=1 BB_VR_HEAD_LEVEL=1 BB_VR_FP=1 BB_VR_FP_LOG=1 BB_VR_DASH=1 BB_VR_DASH_LOG=1 BB_VR_FP_CONTROL=out/fp.txt BB_VR_STAMINA=defer BB_VR_STAMINA_LOG=1 BB_HOOKS="VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);VR LOD Fix (SetCamera);VR FP Camera Heading (in-game step);VR Stamina Cost (behavior);VR Stamina Hits" python run.py
```

At start the log says `VR stamina: mode defer, window 0.75 s, match any, log 1`. `BB_VR_STAMINA_LOG=1`
prints one line per ticket when its window closes: `ticket N atk A cost C: HIT at +0.37 s (1 hit, by id,
atk A), paid C (SP x -> y)` or `MISS after 0.75 s, free, saved C`; a player hit with no open ticket prints
`... hit atk A isPc P: no open ticket`. Hit a dummy-like enemy 20 times and miss 20 times; stamina after the
misses must equal the start value minus nothing (plus regen); after the hits, exactly 20 x cost. Repeat with
a 3-enemy sweep (one charge, `3 hits`), against a wall (one charge, the world hook), during an enemy's
i-frames, and with the debug flag on (`debug flag: free`). `BB_VR_STAMINA=refund` for the comparison (the
bar dips at the swing and refills after a miss). If every hit says `newest ticket`, the AtkParam ids differ
from the rows' `refId` (note them; `match any` still charges correctly for one attack at a time).

---

## 8. Risks and open questions

- **Thread of the hooks [L]:** the behavior executor, the hit dispatcher and the regen run in the character
  update; the handlers write stamina only where the game itself does, but this is not proven per thread.
  Mitigation: all writes in game-thread hooks, none from the bridge or the pad thread.
- **Unhooked hit paths:** type-3 targets (0x1cb8940) and bullet-to-character hits for costed bullets stay
  vanilla. Melee only is in scope.
- **Hits during i-frames, guarded and parried hits:** whether 0x1cb79e0 sees them decides whether they cost.
- **The HKS gate is inferred [L]:** if the script gates on "stamina >= cost" instead of "> 0" for some
  actions, deferral lets more attacks start than vanilla. The probe's step "R1 while <= 0" and a test at
  stamina 5 with a 20-cost attack settle it.
- **Late hits under defer** let an exhausted player start one extra attack; acceptable, or gate the bridge on
  `stamina - pending` for the dash only.
- **Swing/animation mismatch in first person** (4.7): the main comfort and presence issue; option C is the
  fix and needs the dummy-poly query RE (14 3.4).
- **Classifier thresholds are synthetic:** real Dream Air swings, controller jitter and user styles need the
  sandbox recordings; the defaults are starting points.
- **The anim-time float in the TimeAct ring** (which of the three is "now") is [L]; the probe prints all three.

## 9. Files

| File | What |
|---|---|
| `gpu/xr_bridge/swing_logic.h` | classifier, scheduler, stamina model (pure, header-only) |
| `gpu/xr_bridge/swing_logic_test.cpp` | 113 offline checks |
| `gpu/xr_bridge/sandbox3d/combat.{h,cpp}` | the sandbox's use of it (6.3): contacts, tickets, the moves' gate, labels, haptics, logs, the `"swing"` config |
| `gpu/xr_bridge/sandbox3d/combat_view.{h,cpp}` | the HUD (stamina bar, labels, speed scale, floating costs) and the `--offline` `combat_*.png` previews |
| `gpu/xr_bridge/sandbox3d/combat_test.cpp` | `bbport_xr_sandbox3d_combat_test.exe`, 147 checks |
| `gpu/xr_bridge/CMakeLists.txt` | `bbport_xr_swing_test` (one `add_executable`) |
| `tools/vr_stamina_probe.py` | read-only live probe, `--check-elf`, `--self-test`, `--script` |

Cross-links: 13 (arm IK blend policy, equipment and form readout, TimeAct ring), 14 (hit chain, option C,
the shape-test RE), 16 (player pointer measured), 18 (body-anchored UI for the bar), 19 (the dash camera),
12 (recording and replay).
