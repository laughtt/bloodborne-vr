#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Headset-free checks of Phase 4 true stereo (BB_XR_MODE=stereo, docs/vr/findings/31-true-stereo.md).
The game runs flat (BB_VR=0, fake head) on a copy of user/ with its own data folder, like
tools/vr_stereo_poc.py, whose session helpers it reuses, and ends itself.

    python tools/vr_true_stereo_test.py replay [--ipd 0.063] [--out DIR] [--probe EXE]
    python tools/vr_true_stereo_test.py cost   [--out DIR] [--probe EXE] [--seconds 15]
    python tools/vr_true_stereo_test.py oracle [--offset 0.0315] [--out DIR] [--probe EXE]

replay  P4.3, record once / execute twice. One session, three doubled G-buffer passes:
        1. identity: eye R at eye L's position ("same=1"): every eye-R target must equal eye L's
           bit for bit (the replay reproduces the recorded commands exactly);
        2. stereo: eyes at -/+ ipd/2 with the replay: eye R vs eye L warped by depth (as
           vr_stereo_poc.py dup), and the log's counts (passes executed twice, none cut short,
           no draw outside a capture);
        3. the same with the prototype's per-draw second draw ("replay=0"), for comparison.
cost    GPU time per frame (BB_GPU_PROFILE=1): stereo off in the region, every frame's G-buffer
        doubled per draw (prototype), doubled by record once / execute twice.
oracle  P4.1: tools/vr_stereo_poc.py's oracle (the shift against the CPU camera moved, H1).

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
    setups = [('identity', f'offset=0 ipd={a.ipd} same=1 replay=1 guardtest=0 dup=1'),
              ('replay', f'offset=0 ipd={a.ipd} same=0 replay=1 guardtest=0 dup=2'),
              ('perdraw', f'offset=0 ipd={a.ipd} same=0 replay=0 guardtest=0 dup=3'),
              # The write guard's negative control: every draw counts as writing memory, so
              # no doubled pass may get an eye R (eye R's targets then differ from eye L's).
              ('guard', f'offset=0 ipd={a.ipd} same=1 replay=1 guardtest=1 dup=4')]
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
            fired = bool(g) and g.group(1) == '0' and int(g.group(2)) > 0 and differ > 0
            ok &= fired
            lines.append(f'  guard fired: {g.group(2) if g else "?"} passes without eye R, '
                         f'{g.group(1) if g else "?"} executed twice, {differ} targets differ '
                         f'-> {"OK" if fired else "FAIL"}')
            continue
        if label == 'identity':
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
    ap.add_argument('mode', choices=['replay', 'cost', 'oracle', 'look', 'timing'])
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
    ap.add_argument('--analyze', action='store_true', help='replay: only analyze an earlier run in --out')
    a = ap.parse_args()
    a.out = a.out or str(PORT / 'out' / 'vr_true_stereo' / a.mode)
    Path(a.out).mkdir(parents=True, exist_ok=True)
    {'replay': replay, 'cost': cost, 'oracle': oracle, 'look': look, 'timing': timing}[a.mode](a)


if __name__ == '__main__':
    main()
