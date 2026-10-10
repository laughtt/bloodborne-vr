#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Headset-free image-quality captures for VR (docs/vr/findings/30-vr-image-quality.md).

Runs the game flat with the VR camera hooks, a fake head rendering the headset's eye frustum
(115.2 x 97.1 deg with the default overscan: BB_VR_HEAD fov/aspect), BB_XR_MODE=alt, enters the
world (cross, cross) and, for every view and every variant, dumps the presented frame
(BB_FRAME_DUMP_TRIGGER: output_*.raw is the image an eye swapchain gets) and keeps the GPU profile
(BB_GPU_PROFILE=1) printed while the variant was on. Variants switch the presenter's VR eye
passes live through BB_XR_IQ_CONTROL (vk_xr_eye_iq.h: aa=off|fxaa upscale=off|fsr1 sharpen=<0..1>
res=WxH); patches and BB_RENDER_RES need a separate run (--patches, --env).

    python tools/vr_iq_capture.py --out out/vr_iq/run1 --probe out/gpu/bb-probe-iq.exe \
        --game-dir C:/games/CUSA03173 [--patches '...'] [--env KEY=VALUE ...] \
        [--variants 'off:aa=off upscale=off;fxaa:aa=fxaa'] [--analyzer]

The game runs on a copy of user/ and its own BB_DATA_DIR in --out, ends itself (BB_EXIT_FILE);
this script never kills it. Results: DIR/captures.txt (label -> dump files, log span), DIR/png/,
DIR/session.log, DIR/capture/ (frame analyzer, --analyzer).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from vr_head_test import draws_per_frame, log_text, press, wait_for  # noqa: E402

HOOKS = ('VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);'
         'VR LOD Fix (SetCamera)')
EFFECTS_OFF = ('Disable Motion Blur (perf increase);Disable AA;Disable DoF;'
               'Disable Chromatic Aberration')
# The Dream Air's eye union with BB_XR_OVERSCAN 0.15/0.10 (vertical FOV, tangent aspect).
EYE_FOV = 'fov=97.1 aspect=1.178'
VIEWS = 'near:yaw=0 pitch=-5;far:yaw=180 pitch=6'
VARIANTS = 'off:aa=off upscale=off'


def parse(spec):
    out = []
    for item in spec.split(';'):
        if item.strip():
            label, _, text = item.partition(':')
            out.append((label.strip(), text.strip()))
    return out


def to_png(raw, png):
    import numpy as np
    from PIL import Image
    import re
    w, h = map(int, re.search(r'_(\d+)x(\d+)\.raw$', raw.name).groups())
    px = np.frombuffer(raw.read_bytes()[:w * h * 4], dtype=np.uint8).reshape(h, w, 4)
    Image.fromarray(px[:, :, 2::-1].copy()).save(png)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--probe')
    p.add_argument('--game-dir')
    p.add_argument('--patches', default=EFFECTS_OFF, help="BB_PATCHES ('' = the game's defaults)")
    p.add_argument('--upscaler', default='off', help='BB_UPSCALER')
    p.add_argument('--env', action='append', default=[], help='extra KEY=VALUE')
    p.add_argument('--views', default=VIEWS)
    p.add_argument('--variants', default=VARIANTS)
    p.add_argument('--stereo', default='alt=0', help='stereo control (alt=0: mono camera, comparable frames)')
    p.add_argument('--count', type=int, default=2, help='frames dumped per capture')
    p.add_argument('--settle', type=float, default=7.0, help='seconds per variant before the dump (profile window 5 s)')
    p.add_argument('--analyzer', action='store_true', help='frame analyzer capture at the first view')
    p.add_argument('--eye-fov', default=EYE_FOV, help="fake head fov/aspect ('' = the game's)")
    p.add_argument('--flat', action='store_true', help='control run: no VR hooks, no fake head, no alt mode')
    p.add_argument('--timeout', type=int, default=900)
    p.add_argument('--title-wait', type=float, default=6.0)
    args = p.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    pad, head, control, iq = out / 'pad', out / 'head.txt', out / 'stereo.txt', out / 'iq.txt'
    trigger, exit_file, dump, cap_trigger = out / 'dump.trigger', out / 'exit', out / 'dump', out / 'capture.trigger'
    if dump.exists():
        shutil.rmtree(dump)
    dump.mkdir(parents=True)
    (out / 'capture').mkdir(exist_ok=True)
    for f in (trigger, exit_file, cap_trigger):
        f.unlink(missing_ok=True)
    pad.write_text('')
    head.write_text(f'yaw=0 {args.eye_fov}\n')
    control.write_text(args.stereo + '\n')
    variants = parse(args.variants)
    views = parse(args.views)
    iq.write_text(variants[0][1] + '\n')
    user = out / 'user'
    if user.exists():
        shutil.rmtree(user)
    shutil.copytree(PORT / 'user', user, ignore=shutil.ignore_patterns('captures'))
    data = out / 'data'
    data.mkdir(exist_ok=True)
    env = dict(os.environ)
    for key in ('BB_VR', 'BB_XR_MODE', 'BB_VR_HEAD_FOV', 'BB_VR_HEAD_POS', 'BB_VR_FP', 'BB_PATCHES',
                'BB_UPSCALER', 'BB_FPS_LIMIT', 'BB_RENDER_RES', 'BB_OUTPUT_RES'):
        env.pop(key, None)
    env.update(BB_HOOKS=HOOKS, BB_VR_HEAD='1', BB_VR_HEAD_LEVEL='1', BB_VR_FAKE_HEAD='@' + str(head),
               BB_XR_MODE='alt', BB_VR_STEREO_LOG='1', BB_VR_STEREO_CONTROL=str(control),
               BB_XR_IQ_CONTROL=str(iq), BB_PAD_FILE=str(pad), BB_FRAME_STATS='1', BB_GPU_PROFILE='1',
               BB_FRAME_DUMP_TRIGGER=str(trigger), BB_FRAME_DUMP_COUNT=str(args.count),
               BB_DUMP_DIR=str(dump), BB_EXIT_FILE=str(exit_file), BB_CAPTURE_TRIGGER=str(cap_trigger),
               BB_CAPTURE_DIR=str(out / 'capture'), BB_EXIT_AFTER_SECONDS=str(args.timeout),
               BB_USER_DIR=str(user), BB_DATA_DIR=str(data), BB_CONFIG=str(data / 'bbport.ini'),
               BB_PAD_KEYBOARD='0', BB_UPSCALER=args.upscaler, BB_FPS_LIMIT='72')
    if args.patches:
        env['BB_PATCHES'] = args.patches
    if args.flat:
        for key in ('BB_HOOKS', 'BB_VR_HEAD', 'BB_VR_FAKE_HEAD', 'BB_XR_MODE', 'BB_VR_STEREO_CONTROL'):
            env.pop(key, None)
    for kv in args.env:
        k, _, v = kv.partition('=')
        env[k] = v
    if args.game_dir:
        env['BB_GAME_DIR'] = args.game_dir
    if args.probe:
        env['BB_PROBE'] = str(Path(args.probe).resolve())
    log = out / 'session.log'
    print(f'starting the game (log {log})', flush=True)
    sink = open(log, 'wb')
    process = subprocess.Popen([sys.executable, str(PORT / 'run.py')], cwd=PORT, env=env,
                               stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT)
    lines = []
    try:
        wait_for(lambda: log_text(log).count('Frame stats') >= 3 and 'pad opened' in log_text(log),
                 300, process, 'the title screen')
        time.sleep(args.title_wait)
        press(pad, 'cross')
        time.sleep(6)
        press(pad, 'cross')
        wait_for(lambda: draws_per_frame(log_text(log)) > 600, 240, process, 'the world')
        time.sleep(10)
        for vi, (view, head_text) in enumerate(views):
            head.write_text(f'{head_text} {args.eye_fov} step={vi}\n')
            control.write_text(f'{args.stereo} step={vi}\n')
            for label, iq_text in variants:
                iq.write_text(f'{iq_text}\n')
                start = len(log_text(log))
                time.sleep(args.settle)
                if args.analyzer and vi == 0 and label == variants[0][0]:
                    cap_trigger.write_text('')
                    time.sleep(4)
                before = set(dump.glob('output_*.raw'))
                trigger.write_text('')
                wait_for(lambda: len(set(dump.glob('output_*.raw')) - before) >= args.count, 30, process, 'a dump')
                time.sleep(1.5)
                new = sorted(set(dump.glob('output_*.raw')) - before)[:args.count]
                end = len(log_text(log))
                lines.append(f'{view}_{label} {start} {end} {" ".join(n.name for n in new)}\n')
                print(f'captured {view}_{label}: {", ".join(n.name for n in new)}', flush=True)
        (out / 'captures.txt').write_text(''.join(lines))
    finally:
        exit_file.write_text('')
        try:
            process.wait(timeout=90)
        except subprocess.TimeoutExpired:
            print('the game did not end within 90 s; it ends itself after BB_EXIT_AFTER_SECONDS')
        sink.close()
    png = out / 'png'
    png.mkdir(exist_ok=True)
    for line in lines:
        label, _, _, *names = line.split()
        for i, name in enumerate(names):
            to_png(dump / name, png / f'{label}_{i}.png')
    return 0


if __name__ == '__main__':
    sys.exit(main())
