#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Headset-free checks of Phase 4 true stereo (BB_XR_MODE=stereo, docs/vr/findings/31-true-stereo.md).
The game runs flat (BB_VR=0, fake head) on a copy of user/ with its own data folder, like
tools/vr_stereo_poc.py, whose session helpers it reuses, and ends itself.

    python tools/vr_true_stereo_test.py replay [--ipd 0.063] [--out DIR] [--probe EXE]
    python tools/vr_true_stereo_test.py cost   [--out DIR] [--probe EXE] [--seconds 15]
    python tools/vr_true_stereo_test.py oracle [--offset 0.0315] [--out DIR] [--probe EXE]
    python tools/vr_true_stereo_test.py warp   [--ipd 0.063] [--out DIR] [--probe EXE]
    python tools/vr_true_stereo_test.py warpcost [--out DIR] [--probe EXE] [--seconds 15]
    python tools/vr_true_stereo_test.py full   [--ipd 0.063] [--out DIR] [--probe EXE]
    python tools/vr_true_stereo_test.py fullcost [--out DIR] [--probe EXE] [--seconds 15]
    python tools/vr_true_stereo_test.py nrplace [--out DIR] [--probe EXE] [--seconds 15]

replay  P4.3, record once / execute twice. One session, three doubled G-buffer passes:
        1. identity: eye R at eye L's position ("same=1"): every eye-R target must equal eye L's
           bit for bit (the replay reproduces the recorded commands exactly);
        2. stereo: eyes at -/+ ipd/2 with the replay: eye R vs eye L warped by depth (as
           vr_stereo_poc.py dup), and the log's counts (passes executed twice, none cut short,
           no draw outside a capture);
        3. the same with the prototype's per-draw second draw ("replay=0"), for comparison;
        4. the write guard's negative control ("guardtest=1"): no pass and no dispatch gets eye R;
        5. identity again with P4.4 slice 3 off ("compute=0"): dispatch capture and eye-R
           descriptors (on in 1, 2 and 4) must not be what makes eye R equal eye L.
cost    GPU time per frame (BB_GPU_PROFILE=1): stereo off in the region, every frame's G-buffer
        doubled per draw (prototype), doubled by record once / execute twice.
oracle  P4.1: tools/vr_stereo_poc.py's oracle (the shift against the CPU camera moved, H1).
warp    Hybrid stereo (findings/31 "Hybrid stereo warp"): eye R's final image warped from eye L's
        HUD-free scene with eye R's own G-buffer depth. One session ("warpdump=<n>" dumps eye L's
        scene and depth, eye R's image and depth twin at the region's end):
        1. same=1: zero eye distance, eye R must equal eye L byte for byte;
        2. eyes at -/+ ipd/2: the depth of the two eyes agrees at x_L = x_R + shift / z, not at
           x_R - shift / z (the sign), eye R's image likewise;
        3. inverted ipd: the shift and the agreeing side flip;
        4. eye R against the oracle: the whole region rendered from the camera moved +ipd/2
           (offset=+ipd/2, hybrid off), residual on static pixels next to the noise (the same
           +ipd/2 image twice) and the signal (eye L, -ipd/2, against it);
        5. the disocclusion fill on its own: the same against the oracle on the hole pixels
           only (eye R's direct taps both fail), G-buffer-guided fill vs the mirrored fill
           ("guided=0", two more dumps).
warpcost GPU time of the eye-R warp (BB_GPU_PROFILE=1, labels "stereo: eye R warp" and "stereo:
        eye R guided hole fill"), hybrid
        on, with the G-buffer-guided hole fill and with the mirrored fill ("guided=0"), A B A B.
full    Full stereo (findings/31 section 10): eye R rendered for real from the G-buffer to the
        display pass (the UI once), its final image the display buffer's twin. One session with
        BB_UPSCALER=off and BB_XR_HUD=force (the display buffers carry no HUD, as in the headset):
        1. same=1: eye R's rendered final image equals eye L's byte for byte (also the hybrid's);
        2. the sign of the disparity, normal and inverted ipd (as warp);
        3. eye R against the oracle (the whole region from the camera moved +ipd/2), next to the
           noise, the signal and the hybrid warp of the same session; also on the hybrid's hole
           pixels. PASS: every flip rendered, identity exact, full below the hybrid and within
           2x the noise.
fullcost GPU time per frame: stereo shaders only, hybrid, full stereo (A B C A B C), with the
        eye-R labels ("stereo: eye R ...").
nrplace DLSS-NR post vs pre-SR (docs/vr/findings/42 section 7): GPU ms and fps with eye R's own
        DLSS at 2880x1620, hybrid and full stereo, DLSS only / NR post / NR pre-SR (A B C x2),
        then frame dumps of each (--env for extra settings, e.g. BB_XR_DLSSNR_PASSES=2).

Needs numpy and Pillow. Results: DIR/report.txt.
"""
import argparse
import re
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vr_stereo_poc as poc  # noqa: E402

PORT = poc.PORT
EXTRA_ENV = {'BB_VR': '0', 'BB_IMAGE_COPY_DEBUG': '1', 'BB_STEREO_WRITE_AUDIT': '1'}


def check_no_other_probe():
    import subprocess
    out = subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower()
    running = sorted({line.split()[0] for line in out.splitlines()
                      if line.startswith('bb-probe') or line.startswith('bb-play')})
    if running:
        raise SystemExit(f'another game is running ({", ".join(running)}): not starting')


def stereo_pairs(folder, frame):
    """{(target index, format, size): {'L': path, 'R': path}} of one doubled pass's dumps."""
    pairs = {}
    for raw in folder.rglob(f'stereo_f{frame}_t*_[LR]_*.raw'):
        m = re.match(r'stereo_f(\d+)_t(\d+)_([LR])_(.+)_(\d+)x(\d+)\.raw$', raw.name)
        if m:
            key = (int(m.group(2)), m.group(4), int(m.group(5)), int(m.group(6)))
            pairs.setdefault(key, {})[m.group(3)] = raw
    return {k: v for k, v in sorted(pairs.items()) if len(v) == 2}


def load(raw, fmt, w, h):
    data = np.fromfile(raw, dtype=np.uint8)
    bpp = len(data) // (w * h)
    return data[:w * h * bpp].reshape(h, w, bpp)


def warp_metrics(pairs, ipd):
    """Eye R's albedo against eye L's warped by depth (vr_stereo_poc.py dup)."""
    alb = {k: v for k, v in pairs.items() if k[1] == 'R8G8B8A8Srgb'}
    dep = {k: v for k, v in pairs.items() if k[1] == 'D32SfloatS8Uint'}
    if not alb or not dep:
        return ['  (no albedo or depth dump)']
    (_, _, w, h), files = next(iter(alb.items()))
    alb_l = load(files['L'], '', w, h)[..., :3].astype(np.float32)
    alb_r = load(files['R'], '', w, h)[..., :3].astype(np.float32)
    a_, b_ = poc.FAR / (poc.FAR - poc.NEAR), -poc.NEAR * poc.FAR / (poc.FAR - poc.NEAR)
    files = next(iter(dep.values()))
    z = {e: b_ / (load(files[e], '', w, h).view(np.float32)[..., 0] - a_) for e in 'LR'}
    ys, xs = np.mgrid[0:h, 0:w]
    valid = (z['R'] > poc.NEAR) & (z['R'] < 2000)
    disparity = poc.P00 * ipd / np.where(valid, z['R'], 1e9) * (w / 2)
    xl = np.clip(np.round(xs + disparity).astype(int), 0, w - 1)
    same = valid & (np.abs(z['L'][ys, xl] - z['R']) < 0.02 * z['R'] + 0.01)
    err = np.abs(alb_l[ys, xl] - alb_r).mean(-1)[same]
    err0 = np.abs(alb_l - alb_r).mean(-1)[same]
    return [f'  disparity: median {np.median(disparity[valid]):.1f} px, max {disparity[valid].max():.1f} px',
            f'  same surface in both eyes: {100 * same.mean():.1f}% of pixels',
            f'  albedo |R - L warped by depth|: mean {err.mean():.2f}, p95 {np.percentile(err, 95):.1f}',
            f'  albedo |R - L| without the warp: mean {err0.mean():.2f}, p95 {np.percentile(err0, 95):.1f}',
            f'  depth of a point, R vs L: median relative difference '
            f'{np.median(np.abs(z["L"][ys, xl] - z["R"])[same] / z["R"][same]):.5f}']


def replay(a):
    out = Path(a.out)
    # compute=1 (the default): P4.4 slice 3, dispatches reading twins captured and executed again
    # for eye R, captured commands carrying eye R's descriptors (findings/31 section 5.3).
    setups = [('identity', f'offset=0 ipd={a.ipd} same=1 replay=1 guardtest=0 compute=1 dup=1'),
              ('replay', f'offset=0 ipd={a.ipd} same=0 replay=1 guardtest=0 compute=1 dup=2'),
              ('perdraw', f'offset=0 ipd={a.ipd} same=0 replay=0 guardtest=0 compute=1 dup=3'),
              # The write guard's negative control: every draw and every dispatch that would run
              # twice counts as writing memory, so no doubled pass and no dispatch may get an eye
              # R (eye R's targets then differ from eye L's).
              ('guard', f'offset=0 ipd={a.ipd} same=1 replay=1 guardtest=1 compute=1 dup=4'),
              ('identity-nocompute',
               f'offset=0 ipd={a.ipd} same=1 replay=1 guardtest=0 compute=0 dup=5')]
    if not a.analyze:
        check_no_other_probe()
        game = poc.Game(out, a.probe, a.game_dir, 600, EXTRA_ENV)
        try:
            game.enter_world()
            game.head.write_text(f'yaw={a.yaw}' + chr(10))
            for label, control in setups:
                game.control.write_text(control + '\n')
                game.capture(label, settle=2.0)
                time.sleep(1.0)
        finally:
            game.close()
    text = poc.log_text(out / 'session.log')
    found = re.findall(r'^Stereo: G-buffer pass of frame (\d+) drawn twice.*$', text, re.M)
    lines_log = [line for line in text.splitlines() if 'drawn twice' in line]
    lines = [f'replay: P4.3 record once / execute twice, ipd {a.ipd} m', '']
    ok = len(found) >= len(setups)
    for (label, _), frame, log_line in zip(setups, found, lines_log):
        lines.append(f'{label} (frame {frame}):')
        lines.append('  log: ' + log_line.strip())
        pairs = stereo_pairs(out / 'dump', frame)
        if not pairs:
            lines.append('  no dumps')
            ok = False
            continue
        if label == 'guard':
            g = re.search(r'(\d+) render passes executed twice.*write guard: (\d+) passes '
                          r'without eye R \(guest writes\)', log_line)
            differ = sum(np.count_nonzero(np.fromfile(f['L'], np.uint8) !=
                                          np.fromfile(f['R'], np.uint8)) > 0
                         for (_, fmt, _, _), f in pairs.items() if fmt != 'Buffer')
            c = re.search(r'compute: (\d+) dispatches executed twice', log_line)
            fired = (bool(g) and g.group(1) == '0' and int(g.group(2)) > 0 and differ > 0 and
                     bool(c) and c.group(1) == '0')
            ok &= fired
            lines.append(f'  guard fired: {g.group(2) if g else "?"} passes without eye R, '
                         f'{g.group(1) if g else "?"} executed twice, '
                         f'{c.group(1) if c else "?"} dispatches executed twice, {differ} targets '
                         f'differ -> {"OK" if fired else "FAIL"}')
            continue
        if label.startswith('identity'):
            for (index, fmt, w, h), files in pairs.items():
                left = np.fromfile(files['L'], dtype=np.uint8)
                right = np.fromfile(files['R'], dtype=np.uint8)
                diff = int(np.count_nonzero(left != right)) if len(left) == len(right) else -1
                same = diff == 0
                note = ''
                if fmt == 'Buffer' and diff > 0:
                    # Eye R's copy of the copy shader's tiled bytes (P4.4 slice 2). The guest's
                    # download leaves the rows past the image (1080-1087 of a 1088-row tiled
                    # surface) holding whatever its staging held; they share the last 64-row
                    # tile band. Texel bytes must match: differences only in that band.
                    first = int(np.nonzero(left != right)[0][0])
                    band = len(left) - len(left) * 64 // 1088
                    dwords = len(np.unique(np.nonzero(left != right)[0] // 4))
                    padding = len(left) // 4 * 8 // 1088
                    same = first >= band and dwords <= padding
                    note = (f' ({dwords} dwords, all in the last tile band, at most the '
                            f'{padding} padding texels)' if same else
                            f' (first at {first}, band {band}; {dwords} dwords, padding {padding})')
                ok &= same
                lines.append(f'  target {index} {fmt} {w}x{h}: '
                             f'{"bit-identical" if diff == 0 else f"{diff} of {len(left)} bytes differ"}'
                             f'{note}')
        else:
            lines += warp_metrics(pairs, a.ipd)
        m = re.search(r'(\d+) cut short, (\d+) draws outside', log_line)
        if label != 'perdraw' and m and (m.group(1) != '0' or m.group(2) != '0'):
            ok = False
        # P4.4 slice 3: the dispatches' counts are in the log, and no dispatch capture was cut
        # short (eye R would then read eye L's results).
        c = re.search(r'compute: (\d+) dispatches executed twice, (\d+) once \(no twin read\), '
                      r'(\d+) cut short', log_line)
        if not c:
            lines.append('  compute: no slice-3 counts in the log (an older build?)')
            ok = False
        elif label != 'perdraw' and c.group(3) != '0':
            lines.append(f'  compute: {c.group(3)} dispatch captures cut short')
            ok = False
        # P4.4 write guard: no doubled pass of the G-buffer writes memory or uses a guest
        # attachment for eye R.
        g = re.search(r'write guard: (\d+) passes without eye R \(guest writes\), (\d+)', log_line)
        if label != 'perdraw' and (not g or g.group(1) != '0' or g.group(2) != '0'):
            lines.append('  write guard: ' + (g.group(0) if g else 'no write-guard counts in the log'))
            ok = False
    lines += ['', 'RESULT: ' + ('PASS' if ok else 'FAIL')]
    poc.report(out, lines)


def profile_blocks(text):
    """The GPU profile blocks of the log: (total ms/frame, {label: ms/frame})."""
    blocks = []
    for m in re.finditer(r'GPU profile: ([0-9.]+) ms/frame over \d+ frames, \d+ labels\n((?:  .*\n)+)',
                         text):
        labels = {}
        for line in m.group(2).splitlines():
            lm = re.match(r'\s+([0-9.]+) ms/frame\s+[0-9.]+/frame\s+(.*)$', line)
            if lm:
                labels[lm.group(2)] = labels.get(lm.group(2), 0.0) + float(lm.group(1))
        blocks.append((float(m.group(1)), labels))
    return blocks


def fps_values(text):
    return [float(v) for v in re.findall(r'^Frame stats: ([0-9.]+) FPS', text, re.M)]


def cost(a):
    check_no_other_probe()
    out = Path(a.out)
    game = poc.Game(out, a.probe, a.game_dir, 900,
                    dict(EXTRA_ENV, BB_GPU_PROFILE='1', BB_UPSCALER='off'))  # 'none' stops the profile's frame marks
    phases = [('stereo shaders, nothing doubled', 'offset=0 dupall=0'),
              ('G-buffer doubled, per-draw (prototype)', 'offset=0 dupall=1 replay=0'),
              ('G-buffer doubled, record once / execute twice', 'offset=0 dupall=1 replay=1'),
              ('stereo shaders, nothing doubled (again)', 'offset=0 dupall=0')]
    results = []
    try:
        game.enter_world()
        (out / 'png').mkdir(exist_ok=True)
        for yaw in (0, 90, 180, 270):  # which way the scene has geometry (for --yaw)
            game.head.write_text(f'yaw={yaw}' + chr(10))
            shot = game.capture(f'yaw{yaw}')
            from PIL import Image
            Image.fromarray(poc.frame_png(next(shot.glob('output_*.raw')))).resize(
                (480, 270)).save(out / 'png' / f'yaw{yaw}.png')
        game.head.write_text(f'yaw={a.yaw}' + chr(10))
        for label, control in phases:
            game.control.write_text(control + '\n')
            time.sleep(6)  # the profile window in flight when the setting changed
            mark = len(poc.log_text(game.log))
            time.sleep(a.seconds)
            text = poc.log_text(game.log)[mark:]
            results.append((label, profile_blocks(text), fps_values(text)))
    finally:
        game.close()
    lines = ['cost: GPU ms per frame (BB_GPU_PROFILE=1; busy = total - "GPU idle between submissions")', '']
    for label, blocks, fps in results:
        if not blocks:
            lines.append(f'{label}: no profile')
            continue
        busy = [t - sum(v for k, v in labels.items() if 'between submissions' in k)
                for t, labels in blocks]
        gbuf = [sum(v for k, v in labels.items()
                    if re.search(r'pass \d+x\d+ \[(\S+ ){5,}\]', k)) for _, labels in blocks]
        eye_r = [sum(v for k, v in labels.items() if k.startswith('stereo: eye R'))
                 for _, labels in blocks]
        lines.append(f'{label}:')
        lines.append(f'  busy {np.mean(busy):.2f} ms (blocks: {", ".join(f"{b:.2f}" for b in busy)})')
        lines.append(f'  passes with 5+ color targets {np.mean(gbuf):.2f} ms, eye R replay label '
                     f'{np.mean(eye_r):.2f} ms')
        if fps:
            lines.append(f'  fps {min(fps):.0f}-{max(fps):.0f}')
    poc.report(out, lines)


def warpcost(a):
    """GPU ms/frame of the eye-R warp, guided fill vs mirrored fill (see the docstring)."""
    check_no_other_probe()
    out = Path(a.out)
    game = poc.Game(out, a.probe, a.game_dir, 900,
                    dict(EXTRA_ENV, BB_GPU_PROFILE='1', BB_UPSCALER='off', BB_STEREO_HYBRID='1'))
    phases = [('guided', 'offset=0 hybrid=1 full=0 guided=1'),
              ('mirrored', 'offset=0 hybrid=1 full=0 guided=0'),
              ('guided', 'offset=0 hybrid=1 full=0 guided=1 '),
              ('mirrored', 'offset=0 hybrid=1 full=0 guided=0 ')]
    results = []
    try:
        game.enter_world()
        game.head.write_text(f'yaw={a.yaw}' + chr(10))
        for label, control in phases:
            game.control.write_text(control + '\n')
            time.sleep(6)
            mark = len(poc.log_text(game.log))
            time.sleep(a.seconds)
            text = poc.log_text(game.log)[mark:]
            warp_ms = [sum(v for k, v in labels.items()
                           if k.startswith('stereo: eye R warp') or
                           k.startswith('stereo: eye R guided'))
                       for _, labels in profile_blocks(text)]
            results.append((label, warp_ms))
    finally:
        game.close()
    lines = ['warpcost: eye-R warp pass, GPU ms/frame (BB_GPU_PROFILE=1, BB_UPSCALER=off)', '']
    for label, warp_ms in results:
        lines.append(f'{label:9s}: ' + (', '.join(f'{v:.3f}' for v in warp_ms) or 'no profile'))
    for label in ('guided', 'mirrored'):
        v = [x for lab, ms in results if lab == label for x in ms]
        if v:
            lines.append(f'{label}: mean {np.mean(v):.3f} ms/frame over {len(v)} blocks')
    poc.report(out, lines)


def look(a):
    """A/B helper: the view at four head yaws (PNGs), with --xr-mode and --env overrides."""
    check_no_other_probe()
    out = Path(a.out)
    game = poc.Game(out, a.probe, a.game_dir, 400, extra_env(a))
    from PIL import Image
    (out / 'png').mkdir(exist_ok=True)
    try:
        game.enter_world()
        for yaw in (0, 90, 180, 270):
            game.head.write_text(f'yaw={yaw}' + chr(10))
            shot = game.capture(f'yaw{yaw}')
            Image.fromarray(poc.frame_png(next(shot.glob('output_*.raw')))).resize(
                (480, 270)).save(out / 'png' / f'yaw{yaw}.png')
    finally:
        game.close()
    text = poc.log_text(game.log)
    lines = [f'look: probe {a.probe}, env {extra_env(a)}',
             *[line for line in text.splitlines() if 'remapped (perspective)' in line][-2:],
             *re.findall(r'^Frame stats: .*$', text, re.M)[-1:]]
    poc.report(out, lines)


def timing(a):
    """Alt/mono regression check: the same flat session (fake head, uncapped) with each probe in
    --probes, in turn (A B A B ...), the game's frame rate and the port's CPU stats in the world."""
    out = Path(a.out)
    probes = a.probes.split(',')
    rows = []
    for rep_index in range(a.repeats):
        for probe in probes:
            check_no_other_probe()
            run_out = out / f'{Path(probe).stem}_{rep_index}'
            env = {k: v for k, v in extra_env(a).items()
                   if k not in ('BB_IMAGE_COPY_DEBUG', 'BB_STEREO_WRITE_AUDIT')}  # no diagnostics
            env.setdefault('BB_XR_MODE', 'alt')
            game = poc.Game(run_out, probe, a.game_dir, 400, env)
            try:
                game.enter_world()
                mark = len(poc.log_text(game.log))
                time.sleep(a.seconds)
                text = poc.log_text(game.log)[mark:]
            finally:
                game.close()
            stats = re.findall(r'^Frame stats: ([0-9.]+) FPS, worst frame ([0-9.]+) ms.*?'
                               r'GPU thread ([0-9.]+) us/draw, ([0-9.]+) draws/frame', text, re.M)
            pipe = re.findall(r'^Draw pipe: .*stage A waited ([0-9.]+)%, stage B busy ([0-9.]+)%',
                              text, re.M)
            fps = [float(x[0]) for x in stats]
            worst = [float(x[1]) for x in stats]
            draws = [float(x[3]) for x in stats]
            busy_b = [float(x[1]) for x in pipe]
            rows.append((Path(probe).name, rep_index, fps, worst, draws, busy_b))
    lines = [f'timing: BB_XR_MODE={extra_env(a).get("BB_XR_MODE", "alt")}, flat, {a.seconds:.0f} s in '
             f'the world per run, uncapped', '']
    for name, rep_index, fps, worst, draws, busy_b in rows:
        if not fps:
            lines.append(f'{name} #{rep_index}: no frame stats')
            continue
        lines.append(f'{name} #{rep_index}: FPS median {np.median(fps):.1f} (min {min(fps):.1f}), '
                     f'frame {1000 / np.median(fps):.2f} ms, worst frame median {np.median(worst):.1f} ms, '
                     f'draws/frame {np.median(draws):.0f}, stage B busy '
                     f'{np.median(busy_b) if busy_b else float("nan"):.1f}%')
    for probe in probes:
        name = Path(probe).name
        fps = [np.median(r[2]) for r in rows if r[0] == name and r[2]]
        if fps:
            lines.append(f'{name}: mean of run medians {np.mean(fps):.1f} FPS '
                         f'({1000 / np.mean(fps):.2f} ms)')
    poc.report(out, lines)


def wait_warp_dump(game, n):
    """Moves dump n's files (warp<n>_f*) into dump/warp<n>, once they are written."""
    poc.wait_for(lambda: list(game.dump.glob(f'warp{n}_f*_color_L_*.raw')), 30, game.process,
                 f'warp dump {n}')
    time.sleep(1.5)  # the other files of the same frame
    target = game.dump / f'warp{n}'
    target.mkdir(exist_ok=True)
    for f in game.dump.glob(f'warp{n}_f*.raw'):
        f.rename(target / f.name)
    return target


def warp_images(folder):
    """{'L', 'R', 'zL', 'zR' (raw depth), 'raw_L', 'raw_R' (bytes)} of one warp dump."""
    out = {}
    for raw in folder.glob('warp*_f*_*_[LR]_*.raw'):
        m = re.match(r'warp\d+_f\d+_(color|depth)_([LR])_(.+)_(\d+)x(\d+)\.raw$', raw.name)
        if not m:
            continue
        w, h = int(m.group(4)), int(m.group(5))
        data = np.fromfile(raw, dtype=np.uint8)[:w * h * 4]
        if m.group(1) == 'color':
            out['raw_' + m.group(2)] = data
            out[m.group(2)] = data.reshape(h, w, 4)[..., :3].astype(np.float32)
        else:
            out['d' + m.group(2)] = data.view(np.float32).reshape(h, w)
    return out


def warp_log(text):
    """{dump id: (shift px*m, zs, zo, eye R warped?)} from the warp dump log lines."""
    found = {}
    for m in re.finditer(r'^Stereo: warp (\d+) of frame \d+: (.*?), eye L ([-0-9.]+) m, eye R '
                         r'([-0-9.]+) m, P00 ([-0-9.]+), shift ([-0-9.e+]+) px\*m, zs ([-0-9.e+]+), '
                         r'zo ([-0-9.e+]+)', text, re.M):
        found[int(m.group(1))] = (float(m.group(6)), float(m.group(7)), float(m.group(8)),
                                  m.group(2) == 'eye R warped')
    return found


def side_agreement(img, shift, zs, zo):
    """Eye R's pixels whose surface (depth) and color agree with eye L at x + d and at x - d
    (d = shift / z, pixels shifted by at least 1): (depth +, depth -, color +, color -)."""
    h, w = img['dR'].shape
    with np.errstate(divide='ignore', invalid='ignore'):
        z_r = zo / (img['dR'] - zs)
        z_l = zo / (img['dL'] - zs)
    valid = np.isfinite(z_r) & (z_r > 0) & (z_r < 2000)
    d = np.where(valid, shift / np.where(valid, z_r, 1.0), 0.0)
    valid &= np.abs(d) >= 1.0
    ys, xs = np.mgrid[0:h, 0:w]
    result = []
    for sign in (1, -1):
        xl = np.clip(np.floor(xs + 0.5 + sign * d).astype(int), 0, w - 1)
        same = np.abs(z_l[ys, xl] - z_r) < 0.02 * z_r + 0.01
        result.append(100 * (same & valid).sum() / max(valid.sum(), 1))
    for sign in (1, -1):
        xl = np.clip(np.floor(xs + 0.5 + sign * d).astype(int), 0, w - 1)
        result.append(np.abs(img['L'][ys, xl] - img['R']).mean(-1)[valid].mean())
    return result


def hole_mask(img, shift, zs, zo):
    """Eye R's disoccluded pixels: both direct taps of the warp fail (vk_stereo_warp.h
    DirectSample, same tolerances; depth at the color's size), the sky excluded."""
    h, w = img['dR'].shape
    with np.errstate(divide='ignore', invalid='ignore'):
        def view_z(d):
            z = zo / (d - zs)
            return np.where(np.isfinite(z) & (z > 0), z, 1e30)
        z_r = view_z(img['dR'])
        z_l = view_z(img['dL'])
        u = np.arange(w)[None, :] + shift / z_r
        x0 = np.floor(u).astype(np.int64)
        f = u - x0
        ys = np.arange(h)[:, None].repeat(w, 1)

        def tap(xl):
            inside = (xl >= 0) & (xl < w)
            zl = z_l[ys, np.clip(xl, 0, w - 1)]
            same = (np.abs(zl - z_r) <= 0.02 * z_r + 0.01) | \
                (np.abs(shift) * np.abs(1 / zl - 1 / z_r) <= 0.5)
            return inside & same
        v0 = tap(x0)
        v1 = (f > 0) & tap(x0 + 1)
    return ~(v0 | v1) & (z_r < 1000)


def warp(a):
    """Hybrid stereo: eye R warped from eye L (see the module docstring)."""
    out = Path(a.out)
    e = a.ipd / 2
    setups = [('same', f'offset=0 hybrid=1 full=0 guided=1 ipd={a.ipd} same=1 replay=1 warpdump=1', 1),
              ('stereo0', f'offset=0 hybrid=1 full=0 guided=1 ipd={a.ipd} same=0 replay=1 warpdump=2', 2),
              ('stereo1', f'offset=0 hybrid=1 full=0 guided=1 ipd={a.ipd} same=0 replay=1 warpdump=3', 3),
              ('inverted', f'offset=0 hybrid=1 full=0 guided=1 ipd={-a.ipd} same=0 replay=1 warpdump=4', 4),
              # The oracle: the whole region from the camera moved +ipd/2 (P4.1's shift).
              ('oracle0', f'offset={e} hybrid=0 ipd={a.ipd} same=0 replay=1 warpdump=5', 5),
              ('oracle1', f'offset={e} hybrid=0 ipd={a.ipd} same=0 replay=1 warpdump=6', 6),
              # The mirrored fill alone (section 9.10's before), against the same oracle.
              ('mirror0', f'offset=0 hybrid=1 full=0 guided=0 ipd={a.ipd} same=0 replay=1 warpdump=7', 7),
              ('mirror1', f'offset=0 hybrid=1 full=0 guided=0 ipd={a.ipd} same=0 replay=1 warpdump=8', 8)]
    folders = {}
    if not a.analyze:
        check_no_other_probe()
        game = poc.Game(out, a.probe, a.game_dir, 600, dict(EXTRA_ENV, BB_STEREO_HYBRID='1'))
        try:
            game.enter_world()
            game.head.write_text(f'yaw={a.yaw}' + chr(10))
            for label, control, n in setups:
                game.control.write_text(control + '\n')
                time.sleep(2.0)
                folders[label] = wait_warp_dump(game, n)
        finally:
            game.close()
    else:
        folders = {label: out / 'dump' / f'warp{n}' for label, _, n in setups}
    text = poc.log_text(out / 'session.log')
    logged = warp_log(text)
    img = {label: warp_images(folder) for label, folder in folders.items()}
    lines = [f'warp: hybrid stereo, eye R warped from eye L, ipd {a.ipd} m', '']
    ok = True

    # Every frame warped while hybrid is on (the 5 s log lines).
    rates = [(int(w_), int(f_)) for w_, f_ in
             re.findall(r'hybrid on, ipd [-0-9.]+ m: (\d+) eye-R warps in (\d+) frames', text)]
    best = max((w_ / f_ for w_, f_ in rates if f_), default=0.0)
    lines.append(f'frames with an eye-R warp (best 5 s window): {100 * best:.0f}% '
                 f'({len(rates)} windows)')
    ok &= best >= 0.9

    # 1. Identity.
    s = img.get('same', {})
    if 'raw_L' in s and 'raw_R' in s:
        diff = int(np.count_nonzero(s['raw_L'] != s['raw_R']))
        lines.append(f'same=1: eye R vs eye L: '
                     f'{"byte-identical" if diff == 0 else f"{diff} bytes differ"}')
        ok &= diff == 0
    else:
        lines.append('same=1: no eye R dump (no warp?)')
        ok = False

    # 2./3. The sign of the disparity, normal and inverted.
    shifts = {}
    for label, n in (('stereo0', 2), ('inverted', 4)):
        if n not in logged or not logged[n][3] or 'R' not in img.get(label, {}):
            lines.append(f'{label}: no warp logged or dumped')
            ok = False
            continue
        shift, zs, zo, _ = logged[n]
        shifts[label] = shift
        dp, dm, cp, cm = side_agreement(img[label], shift, zs, zo)
        lines.append(f'{label}: shift {shift:+.3f} px*m (zs {zs:.6f}, zo {zo:.6f}); same surface '
                     f'at x + d {dp:.1f}%, at x - d {dm:.1f}%; |R - L| at x + d {cp:.2f}, at x - d '
                     f'{cm:.2f}')
        ok &= dp > dm and cp < cm  # depth: independent of the warp; color: its sign
    if len(shifts) == 2:
        flipped = shifts['stereo0'] > 0 > shifts['inverted'] and \
            abs(shifts['stereo0'] + shifts['inverted']) < 0.01 * abs(shifts['stereo0'])
        lines.append(f'inverted ipd flips the shift: {"yes" if flipped else "NO"}')
        ok &= flipped

    # 4. Eye R against the camera moved +ipd/2 (static pixels only).
    need = ('stereo0', 'stereo1', 'oracle0', 'oracle1')
    if all('L' in img.get(k, {}) for k in need) and all('R' in img[k] for k in need[:2]):
        h, w = img['oracle0']['L'].shape[:2]
        crop = (slice(0, h), slice(int(w * 0.03), int(w * 0.97)))  # the edges have no source

        def amax(x, y):
            return np.max(np.abs(x[crop] - y[crop]), axis=-1)
        static = ((amax(img['stereo0']['L'], img['stereo1']['L']) < 4) &
                  (amax(img['stereo0']['R'], img['stereo1']['R']) < 4) &
                  (amax(img['oracle0']['L'], img['oracle1']['L']) < 4))
        lines.append(f'oracle: static pixels {100 * static.mean():.1f}%')
        means = {}
        for key, x, y, what in [
                ('noise', img['oracle0']['L'], img['oracle1']['L'], 'noise: +ipd/2 camera twice'),
                ('signal', img['stereo0']['L'], img['oracle0']['L'], 'signal: eye L vs +ipd/2 camera'),
                ('warp0', img['stereo0']['R'], img['oracle0']['L'], 'warp: eye R vs +ipd/2 camera'),
                ('warp1', img['stereo1']['R'], img['oracle1']['L'], 'warp: eye R vs +ipd/2 (2nd)')]:
            d = np.mean(np.abs(x[crop] - y[crop]), axis=-1)[static]
            means[key] = d.mean()
            lines.append(f'  {what:34s} mean {d.mean():5.2f}  p99 {np.percentile(d, 99):5.1f}  '
                         f'>8: {100 * (d > 8).mean():5.2f}%')
        signal = means['signal']
        worst = max(means['warp0'], means['warp1'])
        good = signal > 0 and worst < 0.35 * signal
        lines.append(f'  warp residual / signal: {worst / signal if signal else float("nan"):.2f} '
                     f'(pass below 0.35)')
        ok &= good
        from PIL import Image
        (out / 'png').mkdir(exist_ok=True)
        s0 = img['stereo0']
        Image.fromarray(np.concatenate([s0['L'], s0['R']], 1).astype(np.uint8)).resize(
            (1920, 540)).save(out / 'png' / 'eyes_LR.png')
        ana = np.stack([s0['L'].mean(-1), s0['R'].mean(-1), s0['R'].mean(-1)], -1)
        Image.fromarray(ana.astype(np.uint8)).resize((960, 540)).save(out / 'png' / 'anaglyph.png')
        err = np.clip(np.abs(s0['R'] - img['oracle0']['L']).mean(-1) * 8, 0, 255)
        Image.fromarray(err.astype(np.uint8)).resize((960, 540)).save(
            out / 'png' / 'warp_vs_oracle_x8.png')

        # 5. The hole pixels alone: guided fill (stereo0/1) vs mirrored fill (mirror0/1).
        if all('R' in img.get(k, {}) for k in ('mirror0', 'mirror1')) and 2 in logged:
            shift, zs, zo, _ = logged[2]
            lines.append('disocclusion fill vs the +ipd/2 camera (hole pixels: both direct taps '
                         'fail; static, sky excluded):')
            fills = {}
            for key, label, oracle_key, what in [
                    ('guided0', 'stereo0', 'oracle0', 'guided fill'),
                    ('guided1', 'stereo1', 'oracle1', 'guided fill (2nd)'),
                    ('mirror0', 'mirror0', 'oracle0', 'mirrored fill'),
                    ('mirror1', 'mirror1', 'oracle1', 'mirrored fill (2nd)')]:
                holes = hole_mask(img[label], shift, zs, zo)[crop]
                sel = holes & static
                d = np.mean(np.abs(img[label]['R'][crop] - img[oracle_key]['L'][crop]), axis=-1)
                sig = np.mean(np.abs(img[label]['L'][crop] - img[oracle_key]['L'][crop]),
                              axis=-1)
                if sel.sum() == 0:
                    lines.append(f'  {what}: no hole pixels')
                    continue
                fills[key] = d[sel].mean()
                lines.append(f'  {what:22s} {int(sel.sum()):7d} px ({100 * holes.mean():.2f}% '
                             f'holes)  mean {d[sel].mean():5.2f}  median {np.median(d[sel]):5.2f}'
                             f'  p90 {np.percentile(d[sel], 90):5.1f}  (eye L there: '
                             f'{sig[sel].mean():5.2f})')
                if key == 'guided0':
                    e8 = np.clip(d * 8, 0, 255) * holes
                    Image.fromarray(e8.astype(np.uint8)).resize((960, 540)).save(
                        out / 'png' / 'holes_guided_vs_oracle_x8.png')
                if key == 'mirror0':
                    e8 = np.clip(d * 8, 0, 255) * holes
                    Image.fromarray(e8.astype(np.uint8)).resize((960, 540)).save(
                        out / 'png' / 'holes_mirror_vs_oracle_x8.png')
            if len(fills) == 4:
                g = (fills['guided0'] + fills['guided1']) / 2
                m = (fills['mirror0'] + fills['mirror1']) / 2
                lines.append(f'  hole error guided / mirrored: {g:.2f} / {m:.2f} = {g / m:.2f}')
    else:
        lines.append('oracle: dumps missing')
        ok = False
    lines += ['', 'RESULT: ' + ('PASS' if ok else 'FAIL')]
    poc.report(out, lines)


FULL_ENV = {'BB_UPSCALER': 'off', 'BB_XR_HUD': 'force', 'BB_STEREO_HYBRID': '1'}


def full_log(text):
    """Every 5 s line's full stereo counts: [(regions, rendered flips, warp fallbacks)]."""
    return [(int(a_), int(b_), int(c_)) for a_, b_, c_ in
            re.findall(r'full stereo on: (\d+) regions, eye R rendered at (\d+) flips, warp '
                       r'fallback at (\d+)', text)]


def oracle_residuals(img, labels, oracle_pair, lines, crop_frac=0.03):
    """Mean |eye R - oracle| on static pixels for each (key, label pair) in `labels`, next to the
    noise (the oracle twice) and the signal (eye L against the oracle). Returns {key: mean}."""
    o0, o1 = oracle_pair
    h, w = img[o0]['L'].shape[:2]
    crop = (slice(0, h), slice(int(w * crop_frac), int(w * (1 - crop_frac))))

    def amax(x, y):
        return np.max(np.abs(x[crop] - y[crop]), axis=-1)
    static = amax(img[o0]['L'], img[o1]['L']) < 4
    for key, (a_, b_) in labels.items():
        static &= amax(img[a_]['L'], img[b_]['L']) < 4
        static &= amax(img[a_]['R'], img[b_]['R']) < 4
    lines.append(f'oracle: static pixels {100 * static.mean():.1f}%')
    means = {}
    rows = [('noise', img[o0]['L'], img[o1]['L'], 'noise: +ipd/2 camera twice')]
    first = next(iter(labels.values()))[0]
    rows.append(('signal', img[first]['L'], img[o0]['L'], 'signal: eye L vs +ipd/2 camera'))
    for key, (a_, b_) in labels.items():
        rows.append((key + '0', img[a_]['R'], img[o0]['L'], f'{key}: eye R vs +ipd/2 camera'))
        rows.append((key + '1', img[b_]['R'], img[o1]['L'], f'{key}: eye R vs +ipd/2 (2nd)'))
    for key, x, y, what in rows:
        d = np.mean(np.abs(x[crop] - y[crop]), axis=-1)[static]
        means[key] = d.mean()
        lines.append(f'  {what:38s} mean {d.mean():5.2f}  p99 {np.percentile(d, 99):5.1f}  '
                     f'>8: {100 * (d > 8).mean():5.2f}%')
    return means, static, crop


def full(a):
    """Full stereo: eye R rendered for real (findings/31 section 10); see the module docstring."""
    out = Path(a.out)
    e = a.ipd / 2
    setups = [('same', f'offset=0 hybrid=1 full=1 ipd={a.ipd} same=1 replay=1 compute=1 warpdump=1', 1),
              ('full0', f'offset=0 hybrid=1 full=1 ipd={a.ipd} same=0 replay=1 compute=1 warpdump=2 '
                        f'audit=1', 2),
              ('full1', f'offset=0 hybrid=1 full=1 ipd={a.ipd} same=0 replay=1 compute=1 warpdump=3', 3),
              ('inverted', f'offset=0 hybrid=1 full=1 ipd={-a.ipd} same=0 replay=1 compute=1 warpdump=4', 4),
              ('oracle0', f'offset={e} hybrid=0 full=1 ipd={a.ipd} same=0 replay=1 warpdump=5', 5),
              ('oracle1', f'offset={e} hybrid=0 full=1 ipd={a.ipd} same=0 replay=1 warpdump=6', 6),
              # The hybrid warp in the same session (HUD-free display buffer, same settings).
              ('hybrid0', f'offset=0 hybrid=1 full=0 guided=1 ipd={a.ipd} same=0 replay=1 warpdump=7', 7),
              ('hybrid1', f'offset=0 hybrid=1 full=0 guided=1 ipd={a.ipd} same=0 replay=1 warpdump=8', 8),
              ('samehybrid', f'offset=0 hybrid=1 full=0 ipd={a.ipd} same=1 replay=1 warpdump=9', 9)]
    folders = {}
    if not a.analyze:
        check_no_other_probe()
        game = poc.Game(out, a.probe, a.game_dir, 600, dict(EXTRA_ENV, **FULL_ENV))
        try:
            game.enter_world()
            game.head.write_text(f'yaw={a.yaw}' + chr(10))
            for label, control, n in setups:
                game.control.write_text(control + '\n')
                time.sleep(3.0)  # a full region needs the twins of a frame or two
                folders[label] = wait_warp_dump(game, n)
            time.sleep(6.0)  # one more 5 s log window with full stereo off, then on
            game.control.write_text(f'offset=0 hybrid=1 full=1 ipd={a.ipd} same=0 replay=1 '
                                    f'compute=1 warpdump=9\n')
            time.sleep(11.0)
        finally:
            game.close()
    else:
        folders = {label: out / 'dump' / f'warp{n}' for label, _, n in setups}
    text = poc.log_text(out / 'session.log')
    logged = {}
    for m in re.finditer(r'^Stereo: warp (\d+) of frame \d+: (.*?), eye L ([-0-9.]+) m, eye R '
                         r'([-0-9.]+) m, P00 ([-0-9.]+), shift ([-0-9.e+]+) px\*m, zs ([-0-9.e+]+), '
                         r'zo ([-0-9.e+]+)', text, re.M):
        logged[int(m.group(1))] = (float(m.group(6)), float(m.group(7)), float(m.group(8)),
                                   m.group(2))
    img = {label: warp_images(folder) for label, folder in folders.items()}
    lines = [f'full: full stereo, eye R rendered (G-buffer to the display pass), ipd {a.ipd} m', '']
    # The write audit: one full region listed pass by pass ("audit=1"), and per shader the
    # dispatch decisions of the session (BB_STEREO_WRITE_AUDIT=1).
    audit = re.findall(r'^Stereo: (?:audit|write audit|write guard): .*$', text, re.M)
    (out / 'audit.txt').write_text('\n'.join(audit) + '\n')
    passes = [x for x in audit if x.startswith('Stereo: audit: pass ')]
    lines.append(f'write audit: {len(audit)} lines in audit.txt ({len(passes)} render passes of '
                 f'the listed region)')
    ok = True

    counts = full_log(text)
    best = max((c[1] / c[0] for c in counts if c[0]), default=0.0)
    lines.append(f'flips with a rendered eye R per full region (best 5 s window): {100 * best:.0f}% '
                 f'(windows: {", ".join(f"{r}/{c}/{f}" for r, c, f in counts[-6:])} regions/'
                 f'rendered/fallback)')
    ok &= best >= 0.9
    for n in (1, 2, 3, 4):
        what = logged.get(n, (0, 0, 0, 'not logged'))[3]
        lines.append(f'dump {n}: {what}')
        ok &= what == 'eye R rendered'
    summaries = re.findall(r'^Stereo: full region of frame \d+: .*$', text, re.M)
    if summaries:
        lines.append('last full region summary: ' + summaries[-1][len('Stereo: '):])
    details = re.findall(r'^Stereo: full region of frame \d+ drawn twice .*$', text, re.M)
    if details:
        lines.append('  ' + details[-1][len('Stereo: '):])
        m = re.search(r'(\d+) cut short, (\d+) draws outside a capture', details[-1])
        if m:
            ok &= int(m.group(1)) == 0

    # 1. Identity: zero eye distance, eye R's rendered final image equals eye L's.
    for label in ('same', 'samehybrid'):
        s = img.get(label, {})
        if 'raw_L' in s and 'raw_R' in s:
            diff = int(np.count_nonzero(s['raw_L'] != s['raw_R']))
            px = int(np.count_nonzero(np.any(s['L'] != s['R'], axis=-1)))
            lines.append(f'{label}=1: eye R vs eye L: '
                         f'{"byte-identical" if diff == 0 else f"{diff} bytes ({px} pixels) differ"}')
            if label == 'same':
                ok &= diff == 0
                if diff:
                    from PIL import Image
                    (out / 'png').mkdir(exist_ok=True)
                    d8 = np.clip(np.abs(s['L'] - s['R']).max(-1) * 16, 0, 255).astype(np.uint8)
                    Image.fromarray(d8).save(out / 'png' / 'same_diff_x16.png')
        else:
            lines.append(f'{label}=1: no eye R dump')
            ok &= label != 'same'

    # 2. The sign: eye R's surfaces agree with eye L at x + d, inverted ipd flips it.
    for label, n in (('full0', 2), ('inverted', 4)):
        if n not in logged or 'R' not in img.get(label, {}):
            lines.append(f'{label}: no dump')
            ok = False
            continue
        shift, zs, zo, _ = logged[n]
        dp, dm, cp, cm = side_agreement(img[label], shift, zs, zo)
        lines.append(f'{label}: shift {shift:+.3f} px*m; same surface at x + d {dp:.1f}%, at x - d '
                     f'{dm:.1f}%; |R - L| at x + d {cp:.2f}, at x - d {cm:.2f}')
        ok &= dp > dm and cp < cm

    # 3. Against the oracle (the whole region from the camera moved +ipd/2).
    need = ('full0', 'full1', 'oracle0', 'oracle1', 'hybrid0', 'hybrid1')
    if all('L' in img.get(k, {}) for k in need) and all('R' in img[k] for k in
                                                        ('full0', 'full1', 'hybrid0', 'hybrid1')):
        means, static, crop = oracle_residuals(
            img, {'full': ('full0', 'full1'), 'hybrid': ('hybrid0', 'hybrid1')},
            ('oracle0', 'oracle1'), lines)
        signal, noise = means['signal'], means['noise']
        f_ = max(means['full0'], means['full1'])
        h_ = max(means['hybrid0'], means['hybrid1'])
        lines.append(f'  residual / signal: full {f_ / signal:.3f}, hybrid {h_ / signal:.3f}; '
                     f'full - noise {f_ - noise:+.2f} (pass: full below hybrid and below 2x noise)')
        ok &= f_ < h_ and f_ < 2.0 * noise + 0.1
        # The hybrid's hole pixels (both direct taps fail), where the warp had to invent pixels.
        if 7 in logged:
            shift, zs, zo, _ = logged[7]
            lines.append('hole pixels of the hybrid (eye R sees what eye L does not; static, sky '
                         'excluded):')
            for key, label, oracle_key in (('full', 'full0', 'oracle0'), ('full', 'full1', 'oracle1'),
                                           ('hybrid', 'hybrid0', 'oracle0'),
                                           ('hybrid', 'hybrid1', 'oracle1')):
                holes = hole_mask(img[label], shift, zs, zo)[crop]
                sel = holes & static
                d = np.mean(np.abs(img[label]['R'][crop] - img[oracle_key]['L'][crop]), axis=-1)
                if sel.sum():
                    lines.append(f'  {key:6s} ({label}) {int(sel.sum()):7d} px  mean {d[sel].mean():5.2f}  '
                                 f'median {np.median(d[sel]):5.2f}  p90 {np.percentile(d[sel], 90):5.1f}')
        from PIL import Image
        (out / 'png').mkdir(exist_ok=True)
        f0 = img['full0']
        Image.fromarray(np.concatenate([f0['L'], f0['R']], 1).astype(np.uint8)).resize(
            (1920, 540)).save(out / 'png' / 'full_eyes_LR.png')
        for label, oracle_key in (('full0', 'oracle0'), ('hybrid0', 'oracle0')):
            err = np.clip(np.abs(img[label]['R'] - img[oracle_key]['L']).mean(-1) * 8, 0, 255)
            Image.fromarray(err.astype(np.uint8)).resize((960, 540)).save(
                out / 'png' / f'{label}_vs_oracle_x8.png')
    else:
        lines.append('oracle: dumps missing')
        ok = False
    lines += ['', 'RESULT: ' + ('PASS' if ok else 'FAIL')]
    poc.report(out, lines)


def fullcost(a):
    """GPU ms/frame: stereo shaders only, hybrid (G-buffer doubled + warp), full stereo; A B C x2."""
    check_no_other_probe()
    out = Path(a.out)
    game = poc.Game(out, a.probe, a.game_dir, 900,
                    dict(EXTRA_ENV, BB_GPU_PROFILE='1', **FULL_ENV))
    phases = [('mono (stereo shaders, nothing doubled)', 'offset=0 hybrid=0 full=0'),
              ('hybrid (G-buffer doubled, warp)', 'offset=0 hybrid=1 full=0'),
              ('full stereo', 'offset=0 hybrid=1 full=1'),
              ('mono (stereo shaders, nothing doubled)', 'offset=0 hybrid=0 full=0 '),
              ('hybrid (G-buffer doubled, warp)', 'offset=0 hybrid=1 full=0 '),
              ('full stereo', 'offset=0 hybrid=1 full=1 ')]
    results = []
    try:
        game.enter_world()
        game.head.write_text(f'yaw={a.yaw}' + chr(10))
        for label, control in phases:
            game.control.write_text(control + '\n')
            time.sleep(6)
            mark = len(poc.log_text(game.log))
            time.sleep(a.seconds)
            text = poc.log_text(game.log)[mark:]
            results.append((label, profile_blocks(text), fps_values(text)))
    finally:
        game.close()
    lines = ['fullcost: GPU ms per frame (BB_GPU_PROFILE=1, BB_UPSCALER=off, BB_XR_HUD=force; '
             'busy = total - "GPU idle between submissions")', '']
    summary = {}
    for label, blocks, fps in results:
        if not blocks:
            lines.append(f'{label}: no profile')
            continue
        busy = [t - sum(v for k, v in labels.items() if 'between submissions' in k)
                for t, labels in blocks]
        eye_r = {}
        for _, labels in blocks:
            for k, v in labels.items():
                if k.startswith('stereo:'):
                    eye_r[k] = eye_r.get(k, 0.0) + v / len(blocks)
        lines.append(f'{label}:')
        lines.append(f'  busy {np.mean(busy):.2f} ms (blocks: {", ".join(f"{b:.2f}" for b in busy)})')
        for k, v in sorted(eye_r.items()):
            lines.append(f'    {v:6.3f} ms  {k}')
        if fps:
            lines.append(f'  fps {min(fps):.0f}-{max(fps):.0f} (median {np.median(fps):.0f})')
        summary.setdefault(label, []).append(np.mean(busy))
    lines.append('')
    for label, v in summary.items():
        lines.append(f'{label}: mean busy {np.mean(v):.2f} ms')
    poc.report(out, lines)


NR_PLACE_IQ = 'upscale=dlss dlss_quality=quality'
NR_PLACE_PHASES = [('dlss only', 'dlssnr=0'),
                   ('NR post', 'dlssnr=1 dlssnr_place=post'),
                   ('NR pre-SR', 'dlssnr=1 dlssnr_place=pre')]


def nrplace(a):
    """DLSS-NR post vs pre-SR (docs/vr/findings/42 section 7): GPU ms and fps, hybrid and full.

    Flat, fake head, BB_XR_MODE=stereo with eye R's own DLSS (2880x1620 per eye, quality, as the
    headset) and BB_XR_DLSSNR_FLAT_PAIRS=1, so both eyes are processed every frame as in the
    headset. Per stereo mode: DLSS only, NR post, NR pre-SR, A B C A B C, then frame dumps of
    each placement (output_* = eye L, output_r_* = eye R)."""
    check_no_other_probe()
    out = Path(a.out)
    iq = out / 'iq.txt'
    iq.write_text(f'{NR_PLACE_IQ} dlssnr=0\n')
    env = dict(EXTRA_ENV, BB_GPU_PROFILE='1', **FULL_ENV, BB_XR_IQ_CONTROL=str(iq),
               BB_XR_DLSSNR_FLAT_PAIRS='1', BB_XR_UPSCALE='dlss')
    for kv in a.env:
        key, _, value = kv.partition('=')
        env[key] = value
    game = poc.Game(out, a.probe, a.game_dir, 1500, env)
    modes = [('hybrid', 'offset=0 hybrid=1 full=0'), ('full', 'offset=0 hybrid=1 full=1')]
    results = []
    shots = {}
    try:
        game.enter_world()
        game.head.write_text(f'yaw={a.yaw}' + chr(10))
        for mode, control in modes:
            game.control.write_text(control + '\n')
            for rep in range(2):
                for label, nr in NR_PLACE_PHASES:
                    iq.write_text(f'{NR_PLACE_IQ} {nr}\n')
                    time.sleep(6)
                    mark = len(poc.log_text(game.log))
                    time.sleep(a.seconds)
                    text = poc.log_text(game.log)[mark:]
                    results.append((mode, label, profile_blocks(text), fps_values(text)))
                    print(f'{mode} {label} #{rep + 1}: fps {fps_values(text)}', flush=True)
            for label, nr in NR_PLACE_PHASES:
                iq.write_text(f'{NR_PLACE_IQ} {nr}\n')
                time.sleep(4)
                shots[(mode, label)] = game.capture(f'{mode}_{label.replace(" ", "_")}')
    finally:
        game.close()
    lines = ['nrplace: DLSS-NR post vs pre-SR, flat stereo, eye R own DLSS, 2880x1620 quality, '
             'both eyes processed (BB_XR_DLSSNR_FLAT_PAIRS=1); busy = total - GPU idle', '']
    summary = {}
    for mode, label, blocks, fps in results:
        if not blocks:
            lines.append(f'{mode} / {label}: no profile')
            continue
        busy = [t - sum(v for k, v in labels.items() if 'between submissions' in k)
                for t, labels in blocks]
        parts = {}
        for _, labels in blocks:
            for k, v in labels.items():
                if k.startswith(('VR DLSS', 'stereo: eye R', 'VR IQ')):
                    parts[k] = parts.get(k, 0.0) + v / len(blocks)
        lines.append(f'{mode} / {label}:')
        lines.append(f'  busy {np.mean(busy):.2f} ms (blocks: {", ".join(f"{b:.2f}" for b in busy)})')
        for k, v in sorted(parts.items()):
            lines.append(f'    {v:6.3f} ms  {k}')
        if fps:
            lines.append(f'  fps {min(fps):.0f}-{max(fps):.0f} (median {np.median(fps):.1f})')
        s = summary.setdefault((mode, label), {'busy': [], 'fps': [], 'nr': []})
        s['busy'].append(np.mean(busy))
        s['fps'] += fps
        s['nr'].append(sum(v for k, v in parts.items() if k.startswith('VR DLSS-NR')))
    lines.append('')
    lines.append('summary (mean of the two rounds):')
    for (mode, label), s in summary.items():
        lines.append(f'  {mode:6s} {label:10s} busy {np.mean(s["busy"]):6.2f} ms, DLSS-NR '
                     f'{np.mean(s["nr"]):5.2f} ms, fps median '
                     f'{np.median(s["fps"]) if s["fps"] else float("nan"):.1f}')
    png = out / 'png'
    png.mkdir(exist_ok=True)
    from PIL import Image
    for (mode, label), folder in shots.items():
        for raw in sorted(folder.glob('output*.raw'))[:2]:
            name = f'{mode}_{label.replace(" ", "_")}_{raw.stem}.png'
            Image.fromarray(poc.frame_png(raw)).save(png / name)
    lines.append('')
    lines.append(f'images: {png}')
    poc.report(out, lines)


def extra_env(a):
    env = dict(EXTRA_ENV)
    if a.xr_mode:
        env['BB_XR_MODE'] = a.xr_mode
    for kv in a.env:
        key, _, value = kv.partition('=')
        env[key] = value
    return env


def oracle(a):
    check_no_other_probe()
    poc.oracle(argparse.Namespace(out=a.out, probe=a.probe, game_dir=a.game_dir, offset=a.offset))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mode', choices=['replay', 'cost', 'oracle', 'look', 'timing', 'warp',
                                     'warpcost', 'full', 'fullcost', 'nrplace'])
    ap.add_argument('--probes', default='', help='timing: comma-separated probe executables')
    ap.add_argument('--repeats', type=int, default=2, help='timing: runs per probe')
    ap.add_argument('--xr-mode', default=None, help='BB_XR_MODE override (look)')
    ap.add_argument('--env', action='append', default=[], help='extra KEY=VALUE (look)')
    ap.add_argument('--out', default=None)
    ap.add_argument('--probe', default=str(PORT / 'out' / 'gpu' / 'bb-probe-stereo.exe'))
    ap.add_argument('--game-dir', default='C:/games/CUSA03173')
    ap.add_argument('--ipd', type=float, default=0.063)
    ap.add_argument('--offset', type=float, default=0.0315)
    ap.add_argument('--seconds', type=float, default=15.0, help='cost/timing: seconds measured')
    ap.add_argument('--yaw', type=float, default=0.0, help='head yaw (degrees) during the checks')
    ap.add_argument('--analyze', action='store_true', help='replay, warp: only analyze an earlier run in --out')
    a = ap.parse_args()
    a.out = a.out or str(PORT / 'out' / 'vr_true_stereo' / a.mode)
    Path(a.out).mkdir(parents=True, exist_ok=True)
    {'replay': replay, 'cost': cost, 'oracle': oracle, 'look': look, 'timing': timing,
     'warp': warp, 'warpcost': warpcost, 'full': full, 'fullcost': fullcost,
     'nrplace': nrplace}[a.mode](a)


if __name__ == '__main__':
    main()
