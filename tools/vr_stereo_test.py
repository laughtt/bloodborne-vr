#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Headset-free check of alternate-eye stereo (BB_XR_MODE=alt, docs/vr/PLAN.md Phase 3,
docs/vr/findings/22-phase3-alt-eye-stereo.md, src/runtime_vr_stereo.c).

Runs the game flat with the camera hooks (H1, H2, LOD fix), a fake head (BB_VR_FAKE_HEAD=@file),
BB_XR_MODE=alt and a stereo control file (BB_VR_STEREO_CONTROL: alt/ipd/invert/tag, re-read while
the game runs), enters the world from the title screen (cross, cross: a save that continues), and
for each step dumps several consecutive frames (BB_FRAME_DUMP_TRIGGER + BB_FRAME_DUMP_COUNT); the
presenter logs which eye each dumped frame went to. Then:
  - parallax: consecutive L/R frames are block-matched; near content (the hunter, about 4 m from
    the third-person camera) must shift right in the left eye's image (disparity x_L - x_R > 0,
    about f * IPD / Z pixels), distant content barely, the screen-fixed HUD not at all; two frames
    of the same eye must not shift, nor two frames of the mono camera;
  - negative control: with invert=1 (each eye rendered from the other eye's position) the
    disparity flips and the presenter's side check reports every frame;
  - culling, LOD, shadows: the draw counts and the shadow cascade constants of consecutive frames
    (trace columns draws, s1, s2) with alternating eyes against the mono camera;
  - the long run (--long SECONDS: walking, turning, a moving head) and the per-frame trace
    (BB_VR_STEREO_TRACE): every frame given to an eye is on that eye's side of the center camera,
    the eyes alternate, nothing is given twice.

    python tools/vr_stereo_test.py [--out DIR] [--probe out/gpu/bb-probe-st.exe]
        [--game-dir C:/games/CUSA03173] [--long 240] [--effects off|default] [--fps 60]
        [--analyze-only]

The game runs on a copy of user/ in DIR and with BB_DATA_DIR=DIR/data (its own boot files, so a
game already running from the repository's out/ is not disturbed); it ends itself (BB_EXIT_FILE,
BB_EXIT_AFTER_SECONDS as a safety net); this script never kills it. Results: DIR/report.txt,
DIR/png/ (side-by-side, red-cyan anaglyphs, disparity maps), DIR/session.log, DIR/trace.csv.
Needs numpy and Pillow.
"""
import argparse
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from vr_head_test import draws_per_frame, load_gray, log_text, press, wait_for  # noqa: E402

HOOKS = ('VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);'
         'VR LOD Fix (SetCamera);VR Stereo Off-Axis (projections)')
# The temporal effects that reproject from the previous frame (the other eye's under alternation):
# off for the parallax measurement (--effects off, the default), the game's defaults with
# --effects default.
EFFECTS_OFF = ('Disable Motion Blur (perf increase);Disable AA;Disable DoF;'
               'Disable Chromatic Aberration')
HMD_FOV = 'fov=89.1 aspect=1.4281'  # the Dream Air's union frustum: 110.2 x 89.1 deg
EYE_EDGES = 'eyel=-54.57 eyer=46'    # its left eye's edges (the right eye mirrored)

# (label, fake head file, stereo control file, what it shows)
STEPS = [
    ('alt', 'yaw=0', 'alt=1', 'alternate eyes, the game\'s 43 deg FOV, IPD 63 mm'),
    ('alt_hmd', f'yaw=0 {HMD_FOV}', 'alt=1', 'alternate eyes with the headset\'s frustum (110 x 89 deg)'),
    ('alt_ipd250', 'yaw=0', 'alt=1 ipd=0.25', 'IPD 250 mm: four times the parallax'),
    ('mono', 'yaw=0', 'alt=0', 'mono camera (no eye offset): consecutive frames must not shift'),
    ('invert', 'yaw=0', 'alt=1 invert=1', 'negative control: each eye from the other eye\'s position'),
    ('notag', 'yaw=0', 'alt=1 tag=0', 'no eye tag: pairing by the camera alone'),
    ('alt_far', 'yaw=180 pitch=12', 'alt=1', 'head turned around, looking across the garden (far content)'),
    ('mono_far', 'yaw=180 pitch=12', 'alt=0', 'the same view with the mono camera'),
    ('alt_turned', 'yaw=25 pitch=-10', 'alt=1', 'head turned 25 deg left, 10 deg down'),
    ('h2eye', 'yaw=0', 'alt=1 h2=1', 'the main context (H2, the LOD camera) at the eye instead of the center'),
    ('axis_sym', f'yaw=0 {HMD_FOV}', f'alt=1 {EYE_EDGES} offaxis=0', 'the headset frustum, symmetric (for the off-axis check)'),
    ('axis_off', f'yaw=0 {HMD_FOV}', f'alt=1 {EYE_EDGES} offaxis=1', 'off-axis eye frusta: left -54.6..46 deg, right -46..54.6'),
    ('alt_end', 'yaw=0', 'alt=1', 'alternate eyes again at the end'),
]
# --fp: first person (BB_VR_FP=1, findings/16) with the eye offset applied after the hunter's eye.
FP_STEPS = [
    ('fp_alt', 'yaw=0 pitch=-15', 'alt=1', 'first person, alternate eyes, looking ahead and a little down'),
    ('fp_alt_down', 'yaw=0 pitch=-60', 'alt=1', 'first person looking down at the body and the ground'),
    ('fp_mono', 'yaw=0 pitch=-15', 'alt=0', 'first person, mono camera'),
    ('fp_invert', 'yaw=0 pitch=-15', 'alt=1 invert=1', 'first person, negative control'),
    ('fp_alt_end', 'yaw=0 pitch=-15', 'alt=1', 'first person, alternate eyes again'),
]
DUMP_COUNT = 4
# The screen-fixed HUD's opaque item boxes (top left), fractions of the frame.
HUD_BOXES = ((0.06, 0.09, 0.14, 0.17),)


def run_game(args, out):
    pad, head, control = out / 'pad', out / 'head.txt', out / 'stereo.txt'
    trigger, exit_file, dump = out / 'dump.trigger', out / 'exit', out / 'dump'
    trace = out / 'trace.csv'
    if dump.exists():
        shutil.rmtree(dump)
    dump.mkdir(parents=True)
    for f in (trigger, exit_file, trace):
        f.unlink(missing_ok=True)
    pad.write_text('')
    head.write_text('yaw=0\n')
    control.write_text('alt=1\n')
    user = Path(args.user) if args.user else out / 'user'
    if not args.user:
        if user.exists():
            shutil.rmtree(user)
        shutil.copytree(PORT / 'user', user)
    data = out / 'data'
    data.mkdir(exist_ok=True)
    env = dict(os.environ)
    for key in ('BB_VR', 'BB_XR_MODE', 'BB_VR_HEAD_FOV', 'BB_VR_HEAD_POS', 'BB_VR_HEAD_LEVEL', 'BB_VR_FP',
                'BB_VR_SNAP_DEG', 'BB_PATCHES', 'BB_UPSCALER', 'BB_FPS_LIMIT'):
        env.pop(key, None)
    env.update(BB_HOOKS=HOOKS + (';VR FP Camera Heading (in-game step)' if args.fp else ''), BB_VR_HEAD='1', BB_VR_HEAD_LEVEL='1', BB_VR_FAKE_HEAD='@' + str(head),
               BB_XR_MODE='alt', BB_VR_STEREO_LOG='1', BB_VR_STEREO_CONTROL=str(control),
               BB_VR_STEREO_TRACE=str(trace), BB_PAD_FILE=str(pad), BB_FRAME_STATS='1',
               BB_FRAME_DUMP_TRIGGER=str(trigger), BB_FRAME_DUMP_COUNT=str(DUMP_COUNT),
               BB_DUMP_DIR=str(dump), BB_EXIT_FILE=str(exit_file),
               BB_EXIT_AFTER_SECONDS=str(args.timeout), BB_USER_DIR=str(user), BB_DATA_DIR=str(data),
               BB_CONFIG=str(data / 'bbport.ini'), BB_PAD_KEYBOARD='0')
    if args.fp:
        env.update(BB_VR_FP='1', BB_VR_SNAP_DEG='0')
    if args.effects in ('off', 'upscaler'):
        env['BB_PATCHES'] = EFFECTS_OFF
    if args.effects == 'off':
        env['BB_UPSCALER'] = 'off'  # no temporal AA (BB_UPSCALER=none would stop the camera tracking)
    if args.fps:
        env['BB_FPS_LIMIT'] = args.fps
    if args.game_dir:
        env['BB_GAME_DIR'] = args.game_dir
    if args.probe:
        env['BB_PROBE'] = str(Path(args.probe).resolve())
    log = out / 'session.log'
    print(f'starting the game (log {log})')
    with open(log, 'wb') as sink:
        process = subprocess.Popen([sys.executable, str(PORT / 'run.py')], cwd=PORT, env=env,
                                   stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT)
    captured = []

    def capture(label, count=DUMP_COUNT):
        before = set(dump.glob('output_*.raw'))
        trigger.write_text('')
        wait_for(lambda: len(set(dump.glob('output_*.raw')) - before) >= count, 30, process,
                 f'the {label} capture')
        time.sleep(1.5)
        new = sorted(set(dump.glob('output_*.raw')) - before)[:count]
        captured.append((label, [n.name for n in new]))
        print(f'captured {label}: {", ".join(n.name for n in new)}')

    try:
        wait_for(lambda: log_text(log).count('Frame stats') >= 3 and 'pad opened' in log_text(log),
                 300, process, 'the title screen')
        time.sleep(args.title_wait)
        press(pad, 'cross')
        time.sleep(6)
        press(pad, 'cross')
        wait_for(lambda: draws_per_frame(log_text(log)) > 600, 240, process, 'the world (draws/frame > 600)')
        time.sleep(10)
        steps = FP_STEPS if args.fp else STEPS
        if args.only:
            steps = [step for step in steps if step[0] in args.only.split(',')]
        for index, (label, head_text, control_text, _) in enumerate(steps):
            head.write_text(f'{head_text} step={index}\n')
            control.write_text(f'{control_text} step={index}\n')  # a change per step: re-read
            time.sleep(args.settle)
            capture(label)
        if args.long:
            long_run(args, pad, head, control, capture)
        (out / 'captures.txt').write_text(''.join(f'{label} {" ".join(names)}\n' for label, names in captured))
    finally:
        exit_file.write_text('')
        try:
            process.wait(timeout=90)
        except subprocess.TimeoutExpired:
            print('the game did not end within 90 s of BB_EXIT_FILE; it ends itself after '
                  f'BB_EXIT_AFTER_SECONDS={args.timeout}')
    return captured


def long_run(args, pad, head, control, capture):
    """Walk, turn and move the head for args.long seconds; every 20 s stand still and dump a pair."""
    control.write_text('alt=1 long=1\n')
    start = time.time()
    k = 0
    while time.time() - start < args.long:
        # 15 s of moving: walk with the camera turning back and forth, the head sweeping.
        phase_end = time.time() + 15
        while time.time() < phase_end:
            t = time.time() - start
            yaw = 35 * math.sin(t * 0.9)
            pitch = 12 * math.sin(t * 0.55)
            head.write_text(f'yaw={yaw:.2f} pitch={pitch:.2f} roll={4 * math.sin(t * 0.3):.2f} k={k}\n')
            k += 1
            stick = 'ly=0 rx=200' if int(t / 5) % 2 == 0 else 'ly=0 rx=60'
            pad.write_text(stick + '\n')
            time.sleep(0.25)
        pad.write_text('')
        head.write_text(f'yaw=0 pitch=0 k={k}\n')
        time.sleep(2.0)
        capture(f'long_{int(time.time() - start):03d}', count=2)


# ---- analysis ----------------------------------------------------------------------------------

def dump_eyes(text):
    """{dump index: (eye 'L'/'R'/'mono'/'held', seq, side, trace frame)} from the presenter's log."""
    eyes = {}
    for m in re.finditer(r'VR stereo: frame dump (\d+): (eye [LR]|mono|held), seq (\d+), lag (\d+), '
                         r'side (-?\d+)(?:, frame (\d+))?', text):
        eyes[int(m.group(1))] = (m.group(2).replace('eye ', ''), int(m.group(3)), int(m.group(5)),
                                 int(m.group(6)) if m.group(6) else 0)
    return eyes


def disparity(left, right, block=16, search=128, scale=2):
    """Per-block horizontal disparity x_L - x_R (full-size pixels) of grey images, by normalized
    cross-correlation over +-search full-size pixels; returns (disparity, confidence) grids."""
    import numpy as np
    a = left[::scale, ::scale].astype(np.float64)
    b = right[::scale, ::scale].astype(np.float64)
    h, w = a.shape
    s_max = search // scale
    gh, gw = h // block, w // block
    hh, ww = gh * block, gw * block

    def block_sum(img):
        return img[:hh, :ww].reshape(gh, block, gw, block).sum(axis=(1, 3))
    n = float(block * block)
    sa, saa = block_sum(a), block_sum(a * a)
    var_a = saa - sa * sa / n
    best = np.full((gh, gw), -2.0)
    best_s = np.zeros((gh, gw))
    for s in range(-s_max, s_max + 1):
        # b shifted so that b_s(x) = b(x + s); columns that leave the image are invalid.
        bs = np.zeros_like(b)
        valid = np.zeros_like(b)
        if s >= 0:
            bs[:, :w - s] = b[:, s:]
            valid[:, :w - s] = 1.0
        else:
            bs[:, -s:] = b[:, :w + s]
            valid[:, -s:] = 1.0
        sb, sbb, sab = block_sum(bs), block_sum(bs * bs), block_sum(a * bs)
        ok = block_sum(valid) == n
        var_b = sbb - sb * sb / n
        ncc = (sab - sa * sb / n) / np.sqrt(np.maximum(var_a * var_b, 1e-9))
        ncc = np.where(ok, ncc, -2.0)
        better = ncc > best
        best = np.where(better, ncc, best)
        best_s = np.where(better, s, best_s)
    # s = x_R - x_L for the content of the left block: disparity x_L - x_R = -s.
    texture = var_a / n > 25.0  # grey levels: flat blocks (sky, black) say nothing
    return -best_s * scale, np.where(texture, best, -2.0)


def region_median(disp, conf, box):
    import numpy as np
    gh, gw = disp.shape
    ys, xs = np.mgrid[0:gh, 0:gw]
    x0, y0, x1, y1 = box
    sel = (conf > 0.85) & (xs >= x0 * gw) & (xs < x1 * gw) & (ys >= y0 * gh) & (ys < y1 * gh)
    return (float(np.median(disp[sel])), int(sel.sum())) if sel.sum() >= 3 else (float('nan'), 0)


def stats_of(disp, conf):
    import numpy as np
    good = conf > 0.85
    values = disp[good]
    if values.size < 20:
        return None
    hud = [region_median(disp, conf, box) for box in HUD_BOXES]
    hud_values = [v for v, n in hud if n]
    return {
        'blocks': int(values.size),
        'p5': float(np.percentile(values, 5)), 'p50': float(np.percentile(values, 50)),
        'p95': float(np.percentile(values, 95)),
        'near': region_median(disp, conf, (0.35, 0.45, 0.65, 0.95))[0],
        'top': region_median(disp, conf, (0.20, 0.0, 0.80, 0.35))[0],
        'hud': max(hud_values, key=abs) if hud_values else float('nan'),
        'shifted': float((np.abs(values) >= 4).mean()),
    }


def save_pngs(png, label, left_rgb, right_rgb, disp, conf):
    import numpy as np
    from PIL import Image

    def half(rgb):
        return Image.fromarray(rgb.clip(0, 255).astype(np.uint8)).resize((rgb.shape[1] // 2, rgb.shape[0] // 2))
    lh, rh = half(left_rgb), half(right_rgb)
    sbs = Image.new('RGB', (lh.width * 2, lh.height))
    sbs.paste(lh, (0, 0))
    sbs.paste(rh, (lh.width, 0))
    sbs.save(png / f'{label}_sbs.png')
    gl = np.asarray(lh.convert('L'), dtype=np.uint8)
    gr = np.asarray(rh.convert('L'), dtype=np.uint8)
    Image.fromarray(np.stack([gl, gr, gr], axis=2)).save(png / f'{label}_anaglyph.png')
    # Disparity map: red = positive (near), blue = negative, grey = 0, dark = no confident match.
    m = np.clip(disp / 24.0, -1, 1)
    rgb = np.zeros(disp.shape + (3,), dtype=np.uint8)
    rgb[..., 0] = np.where(m > 0, 80 + 175 * m, 80)
    rgb[..., 2] = np.where(m < 0, 80 - 175 * m, 80)
    rgb[..., 1] = 80
    rgb[conf <= 0.85] = (40, 40, 40)
    Image.fromarray(rgb).resize((disp.shape[1] * 8, disp.shape[0] * 8), Image.NEAREST).save(png / f'{label}_disparity.png')


def read_trace(path):
    rows = []
    if not path.is_file():
        return rows
    lines = path.read_text().splitlines()
    names = lines[0].split(',')
    for line in lines[1:]:
        parts = line.split(',')
        if len(parts) == len(names):
            rows.append(dict(zip(names, parts)))
    return rows


def step_trace_stats(rows, frame, before=90):
    """Draw-count changes and shadow-cascade changes over the `before` frames up to trace frame
    `frame` (the step's settle time, a still scene): with alternating eyes consecutive frames are
    L/R pairs (s1 the other eye, s2 the same eye two frames apart)."""
    window = [r for r in rows if frame - before <= int(r['frame']) < frame]
    if len(window) < 10:
        return None
    draws = [int(r['draws']) for r in window]
    ddiff = [abs(a - b) for a, b in zip(draws, draws[1:])]
    out = {'frames': len(window), 'draws': sum(draws) / len(draws), 'ddiff': sum(ddiff) / len(ddiff),
           'ddiff_max': max(ddiff), 'kinds': {k: sum(1 for r in window if r['kind'] == k) for k in ('eye', 'mono', 'hold')}}
    if 's1' in window[0]:
        s1 = sorted(float(r['s1']) for r in window if float(r['s1']) >= 0)
        s2 = sorted(float(r['s2']) for r in window if float(r['s2']) >= 0)
        out['s1'] = s1[len(s1) // 2] if s1 else float('nan')
        out['s2'] = s2[len(s2) // 2] if s2 else float('nan')
        out['s1_max'] = s1[-1] if s1 else float('nan')
        out['s2_max'] = s2[-1] if s2 else float('nan')
    return out


def analyze_trace(rows, invert_windows):
    """The per-frame trace: every eye frame on its eye's side, the alternation, no repeats."""
    lines = []
    eye_rows = [r for r in rows if r['kind'] == 'eye']
    if not eye_rows:
        return ['trace: no frame went to an eye'], False

    def inverted(t):
        return any(a <= t <= b for a, b in invert_windows)
    wrong_side = sum(1 for r in eye_rows if int(r['side']) not in (0, 1 if r['eye'] == '1' else -1)
                     and not inverted(float(r['time'])))
    inverted_frames = [r for r in eye_rows if inverted(float(r['time']))]
    inverted_flagged = sum(1 for r in inverted_frames if int(r['side']) == (-1 if r['eye'] == '1' else 1))
    seqs = [int(r['seq']) for r in eye_rows]
    not_increasing = sum(1 for a, b in zip(seqs, seqs[1:]) if b <= a)
    parity_breaks = sum(1 for r in eye_rows if int(r['eye']) != (int(r['seq']) - 1) % 2)
    runs, run = [], 1
    for a, b in zip(eye_rows, eye_rows[1:]):
        if a['eye'] == b['eye']:
            run += 1
        else:
            runs.append(run)
            run = 1
    runs.append(run)
    held = [r for r in rows if r['kind'] == 'hold']
    mono = [r for r in rows if r['kind'] == 'mono']
    reasons = {}
    for r in held + mono:
        reasons[r['reason']] = reasons.get(r['reason'], 0) + 1
    lags = sorted(int(r['lag']) for r in eye_rows)
    left = sum(1 for r in eye_rows if r['eye'] == '0')
    duration = float(rows[-1]['time']) - float(rows[0]['time'])
    lines.append(f'trace: {len(rows)} frames in {duration:.0f} s, {len(eye_rows)} to an eye (L {left}, '
                 f'R {len(eye_rows) - left}), held {len(held)}, mono {len(mono)} {reasons}')
    lines.append(f'trace: eye frames on the wrong side of the center camera: {wrong_side} (must be 0); '
                 f'seq not increasing: {not_increasing} (must be 0); eye != seq parity: {parity_breaks} '
                 f'(guard shifts); same eye twice in a row: {sum(1 for r in runs if r > 1)} times, '
                 f'longest run {max(runs)}; camera-to-flip lag median {lags[len(lags) // 2]} game frames')
    if inverted_frames:
        lines.append(f'trace: negative control (invert=1): {inverted_flagged} of {len(inverted_frames)} eye frames '
                     f'flagged by the side check')
    ok = wrong_side == 0 and not_increasing == 0 and max(runs) <= 3 and \
        (not inverted_frames or inverted_flagged >= 0.9 * len(inverted_frames))
    return lines, ok


def box_blur(img, radius=2, passes=3):
    import numpy as np
    out = img.astype(np.float64)
    k = 2 * radius + 1
    for _ in range(passes):
        for axis in (0, 1):
            pad = [(0, 0), (0, 0)]
            pad[axis] = (radius + 1, radius)
            c = np.cumsum(np.pad(out, pad, mode='edge'), axis=axis)
            out = (np.take(c, range(k, c.shape[axis]), axis=axis) - np.take(c, range(0, c.shape[axis] - k), axis=axis)) / k
    return out


def offaxis_check(results, png):
    """The off-axis renders against the symmetric ones of the same eye and view: resample the
    symmetric image onto the off-axis frustum's columns; geometry must line up (no shift) and the
    shading must agree (lights, fog and AO reconstructed from depth with the shifted projection).
    The screen-fixed HUD boxes and a 3% margin are left out of the shading difference. Returns the
    report lines, geometry ok, shading ok (None, None without the two steps)."""
    import numpy as np
    from PIL import Image
    if 'axis_sym' not in results or 'axis_off' not in results:
        return [], None, None
    ty = math.tan(math.radians(89.1 / 2))
    T = ty * 1.4281
    el, er = math.tan(math.radians(-54.57)), math.tan(math.radians(46.0))
    lines, geometry_ok, shading_ok = [], True, True
    for eye, (l, r) in (('L', (el, er)), ('R', (-er, -el))):
        sym = [f for f in results['axis_sym'][0] if f[1][0] == eye]
        off = [f for f in results['axis_off'][0] if f[1][0] == eye]
        if len(sym) < 2 or len(off) < 2:
            lines.append(f'off-axis {eye}: frames missing')
            geometry_ok = shading_ok = False
            continue
        h, w = off[0][3].shape
        hw, c = 0.5 * (r - l), 0.5 * (r + l)
        x = (np.arange(w) + 0.5) / w * 2 - 1           # off-axis NDC of each column
        t = c + hw * x                                   # its tangent
        xs = (t / T + 1) * w / 2 - 0.5                   # the symmetric image's column
        x0 = np.clip(np.floor(xs).astype(int), 0, w - 2)
        fr = (xs - x0)[None, :, None]
        src = sym[0][2]
        pred = src[:, x0] * (1 - fr) + src[:, x0 + 1] * fr
        margin = int(0.03 * w)
        pg, og = pred.mean(axis=2), off[0][2].mean(axis=2)
        full = np.abs(box_blur(pg) - box_blur(og))
        diff = full[:, margin:w - margin]
        keep = np.ones_like(full, dtype=bool)
        keep[:, :margin] = keep[:, w - margin:] = False
        keep[: int(0.12 * h), : int(0.10 * w)] = False   # health, items (top left)
        keep[: int(0.13 * h), int(0.88 * w):] = False    # echoes, insight (top right)
        noise = np.abs(box_blur(sym[0][3]) - box_blur(sym[1][3])).mean()
        mad = float(full[keep].mean())
        low = box_blur(og, radius=12) - box_blur(pg, radius=12)   # shading (render - resampled), low frequency
        top, bottom = float(low[: h // 2][keep[: h // 2]].mean()), float(low[h // 2:][keep[h // 2:]].mean())
        blocks = diff[: (diff.shape[0] // 32) * 32, : (diff.shape[1] // 32) * 32]
        blocks = blocks.reshape(blocks.shape[0] // 32, 32, blocks.shape[1] // 32, 32).mean(axis=(1, 3))
        disp, conf = disparity(pred.mean(axis=2), off[0][3], search=48)
        st = stats_of(disp, conf)
        lines.append(f'off-axis {eye}: residual shift p5/p50/p95 {fmt(st["p5"]) if st else "n/a"} / '
                     f'{fmt(st["p50"]) if st else "n/a"} / {fmt(st["p95"]) if st else "n/a"} px; resampled symmetric vs '
                     f'off-axis render differ by {mad:.2f} (0..255, blurred, HUD excluded), worst 32 px block '
                     f'{blocks.max():.1f}, 99th percentile block {np.percentile(blocks, 99):.1f}; same-eye symmetric '
                     f'frames {noise:.2f}; low-frequency shading (render - resampled) top half {top:+.2f}, bottom half '
                     f'{bottom:+.2f}')
        geometry_ok = geometry_ok and st is not None and abs(st['p5']) <= 2 and abs(st['p95']) <= 2
        shading_ok = shading_ok and mad < 4.0
        heat = np.clip(blocks / 20.0, 0, 1)
        rgb = np.stack([80 + 175 * heat, 80 * (1 - heat), 80 * (1 - heat)], axis=2).astype(np.uint8)
        Image.fromarray(rgb).resize((blocks.shape[1] * 8, blocks.shape[0] * 8), Image.NEAREST).save(png / f'offaxis_{eye}_diff.png')
        both = np.concatenate([pred, off[0][2]], axis=1)
        Image.fromarray(both.clip(0, 255).astype(np.uint8)).resize((w, h // 2)).save(png / f'offaxis_{eye}_pred_vs_render.png')
        Image.fromarray(np.clip(128 + low * 8, 0, 255).astype(np.uint8)).save(png / f'offaxis_{eye}_shading_x8.png')
    return lines, geometry_ok, shading_ok


def pacing(rows, display_hz=72.0):
    """From the trace's flip times: each eye's update intervals, and a display at display_hz
    sampling the newest image of each eye (its age in display frames), as the headset shows it
    (the presenter's copy and the compositor's latency left out: a constant shift)."""
    eye_rows = [(float(r['time']), int(r['eye'])) for r in rows if r['kind'] == 'eye']
    if len(eye_rows) < 100:
        return []
    # The longest stretch of eye frames without a gap over 0.25 s (one steady step or run).
    best, start = (0, 0), 0
    for i in range(1, len(eye_rows) + 1):
        if i == len(eye_rows) or eye_rows[i][0] - eye_rows[i - 1][0] > 0.25:
            if i - start > best[1] - best[0]:
                best = (start, i)
            start = i
    seg = eye_rows[best[0]:best[1]]
    if len(seg) < 100:
        return []
    t0, t1 = seg[0][0], seg[-1][0]
    fps = (len(seg) - 1) / (t1 - t0)
    gaps = [b[0] - a[0] for a, b in zip(seg, seg[1:])]
    per_eye = {}
    for e in (0, 1):
        times = [t for t, eye in seg if eye == e]
        iv = sorted(b - a for a, b in zip(times, times[1:]))
        per_eye[e] = (len(times) / (t1 - t0), iv[len(iv) // 2] * 1000, iv[int(len(iv) * 0.99)] * 1000)
    hist = {0: [0] * 6, 1: [0] * 6}
    both_stale = n = 0
    newest = {0: None, 1: None}
    j = 0
    t = t0 + 0.1
    while t < t1:
        while j < len(seg) and seg[j][0] <= t:
            newest[seg[j][1]] = seg[j][0]
            j += 1
        if newest[0] is not None and newest[1] is not None:
            ages = [int((t - newest[e]) * display_hz) for e in (0, 1)]
            for e in (0, 1):
                hist[e][min(ages[e], 5)] += 1
            both_stale += ages[0] > 0 and ages[1] > 0
            n += 1
        t += 1.0 / display_hz
    lines = [f'pacing ({t1 - t0:.0f} s of eye frames): game {fps:.1f} FPS, frame interval median '
             f'{sorted(gaps)[len(gaps) // 2] * 1000:.1f} ms p99 {sorted(gaps)[int(len(gaps) * 0.99)] * 1000:.1f} ms; '
             f'each eye {per_eye[0][0]:.1f} / {per_eye[1][0]:.1f} Hz, its interval median {per_eye[0][1]:.1f} / '
             f'{per_eye[1][1]:.1f} ms, p99 {per_eye[0][2]:.1f} / {per_eye[1][2]:.1f} ms']
    for e in (0, 1):
        total = sum(hist[e]) or 1
        mean = sum(i * c for i, c in enumerate(hist[e])) / total
        lines.append(f'pacing at a {display_hz:.0f} Hz display, eye {"LR"[e]}: image age in display frames 0/1/2/3/4/5+ = '
                     + '/'.join(f'{100 * c / total:.0f}%' for c in hist[e]) + f', mean {mean:.2f}')
    lines.append(f'pacing: both eyes older than one display frame in {100 * both_stale / max(n, 1):.0f}% of display frames')
    return lines


def invert_windows_from(rows, text):
    """The trace time spans of the invert step: the trace has no step marks, so take the frames
    whose side check failed in a block of a second or more (a real swap would be scattered)."""
    if 'invert 1' not in text:
        return []
    windows = []
    start = None
    for r in rows:
        if r['kind'] != 'eye':
            continue
        bad = int(r['side']) == (-1 if r['eye'] == '1' else 1)
        t = float(r['time'])
        if bad and start is None:
            start = t
        elif not bad and start is not None:
            if t - start > 1.0:
                windows.append((start - 0.05, t))
            start = None
    if start is not None and rows:
        windows.append((start - 0.05, float(rows[-1]['time'])))
    return windows


def fmt(v, spec='+6.1f'):
    return 'n/a' if v is None or (isinstance(v, float) and math.isnan(v)) else format(v, spec)


def analyze(out):
    captures = [line.split() for line in (out / 'captures.txt').read_text().splitlines() if line.strip()]
    text = log_text(out / 'session.log')
    eyes = dump_eyes(text)
    rows = read_trace(out / 'trace.csv')
    png = out / 'png'
    png.mkdir(exist_ok=True)
    describe = {s[0]: s[3] for s in STEPS + FP_STEPS}
    lines = ['VR alternate-eye stereo test (flat): disparity x_L - x_R in full-size pixels, block NCC',
             '(near = the hunter region, center-bottom; top = the upper middle; hud = the screen-fixed HUD;',
             ' p5/p50/p95 over all confident blocks; shifted = blocks moving 4+ px)', '']
    results, traces, stability = {}, {}, {}
    size = None
    for cap in captures:
        label, names = cap[0], cap[1:]
        frames = []
        for name in names:
            index = int(re.search(r'output_(\d+)_', name).group(1))
            rgb, gray = load_gray(out / 'dump' / name)
            size = gray.shape
            frames.append((index, eyes.get(index, ('?', 0, 0, 0)), rgb, gray))
        tags = ' '.join(f'{f[1][0]}{f[1][1]}' for f in frames)
        lines.append(f'{label}: {describe.get(label, "long run: after walking and turning")}; frames {tags}')
        pairs = []
        for (ia, ea, ra, ga), (ib, eb, rb, gb) in zip(frames, frames[1:]):
            if {ea[0], eb[0]} == {'L', 'R'}:
                (lrgb, lg), (rrgb, rg) = ((ra, ga), (rb, gb)) if ea[0] == 'L' else ((rb, gb), (ra, ga))
                disp, conf = disparity(lg, rg)
                pairs.append(('LR', stats_of(disp, conf)))
                if sum(1 for k, _ in pairs if k == 'LR') == 1:
                    save_pngs(png, label, lrgb, rrgb, disp, conf)
            elif ea[0] == eb[0]:
                disp, conf = disparity(ga, gb)
                pairs.append((ea[0] + ea[0], stats_of(disp, conf)))
                if ea[0] == 'mono' and len(pairs) == 1:
                    save_pngs(png, label, ra, rb, disp, conf)
        for (ia, ea, ra, ga), (ic, ec, rc, gc) in zip(frames, frames[2:]):
            if ea[0] == ec[0] and ea[0] in 'LR':
                disp, conf = disparity(ga, gc)
                pairs.append((ea[0] + '..' + ec[0], stats_of(disp, conf)))
        for kind, st in pairs:
            if st:
                lines.append(f'    {kind:9s} near {fmt(st["near"])}  top {fmt(st["top"])}  hud {fmt(st["hud"])}  '
                             f'p5 {fmt(st["p5"])} p50 {fmt(st["p50"])} p95 {fmt(st["p95"])}  shifted '
                             f'{100 * st["shifted"]:4.1f}% ({st["blocks"]} blocks)')
            else:
                lines.append(f'    {kind:9s} too few confident blocks')
        # Temporal stability and sharpness: what temporal effects (TAA history, motion blur) do
        # when the previous frame is the other eye's. Same-eye frames two apart (or consecutive
        # mono frames) of a still scene should be identical; sharpness = mean |gradient|.
        import numpy as np
        stable = [float(np.abs(fa[2] - fc[2]).mean()) for fa, fc in zip(frames, frames[2:])
                  if fa[1][0] == fc[1][0]]
        consecutive = [float(np.abs(fa[2] - fb[2]).mean()) for fa, fb in zip(frames, frames[1:])]
        sharp = [float(np.abs(np.diff(f[3], axis=1)).mean() + np.abs(np.diff(f[3], axis=0)).mean()) for f in frames]
        stability[label] = (stable, consecutive, sharp)
        lines.append(f'    frames two apart differ by {fmt(sum(stable) / len(stable) if stable else float("nan"), ".2f")} '
                     f'(0..255), consecutive by {fmt(sum(consecutive) / len(consecutive), ".2f")}; sharpness '
                     f'{sum(sharp) / len(sharp):.2f}')
        if rows and frames and frames[0][1][3]:
            ts = step_trace_stats(rows, frames[0][1][3])
            traces[label] = ts
            if ts:
                lines.append(f'    trace before the dump ({ts["frames"]} frames {ts["kinds"]}): draws/frame {ts["draws"]:.1f}, '
                             f'consecutive frames differ by {ts["ddiff"]:.2f} draws (max {ts["ddiff_max"]}); '
                             f'shadow cascades vs the previous frame {fmt(ts.get("s1"), ".3g")} (max {fmt(ts.get("s1_max"), ".3g")}), '
                             f'vs two frames back {fmt(ts.get("s2"), ".3g")} (max {fmt(ts.get("s2_max"), ".3g")})')
        results[label] = (frames, pairs)
    if size:
        f_px = (size[1] / 2) / (math.tan(math.radians(21.5)) * 1.7778)
        lines += ['', f'frame {size[1]}x{size[0]}: f = {f_px:.0f} px for the 43 deg FOV; a point 4 m away should '
                      f'shift {f_px * 0.063 / 4:.1f} px at IPD 63 mm ({f_px * 0.25 / 4:.1f} px at 250 mm), '
                      f'one 50 m away {f_px * 0.063 / 50:.1f} px']

    def lr(label):
        return [st for kind, st in results.get(label, ([], []))[1] if kind == 'LR' and st]

    def same(label):
        return [st for kind, st in results.get(label, ([], []))[1] if '..' in kind and st]

    def avg(values):
        values = [v for v in values if not math.isnan(v)]
        return sum(values) / len(values) if values else float('nan')
    checks = []
    fp = any(label.startswith('fp_') for label in results)
    if fp:
        return analyze_fp(out, lines, results, stability, lr, same, rows, text)
    alt = lr('alt') + lr('alt_end')
    checks.append(('L/R pairs: near content shifts right in the left eye (near > 3 px)',
                   bool(alt) and all(s['near'] > 3 for s in alt)))
    checks.append(('L/R pairs: no content moves the wrong way (p5 > -2 px)', bool(alt) and all(s['p5'] > -2 for s in alt)))
    allpairs = [st for label in results for st in lr(label) if not label.startswith('invert')]
    checks.append(('L/R pairs: the screen-fixed HUD does not shift (|hud| <= 1 px)',
                   bool(allpairs) and all(abs(s['hud']) <= 1 or math.isnan(s['hud']) for s in allpairs)))
    far = lr('alt_far')
    checks.append(('looking across the garden: distant content shifts little (p5 < 3 px) while near content '
                   'still shifts right (p95 > 3 px)', bool(far) and all(s['p5'] < 3 and s['p95'] > 3 for s in far)))
    sm = same('alt') + same('alt_end') + same('alt_ipd250') + same('alt_far')
    checks.append(('same-eye frames two apart do not shift (|p5|,|p95| < 2 px)',
                   bool(sm) and all(abs(s['p5']) < 2 and abs(s['p95']) < 2 for s in sm)))
    big = lr('alt_ipd250')
    ratio = avg([s['near'] for s in big]) / max(0.1, avg([s['near'] for s in alt])) if big and alt else float('nan')
    checks.append((f'IPD 250 mm: near disparity x{fmt(ratio, ".2f")} the 63 mm one (expected 3.97)',
                   not math.isnan(ratio) and 3.3 < ratio < 4.7))
    mono_pairs = [st for label in ('mono', 'mono_far') for kind, st in results.get(label, ([], []))[1] if st]
    checks.append(('mono camera: consecutive frames do not shift (|p5|,|p95| < 1.5 px)',
                   bool(mono_pairs) and all(abs(s['p5']) < 1.5 and abs(s['p95']) < 1.5 for s in mono_pairs)))
    inv = lr('invert')
    checks.append(('negative control: invert=1 flips the disparity (near < -3 px)', bool(inv) and all(s['near'] < -3 for s in inv)))
    hmd = lr('alt_hmd')
    checks.append(('headset frustum: near content still shifts right (near > 1 px)', bool(hmd) and all(s['near'] > 1 for s in hmd)))
    notag = lr('notag')
    checks.append(('no eye tag: pairing by the camera alone still gives L/R pairs with near > 3 px',
                   bool(notag) and all(s['near'] > 3 for s in notag)))
    longs = [label for label in results if label.startswith('long_')]
    if longs:
        lpairs = [st for label in longs for st in lr(label)]
        checks.append((f'long run: every dumped L/R pair has near content shifting right ({len(lpairs)} pairs)',
                       bool(lpairs) and all(s['near'] > 2 or (math.isnan(s['near']) and s['p95'] > 2) for s in lpairs)))
    # Culling/LOD and shadows: alternating eyes against the mono camera in the same views.
    for a, m in (('alt', 'mono'), ('alt_far', 'mono_far')):
        sa, smo = stability.get(a), stability.get(m)
        if sa and smo and sa[0] and smo[1]:
            lines.append(f'{a} vs {m}: same-eye frames two apart differ by {avg(sa[0]):.2f} (0..255) vs consecutive '
                         f'mono frames {avg(smo[1]):.2f}; sharpness {avg(sa[2]):.2f} vs {avg(smo[2]):.2f}')
        ta, tm = traces.get(a), traces.get(m)
        if ta and tm:
            lines.append(f'{a} vs {m}: consecutive frames differ by {ta["ddiff"]:.2f} vs {tm["ddiff"]:.2f} draws; '
                         f'shadow cascades {fmt(ta.get("s1"), ".3g")} (L/R) vs {fmt(tm.get("s1"), ".3g")} (mono)')
    trace_lines, trace_ok = analyze_trace(rows, invert_windows_from(rows, text)) if rows else (['trace: none'], False)
    checks.append(('trace: every eye frame on its eye\'s side, seq increasing, runs <= 3', trace_ok))
    axis_lines, axis_geometry, axis_shading = offaxis_check(results, png)
    if axis_geometry is not None:
        ta = traces.get('axis_off')
        paired = bool(ta) and ta['kinds'].get('eye', 0) == ta['frames']
        checks.append(('off-axis frusta: geometry lines up with the symmetric render resampled (residual shift within '
                       '2 px) and every frame paired', axis_geometry and paired))
        checks.append(('off-axis frusta: shading within noise of the symmetric render resampled (blurred difference '
                       '< 4, HUD excluded)', axis_shading))
        hooks = re.findall(r'VR stereo: off-axis: first .*', text)
        axis_lines += [f'    {h}' for h in hooks[:2]]
    for a, b in (('alt', 'h2eye'),):
        ta, tb = traces.get(a), traces.get(b)
        if ta and tb:
            axis_lines.append(f'main context at the center ({a}) vs at the eye ({b}): consecutive L/R frames differ by '
                              f'{ta["ddiff"]:.2f} vs {tb["ddiff"]:.2f} draws (max {ta["ddiff_max"]} vs {tb["ddiff_max"]})')
    lines += [''] + trace_lines + pacing(rows) + axis_lines + ['']
    summaries = re.findall(r'VR stereo: \d+ frames: .*', text)
    if summaries:
        lines += ['presenter summaries (10 s windows), the last three:'] + [f'    {s}' for s in summaries[-3:]]
    draws = re.findall(r'VR stereo:   draws/frame .*', text)
    if draws:
        lines += ['culling/LOD (draw counts of consecutive L/R frames), the last four windows:'] + [f'    {d}' for d in draws[-4:]]
    lines += ['', 'CHECKS:'] + [f'    {"PASS" if ok else "FAIL"}  {what}' for what, ok in checks]
    passed = all(ok for _, ok in checks)
    lines.append(f'RESULT: {"all checks pass" if passed else "some checks FAIL"}')
    report = '\n'.join(lines) + '\n'
    (out / 'report.txt').write_text(report)
    print(report)
    return passed


def analyze_fp(out, lines, results, stability, lr, same, rows, text):
    checks = []
    alt = lr('fp_alt') + lr('fp_alt_down') + lr('fp_alt_end')
    checks.append(('first person L/R pairs: near content shifts right in the left eye (p95 > 3 px, p5 > -2 px)',
                   bool(alt) and all(s['p95'] > 3 and s['p5'] > -2 for s in alt)))
    sm = same('fp_alt') + same('fp_alt_down') + same('fp_alt_end')
    checks.append(('first person same-eye frames two apart do not shift (|p5|,|p95| < 2 px)',
                   bool(sm) and all(abs(s['p5']) < 2 and abs(s['p95']) < 2 for s in sm)))
    mono_pairs = [st for kind, st in results.get('fp_mono', ([], []))[1] if st]
    checks.append(('first person mono camera: consecutive frames do not shift (|p5|,|p95| < 1.5 px)',
                   bool(mono_pairs) and all(abs(s['p5']) < 1.5 and abs(s['p95']) < 1.5 for s in mono_pairs)))
    inv = lr('fp_invert')
    checks.append(('first person negative control: invert=1 flips the disparity (p5 < -3 px)',
                   bool(inv) and all(s['p5'] < -3 and s['p95'] < 2 for s in inv)))
    trace_lines, trace_ok = analyze_trace(rows, invert_windows_from(rows, text)) if rows else (['trace: none'], False)
    checks.append(("trace: every eye frame on its eye's side, seq increasing, runs <= 3", trace_ok))
    fp_lines = re.findall(r'VR FP: (?:first person|third person|no player).*', text)
    lines += [''] + trace_lines + pacing(rows) + ['', 'first person log (the last two):']
    lines += [f'    {line}' for line in fp_lines[-2:]] + ['', 'CHECKS:']
    lines += [f'    {"PASS" if ok else "FAIL"}  {what}' for what, ok in checks]
    passed = all(ok for _, ok in checks)
    lines.append(f'RESULT: {"all checks pass" if passed else "some checks FAIL"}')
    report = '\n'.join(lines) + '\n'
    (out / 'report.txt').write_text(report)
    print(report)
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=PORT / 'out' / 'vr_stereo_test', help='results folder')
    parser.add_argument('--probe', help='bb-probe.exe to run (default: run.py\'s choice)')
    parser.add_argument('--game-dir', help='BB_GAME_DIR (the user\'s dump; only read)')
    parser.add_argument('--user', help='user folder to run on (default: a copy of user/ in --out)')
    parser.add_argument('--timeout', type=int, default=900, help='BB_EXIT_AFTER_SECONDS safety net')
    parser.add_argument('--title-wait', type=float, default=6.0, help='seconds at the title screen')
    parser.add_argument('--settle', type=float, default=2.0, help='seconds between a step and its capture')
    parser.add_argument('--long', type=int, default=0, help='seconds of walking/turning with periodic dumps')
    parser.add_argument('--effects', choices=('off', 'upscaler', 'default'), default='off',
                        help='off: motion blur, game AA, DoF, CA patched off and BB_UPSCALER=off; '
                             'upscaler: the same patches, the port upscaler as configured (FSR 3); '
                             'default: the game effects and the port upscaler')
    parser.add_argument('--fps', help='BB_FPS_LIMIT for the run (e.g. 60 or 72)')
    parser.add_argument('--fp', action='store_true', help='first person (BB_VR_FP=1, the FP heading hook)')
    parser.add_argument('--only', help='comma-separated step labels to run (default: all)')
    parser.add_argument('--analyze-only', action='store_true', help='analyze an earlier run')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not args.analyze_only:
        run_game(args, out)
    return 0 if analyze(out) else 1


if __name__ == '__main__':
    sys.exit(main())
