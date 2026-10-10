#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Headset-free checks of the Phase 4 stereo prototype (BB_XR_MODE=stereo, vk_stereo.h,
docs/vr/findings/23-phase4-renderer-stereo.md). The game runs flat on a copy of user/ with its own
data folder (a game already running from out/ is not disturbed) and ends itself.

    python tools/vr_stereo_poc.py oracle [--offset 0.25] [--out DIR] [--probe EXE]
    python tools/vr_stereo_poc.py dup [--ipd 0.063] [--out DIR] [--probe EXE]

oracle  the eye shift in the vertex shaders against the real thing: frames of the center camera,
        of the center camera with every perspective draw of the scene shifted as if the camera
        stood --offset metres to the right (the prototype's remap), and of the CPU camera really
        moved there (the H1 hook with a fake head position). Reports the mean difference on
        pixels that do not animate (two captures of each setup decide which those are).
dup     one frame's G-buffer pass drawn a second time into twin images with the other eye's
        offset; both eyes' targets are dumped (BB_DUMP_DIR) and eye R is compared with eye L
        warped by its depth and the expected disparity P00 * ipd / z * width / 2.

Settings: upscaler none, motion blur, DoF, chromatic aberration and the game's AA off, uncapped.
Needs numpy and Pillow. Results: DIR/report.txt, DIR/png/.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

PORT = Path(__file__).resolve().parent.parent
HOOKS = 'VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context)'
INI = ['effect_motion_blur=0', 'effect_dof=0', 'effect_chromatic_aberration=0',
       'effect_game_aa=0', 'upscaler=off']
P00, NEAR, FAR = 1.42799, 0.0500679, 3000.0  # the game's projection at 16:9, 43 deg


def log_text(path):
    try:
        return path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''


def wait_for(predicate, timeout, process, what):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return
        if process.poll() is not None:
            raise SystemExit(f'the game ended while waiting for {what}')
        time.sleep(0.5)
    raise SystemExit(f'timed out waiting for {what}')


def draws_per_frame(text):
    found = re.findall(r'^Frame stats: .*? ([0-9.]+) draws/frame', text, re.M)
    return float(found[-1]) if found else 0.0


class Game:
    def __init__(self, out, probe, game_dir, timeout, extra_env):
        self.out = out
        self.pad, self.exit, self.head = out / 'pad', out / 'exit', out / 'head.txt'
        self.control, self.trigger, self.dump = out / 'stereo.txt', out / 'dump.trigger', out / 'dump'
        if self.dump.exists():
            shutil.rmtree(self.dump)
        self.dump.mkdir(parents=True)
        for f in (self.exit, self.trigger):
            f.unlink(missing_ok=True)
        self.pad.write_text('')
        self.head.write_text('yaw=0\n')
        self.control.write_text('offset=0\n')
        user, data = out / 'user', out / 'data'
        if not user.exists():
            shutil.copytree(PORT / 'user', user)
        data.mkdir(exist_ok=True)
        (data / 'bbport.ini').write_text('\n'.join(INI) + '\n')
        env = {k: v for k, v in os.environ.items() if not k.startswith('BB_')}
        env.update(BB_XR_MODE='stereo', BB_STEREO_CONTROL=str(self.control), BB_UPSCALER='none',
                   BB_HOOKS=HOOKS, BB_VR_HEAD='1', BB_VR_HEAD_POS='1',
                   BB_VR_FAKE_HEAD='@' + str(self.head), BB_PAD_FILE=str(self.pad),
                   BB_FRAME_STATS='1', BB_FRAME_DUMP_TRIGGER=str(self.trigger),
                   BB_DUMP_DIR=str(self.dump), BB_EXIT_FILE=str(self.exit),
                   BB_EXIT_AFTER_SECONDS=str(timeout), BB_USER_DIR=str(user), BB_DATA_DIR=str(data),
                   BB_CONFIG=str(data / 'bbport.ini'), BB_PAD_KEYBOARD='0', BB_GAME_DIR=game_dir,
                   BB_FPS_LIMIT='0', BB_PROBE=str(Path(probe).resolve()))
        env.update(extra_env)  # overrides (BB_XR_MODE, BB_UPSCALER, ...)
        self.log = out / 'session.log'
        with open(self.log, 'wb') as sink:
            self.process = subprocess.Popen([sys.executable, str(PORT / 'run.py')], cwd=PORT, env=env,
                                            stdin=subprocess.DEVNULL, stdout=sink,
                                            stderr=subprocess.STDOUT)

    def enter_world(self):
        wait_for(lambda: log_text(self.log).count('Frame stats') >= 3 and 'pad opened' in log_text(self.log),
                 300, self.process, 'the title screen')
        time.sleep(4)
        # Title: Continue, or (offline) Play Online, two network notices, then Continue.
        for _ in range(6):
            if draws_per_frame(log_text(self.log)) > 600:
                break
            self.pad.write_text('cross\n')
            time.sleep(0.3)
            self.pad.write_text('')
            time.sleep(6)
        wait_for(lambda: draws_per_frame(log_text(self.log)) > 600, 240, self.process, 'the world')
        time.sleep(10)

    def capture(self, label, settle=1.5):
        time.sleep(settle)
        before = set(self.dump.glob('*.raw'))
        self.trigger.write_text('')
        wait_for(lambda: set(self.dump.glob('output_*.raw')) - before, 30, self.process, label)
        time.sleep(1.5)
        target = self.dump / label
        target.mkdir(exist_ok=True)
        for f in sorted(set(self.dump.glob('*.raw')) - before):
            f.rename(target / f.name)
        return target

    def close(self):
        self.exit.write_text('')
        try:
            self.process.wait(timeout=90)
        except subprocess.TimeoutExpired:
            print('the game has not ended yet (BB_EXIT_AFTER_SECONDS will end it)')


def frame_png(raw):
    m = re.search(r'_(\d+)x(\d+)\.raw$', raw.name)
    w, h = int(m.group(1)), int(m.group(2))
    return np.fromfile(raw, dtype=np.uint8)[:w * h * 4].reshape(h, w, 4)[..., [2, 1, 0]]


def oracle(a):
    out = Path(a.out)
    game = Game(out, a.probe, a.game_dir, 600, {})
    shots = {}
    try:
        game.enter_world()
        for rep in (0, 1):
            game.head.write_text('yaw=0\n')
            game.control.write_text('offset=0\n')
            shots[f'mono{rep}'] = game.capture(f'mono{rep}')
            game.control.write_text(f'offset={a.offset}\n')
            shots[f'remap{rep}'] = game.capture(f'remap{rep}')
            game.control.write_text('offset=0\n')
            game.head.write_text(f'yaw=0 x={a.offset}\n')
            shots[f'cpu{rep}'] = game.capture(f'cpu{rep}')
    finally:
        game.close()
    im = {k: frame_png(next(v.glob('output_*.raw'))).astype(np.float32) for k, v in shots.items()}
    h, w = im['mono0'].shape[:2]
    crop = (slice(int(h * 0.15), int(h * 0.95)), slice(int(w * 0.03), int(w * 0.97)))  # no HUD

    def amax(x, y):
        return np.max(np.abs(im[x][crop] - im[y][crop]), axis=-1)
    static = (amax('mono0', 'mono1') < 4) & (amax('cpu0', 'cpu1') < 4) & (amax('remap0', 'remap1') < 4)
    lines = [f'oracle: eye offset {a.offset} m, static (non-animated) pixels {100 * static.mean():.1f}%']
    for x, y, what in [('mono0', 'mono1', 'noise: center camera twice'),
                       ('mono0', 'cpu0', 'signal: center vs CPU camera moved'),
                       ('remap0', 'cpu0', 'remap vs CPU camera moved'),
                       ('remap1', 'cpu1', 'remap vs CPU camera moved (2nd)')]:
        d = np.mean(np.abs(im[x][crop] - im[y][crop]), axis=-1)[static]
        lines.append(f'  {what:38s} mean {d.mean():5.2f}  p99 {np.percentile(d, 99):5.1f}  '
                     f'>8: {100 * (d > 8).mean():5.2f}%')
    (out / 'png').mkdir(exist_ok=True)
    for k in ('mono0', 'remap0', 'cpu0'):
        Image.fromarray(im[k].astype(np.uint8)).resize((960, 540)).save(out / 'png' / f'{k}.png')
    report(out, lines)


def stereo_raw(dump, pattern):
    files = sorted(glob.glob(str(dump / f'stereo_*_{pattern}_*.raw')))
    if not files:
        raise SystemExit(f'no {pattern} dump: did the doubled pass run? (session.log)')
    raw = Path(files[0])
    w, h = (int(v) for v in re.search(r'_(\d+)x(\d+)\.raw$', raw.name).groups())
    return np.fromfile(raw, dtype=np.uint8), w, h


def dup(a):
    out = Path(a.out)
    game = Game(out, a.probe, a.game_dir, 600, {})
    try:
        game.enter_world()
        game.control.write_text(f'offset=0 ipd={a.ipd} dup=1\n')
        game.capture('after_dup', settle=2.0)
    finally:
        game.close()
    raw, w, h = stereo_raw(game.dump, 'L_R8G8B8A8Srgb')
    alb_l = raw[:w * h * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
    raw, _, _ = stereo_raw(game.dump, 'R_R8G8B8A8Srgb')
    alb_r = raw[:w * h * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
    a_, b_ = FAR / (FAR - NEAR), -NEAR * FAR / (FAR - NEAR)
    z = {}
    for eye in 'LR':
        raw, _, _ = stereo_raw(game.dump, f'{eye}_D32SfloatS8Uint')
        z[eye] = b_ / (raw[:w * h * 4].view(np.float32).reshape(h, w) - a_)
    ys, xs = np.mgrid[0:h, 0:w]
    valid = (z['R'] > NEAR) & (z['R'] < 2000)
    disparity = P00 * a.ipd / np.where(valid, z['R'], 1e9) * (w / 2)
    xl = np.clip(np.round(xs + disparity).astype(int), 0, w - 1)
    same = valid & (np.abs(z['L'][ys, xl] - z['R']) < 0.02 * z['R'] + 0.01)
    err = np.abs(alb_l[ys, xl] - alb_r).mean(-1)[same]
    err0 = np.abs(alb_l - alb_r).mean(-1)[same]
    lines = [f'dup: one frame\'s G-buffer pass drawn twice, eyes at -/+ {a.ipd / 2:.4f} m',
             f'  disparity: median {np.median(disparity[valid]):.1f} px, max {disparity[valid].max():.1f} px',
             f'  same surface in both eyes: {100 * same.mean():.1f}% of pixels',
             f'  albedo |R - L warped by depth|: mean {err.mean():.2f}, p95 {np.percentile(err, 95):.1f}',
             f'  albedo |R - L| without the warp: mean {err0.mean():.2f}, p95 {np.percentile(err0, 95):.1f}',
             f'  depth of a point, eye R vs eye L: median relative difference '
             f'{np.median(np.abs(z["L"][ys, xl] - z["R"])[same] / z["R"][same]):.5f}']
    (out / 'png').mkdir(exist_ok=True)
    Image.fromarray(np.concatenate([alb_l, alb_r], 1).astype(np.uint8)).resize((1920, 540)).save(
        out / 'png' / 'albedo_LR.png')
    ana = np.stack([alb_l.mean(-1), alb_r.mean(-1), alb_r.mean(-1)], -1).astype(np.uint8)
    Image.fromarray(ana).resize((960, 540)).save(out / 'png' / 'albedo_anaglyph.png')
    report(out, lines)


def report(out, lines):
    text = '\n'.join(lines) + '\n'
    (out / 'report.txt').write_text(text)
    print(text, end='')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mode', choices=['oracle', 'dup'])
    ap.add_argument('--out', default=str(PORT / 'out' / 'vr_stereo_p4' / 'run'))
    ap.add_argument('--probe', default=str(PORT / 'out' / 'gpu' / 'bb-probe-p4.exe'))
    ap.add_argument('--game-dir', default='C:/games/CUSA03173')
    ap.add_argument('--offset', type=float, default=0.25, help='oracle: eye offset in metres')
    ap.add_argument('--ipd', type=float, default=0.063, help='dup: eye separation in metres')
    a = ap.parse_args()
    Path(a.out).mkdir(parents=True, exist_ok=True)
    (oracle if a.mode == 'oracle' else dup)(a)


if __name__ == '__main__':
    main()
