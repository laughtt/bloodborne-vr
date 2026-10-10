#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Headset-free check of VR head tracking (docs/vr/PLAN.md Phase 2, src/runtime_vr_head.c).

Runs the game flat with the camera hooks H1/H2, BB_VR_HEAD=1 and a scripted fake head
(BB_VR_FAKE_HEAD=@file), enters the world from the title screen (cross, cross: a save that
continues), captures the presented frame (BB_FRAME_DUMP_TRIGGER) at several head poses and
compares them. The injected rotation reaches the render when a turned capture differs from the
straight one far more than two straight captures differ from each other, and the picture moves
the expected way (a head turned left moves the scene right; a head raised moves it down).

    python tools/vr_head_test.py [--out DIR] [--probe out/gpu/bb-probe.exe] [--analyze-only]

The game ends itself (BB_EXIT_FILE, with BB_EXIT_AFTER_SECONDS as a safety net); this script
never kills it. Saves are protected: the game runs on a copy of user/ in DIR (--user to use
another folder). Results: DIR/report.txt, DIR/png/*.png (half size), DIR/session.log.
Needs numpy and Pillow for the comparison.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent.parent
HOOKS = ('VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);'
         'VR LOD Fix (SetCamera)')

# (label, fake head file, what it shows)
STEPS = [
    ('straight', 'yaw=0', 'baseline'),
    ('yaw10', 'yaw=10', 'head 10 deg left, both hooks'),
    ('straight2', 'yaw=0', 'baseline again: the animation noise level'),
    ('yaw10_h1', 'yaw=10 apply=1', 'head 10 deg left, H1 only'),
    ('yaw10_h2', 'yaw=10 apply=2', 'head 10 deg left, H2 only'),
    ('pitch10', 'pitch=10', 'head 10 deg up, both hooks'),
    ('roll15', 'roll=15', 'head tilted 15 deg left'),
    ('fov100', 'fov=100', 'vertical FOV 100 deg (culling at the edges), LOD fix on'),
    ('fov100_nolod', 'fov=100 lod=0', 'vertical FOV 100 deg without the LOD fix'),
    ('fov100_h1', 'fov=100 apply=1', 'vertical FOV 100 deg, H1 only (culling without H2)'),
    ('yaw30', 'yaw=30', 'head 30 deg left'),
    ('yaw30_h1', 'yaw=30 apply=1', 'head 30 deg left, H1 only (culling without H2)'),
    ('straight3', 'yaw=0', 'baseline at the end'),
]


def log_text(path):
    try:
        return path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''


def wait_for(predicate, timeout, process, what):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        if process and process.poll() is not None:
            raise SystemExit(f'the game ended while waiting for {what}')
        time.sleep(1)
    raise SystemExit(f'timed out waiting for {what}')


def press(pad, token, hold=0.3):
    pad.write_text(token + '\n')
    time.sleep(hold)
    pad.write_text('')


def draws_per_frame(text):
    found = re.findall(r'^Frame stats: .*? ([0-9.]+) draws/frame', text, re.M)
    return float(found[-1]) if found else 0.0


def run_game(args, out):
    pad, head, trigger, exit_file = out / 'pad', out / 'head.txt', out / 'dump.trigger', out / 'exit'
    dump = out / 'dump'
    if dump.exists():
        shutil.rmtree(dump)
    dump.mkdir(parents=True)
    for f in (trigger, exit_file):
        f.unlink(missing_ok=True)
    pad.write_text('')
    head.write_text('yaw=0\n')
    user = Path(args.user) if args.user else out / 'user'
    if not args.user:
        if user.exists():
            shutil.rmtree(user)
        shutil.copytree(PORT / 'user', user)
    env = dict(os.environ)
    for key in ('BB_VR', 'BB_XR_MODE', 'BB_VR_HEAD_FOV', 'BB_VR_HEAD_POS', 'BB_VR_HEAD_LEVEL'):
        env.pop(key, None)
    env.update(BB_HOOKS=HOOKS, BB_VR_HEAD='1', BB_VR_HEAD_LOG='1', BB_VR_FAKE_HEAD='@' + str(head),
               BB_PAD_FILE=str(pad), BB_FRAME_STATS='1', BB_FRAME_DUMP_TRIGGER=str(trigger),
               BB_DUMP_DIR=str(dump), BB_EXIT_FILE=str(exit_file),
               BB_EXIT_AFTER_SECONDS=str(args.timeout), BB_USER_DIR=str(user))
    if args.probe:
        env['BB_PROBE'] = str(Path(args.probe).resolve())
    log = out / 'session.log'
    print(f'starting the game (log {log})')
    with open(log, 'wb') as sink:
        process = subprocess.Popen([sys.executable, str(PORT / 'run.py')], cwd=PORT, env=env,
                                   stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT)
    captured = []
    try:
        wait_for(lambda: log_text(log).count('Frame stats') >= 3 and 'pad opened' in log_text(log),
                 240, process, 'the title screen')
        time.sleep(args.title_wait)
        press(pad, 'cross')
        time.sleep(6)
        press(pad, 'cross')
        wait_for(lambda: draws_per_frame(log_text(log)) > 600, 180, process, 'the world (draws/frame > 600)')
        time.sleep(10)
        text = log_text(log)
        for hook in ('H1', 'H2'):
            print(f'{hook} hook fired: {"yes" if f"VR head: {hook} hook fired" in text else "NO"}')
        for label, content, _ in STEPS:
            head.write_text(content + '\n')
            time.sleep(args.settle)
            before = set(dump.glob('output_*.raw'))
            trigger.write_text('')
            wait_for(lambda: set(dump.glob('output_*.raw')) - before, 20, process, f'the {label} capture')
            time.sleep(1.0)
            new = sorted(set(dump.glob('output_*.raw')) - before)[0]
            captured.append((label, new.name, len(log_text(log))))
            print(f'captured {label}: {new.name}')
        (out / 'captures.txt').write_text(''.join(f'{label} {name} {end}\n' for label, name, end in captured))
    finally:
        exit_file.write_text('')
        try:
            process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            print('the game did not end within 60 s of BB_EXIT_FILE; it ends itself after '
                  f'BB_EXIT_AFTER_SECONDS={args.timeout}')
    return captured


def load_gray(path):
    import numpy as np
    w, h = map(int, re.search(r'_(\d+)x(\d+)\.raw$', path.name).groups())
    pixels = np.frombuffer(path.read_bytes()[:w * h * 4], dtype=np.uint8).reshape(h, w, 4)
    rgb = pixels[:, :, 2::-1].astype(np.float32)  # B,G,R,A -> R,G,B
    return rgb, rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)


def phase_shift(a, b, scale=4):
    """(dx, dy) in full-size pixels with b(x) ~ a(x - d), and the peak strength."""
    import numpy as np
    a = a[::scale, ::scale]
    b = b[::scale, ::scale]
    window = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa = np.fft.fft2((a - a.mean()) * window)
    fb = np.fft.fft2((b - b.mean()) * window)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-6
    corr = np.fft.ifft2(r).real
    dy, dx = np.unravel_index(np.argmax(corr), corr.shape)
    h, w = corr.shape
    dy = dy - h if dy > h // 2 else dy
    dx = dx - w if dx > w // 2 else dx
    return dx * scale, dy * scale, float(corr.max())


def analyze(out):
    import numpy as np
    from PIL import Image
    captures = [line.split() for line in (out / 'captures.txt').read_text().splitlines() if line.strip()]
    images = {c[0]: load_gray(out / 'dump' / c[1]) for c in captures}
    # The main context's FOV and LOD scale logged during each step (H2 line, BB_VR_HEAD_LOG=1).
    text, lod, start = log_text(out / 'session.log'), {}, 0
    for c in captures:
        end = int(c[2]) if len(c) > 2 else len(text)
        found = re.findall(r'main context: fov ([0-9.]+) .*?LOD scale ([0-9.]+)', text[start:end])
        lod[c[0]] = f'ctx fov {found[-1][0]} LOD {found[-1][1]}' if found else ''
        start = end
    png = out / 'png'
    png.mkdir(exist_ok=True)
    for label, (rgb, _) in images.items():
        image = Image.fromarray(rgb.clip(0, 255).astype(np.uint8))
        image.resize((image.width // 2, image.height // 2)).save(png / f'{label}.png')
    describe = {label: what for label, _, what in STEPS}
    base_rgb, base = images['straight']
    lines = ['VR head test: mean absolute difference (0..255) to "straight", and the picture shift',
             '(dx > 0: the scene moved right, dy > 0: down; full-size pixels, phase correlation)', '']
    diffs = {}
    for label, (rgb, gray) in images.items():
        if label == 'straight':
            continue
        diff = float(np.abs(rgb - base_rgb).mean())
        dx, dy, peak = phase_shift(base, gray)
        diffs[label] = (diff, dx, dy, peak)
        lines.append(f'{label:12s} diff {diff:6.2f}  shift dx {dx:+5d} dy {dy:+5d} (peak {peak:.3f})  '
                     f'{describe[label]}  [{lod.get(label, "")}]')
    noise = max(diffs['straight2'][0], diffs['straight3'][0], 0.5)
    turned = diffs['yaw10']
    reaches = turned[0] > 3 * noise and turned[1] > 0
    lines += ['', f'noise (straight vs straight2/3): {noise:.2f}',
              f'RESULT: the injected head rotation {"REACHES" if reaches else "does NOT reach"} the rendered image '
              f'(yaw10 diff {turned[0]:.2f} vs noise {noise:.2f}, scene moved {"right" if turned[1] > 0 else "left/none"}).']
    h1, h2 = diffs['yaw10_h1'], diffs['yaw10_h2']
    lines.append(f'H1 only: diff {h1[0]:.2f} shift {h1[1]:+d}; H2 only: diff {h2[0]:.2f} shift {h2[1]:+d} '
                 f'(both: {turned[0]:.2f} shift {turned[1]:+d})')
    # H1 alone vs both hooks (does H2's context cull?), and the LOD fix's effect.
    for a, b in (('yaw10', 'yaw10_h1'), ('yaw30', 'yaw30_h1'), ('fov100', 'fov100_h1'), ('fov100', 'fov100_nolod')):
        if a in images and b in images:
            d = np.abs(images[a][0] - images[b][0]).mean(axis=(0, 2))
            bands = ' '.join(f'{d[i * len(d) // 8:(i + 1) * len(d) // 8].mean():5.1f}' for i in range(8))
            lines.append(f'{a} vs {b}: diff {float(d.mean()):.2f}; by column band, left to right: {bands}')
    report = '\n'.join(lines) + '\n'
    (out / 'report.txt').write_text(report)
    print(report)
    return reaches


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=PORT / 'out' / 'vr_head_test', help='results folder')
    parser.add_argument('--probe', help='bb-probe.exe to run (default: run.py\'s choice)')
    parser.add_argument('--user', help='user folder to run on (default: a copy of user/ in --out)')
    parser.add_argument('--timeout', type=int, default=480, help='BB_EXIT_AFTER_SECONDS safety net')
    parser.add_argument('--title-wait', type=float, default=6.0, help='seconds at the title screen')
    parser.add_argument('--settle', type=float, default=2.0, help='seconds between a pose and its capture')
    parser.add_argument('--analyze-only', action='store_true', help='compare the captures of an earlier run')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not args.analyze_only:
        run_game(args, out)
    return 0 if analyze(out) else 1


if __name__ == '__main__':
    sys.exit(main())
