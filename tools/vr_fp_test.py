#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Headset-free check of the first-person camera (BB_VR_FP=1, src/runtime_vr_fp.c,
docs/vr/findings/16-first-person.md).

Runs the game flat with the camera hooks (H1, H2, LOD fix, the first-person yaw sync), a scripted
fake head (BB_VR_FAKE_HEAD=@file) and a control file (BB_VR_FP_CONTROL: first person on/off and
its settings, re-read while the game runs), enters the world from the title screen (cross, cross:
a save that continues), then for each step sets the head, the settings and the pad (BB_PAD_FILE:
walking, a snap turn), captures the presented frame (BB_FRAME_DUMP_TRIGGER) and ends the game
with BB_EXIT_FILE (BB_EXIT_AFTER_SECONDS as a safety net); this script never kills it.

    python tools/vr_fp_test.py [--out DIR] [--probe out/gpu/bb-probe.exe] [--analyze-only]

The game runs on a copy of user/ in DIR and with BB_DATA_DIR=DIR/data (its generated boot files,
so a game already running from the repository's out/ is not disturbed). Results: DIR/report.txt,
DIR/png/<step>.png (half size), DIR/session.log, DIR/trace.csv (one line per frame: the player's
position and yaw, the eye in model space and in the world, the game camera's heading as drawn and
as the game built it, the first-person heading psi). Needs numpy and Pillow.
"""
import argparse
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from vr_head_test import draws_per_frame, load_gray, log_text, phase_shift, press, wait_for  # noqa: E402

HOOKS = ('VR Camera Hook H1 (draw PersCam);VR Camera Hook H2 (draw main context);'
         'VR LOD Fix (SetCamera);VR FP Camera Heading (in-game step)')

# (label, fake head file, control file, pad action, what it shows). Pad actions: '' none,
# 'walk' (left stick up for --walk seconds, captured while moving), 'snap' (right stick flicked
# right once), 'snapleft', 'turn' / 'turnleft' (right stick held 1 s: smooth turn).
STEPS = [
    ('tp', 'yaw=0', 'fp=0', '', 'third person: the game camera (first person off)'),
    ('fp', 'yaw=0', 'fp=1', '', 'first person, looking ahead'),
    ('fp2', 'yaw=0', 'fp=1', '', 'first person again: the noise level'),
    ('fp_down45', 'pitch=-45', 'fp=1', '', 'looking 45 deg down'),
    ('fp_down80', 'pitch=-80', 'fp=1', '', 'looking 80 deg down at the own body'),
    ('fp_down80_near005', 'pitch=-80', 'fp=1 near=0.05', '', 'the same with the game\'s near plane (0.05 m)'),
    ('fp_near005', 'yaw=0', 'fp=1 near=0.05', '', 'looking ahead with the game\'s near plane'),
    ('fp_up45', 'pitch=45', 'fp=1 near=0.2', '', 'looking 45 deg up (hat brim)'),
    ('fp_left90', 'yaw=90', 'fp=1', '', 'head turned 90 deg left (shoulder)'),
    ('fp_right100_down30', 'yaw=-100 pitch=-30', 'fp=1', '', 'head 100 deg right, 30 deg down'),
    ('fp_body', 'yaw=0', 'fp=1 anchor=body', '', 'body anchor (fixed eye height)'),
    ('fp_walk', 'yaw=0', 'fp=1 anchor=head', 'walk', 'walking forward, captured while moving'),
    ('fp_stop', 'yaw=0', 'fp=1', '', 'stopped after the walk'),
    ('fp_snap', 'yaw=0', 'fp=1', 'snap', 'one snap turn to the right'),
    ('fp_snap_walk', 'yaw=0', 'fp=1', 'walk', 'walking after the snap turn'),
    ('fp_snapleft', 'yaw=0', 'fp=1', 'snapleft', 'one snap turn to the left'),
    ('fp_headmove', 'yaw=60', 'fp=1 move=head', 'walk', 'head 60 deg left, head-relative movement: walks toward the head'),
    ('fp_bodymove', 'yaw=60', 'fp=1 move=body', 'walk', 'head 60 deg left, body-relative movement: walks along psi'),
    ('tp_end', 'yaw=0', 'fp=0', '', 'third person at the end'),
]
# --steps move: where the hunter walks after snap turns, with the game camera turned by the hook
# (sync) or the left stick rotated instead (remap), body- and head-relative.
STEPS_MOVE = [  # zig-zags around the spawn's open heading (~165 deg in the Hunter's Dream save)
    ('fp', 'yaw=0', 'fp=1', '', 'first person'),
    ('fp_snap', 'yaw=0', 'fp=1', 'snap', 'snap right'),
    ('fp_snap_walk', 'yaw=0', 'fp=1', 'walk', 'walking after it: along psi'),
    ('fp_snapleft', 'yaw=0', 'fp=1', 'snapleft', 'snap left'),
    ('fp_snapleft_walk', 'yaw=0', 'fp=1', 'walk', 'walking after it: along psi'),
    ('fp_headmove', 'yaw=-30', 'fp=1 move=head', 'walk', 'head 30 deg right, head-relative: walks toward the head'),
    ('fp_bodymove', 'yaw=-30', 'fp=1 move=body', 'walk', 'head 30 deg right, body-relative: walks along psi'),
    ('fp_shallow', 'yaw=0', 'fp=1 deep=0', 'snap', 'snap right, only ChrCam turned (deep=0)'),
    ('fp_shallow_walk', 'yaw=0', 'fp=1 deep=0', 'walk', 'walking: along the follow camera, not psi'),
    ('fp_remap', 'yaw=0', 'fp=1 deep=1 sync=0 remap=1', 'snapleft', 'snap left, the hook off, the stick remapped'),
    ('fp_remap_walk', 'yaw=0', 'fp=1 sync=0 remap=1', 'walk', 'walking: along psi through the remap'),
    ('tp_end', 'yaw=0', 'fp=0 sync=1 remap=-1', '', 'third person at the end'),
]
# --steps parts: which ChrAsm slot draws what (findings/16 4): third person (the game camera behind
# the hunter), one part hidden at a time (hide_tp=1 hides in third person too), then first person
# looking down with the groups hidden.
STEPS_PARTS = ([('tp', 'yaw=0', 'fp=0 hide=none hide_tp=1', '', 'third person, nothing hidden')] +
               [(f'tp_s{i}', 'yaw=0', f'fp=0 hide=s{i} hide_tp=1', '', f'third person, ChrAsm slot {i} hidden')
                for i in range(24)] +
               [('tp_base', 'yaw=0', 'fp=0 hide=base hide_tp=1', '', 'third person, the base body hidden'),
                ('tp_all', 'yaw=0', 'fp=0 hide=all hide_tp=1', '', 'third person, everything hidden'),
                ('fp_down', 'pitch=-70', 'fp=1 hide=none hide_tp=0 near=0.05', '', 'first person looking down, nothing hidden'),
                ('fp_down_all', 'pitch=-70', 'fp=1 hide=all near=0.05', '', 'first person looking down, everything hidden'),
                ('tp_end', 'yaw=0', 'fp=0 hide=none hide_tp=0', '', 'third person at the end: all back')])
# --steps verify: the hide groups (third person with hide_tp=1, then first person all around at the
# game's own 0.05 m near plane), repeated captures (no flicker), everything back afterwards, then
# the turn model: smooth turn (right stick held), snap, walking after them (the trace shows where).
STEPS_VERIFY = ([('tp', 'yaw=0', 'fp=0 hide=none hide_tp=1', '', 'third person, nothing hidden')] +
                [(f'tp_{g}', 'yaw=0', f'fp=0 hide={g} hide_tp=1', '', f'third person, hide={g}')
                 for g in ('head', 'body', 'all')] +
                [(f'tp_all_r{i}', 'yaw=0', f'fp=0 hide=all hide_tp=1 r={i}', '', 'hide=all again (flicker check)')
                 for i in range(1, 4)] +
                [('tp_none', 'yaw=0', 'fp=0 hide=none hide_tp=0', '', 'third person, hide=none: all back'),
                 ('fp_body_down', 'pitch=-70', 'fp=1 hide=body near=0.05', '', 'first person, hide=body, looking down'),
                 ('fp_body_ahead', 'yaw=0', 'fp=1 hide=body near=0.05', '', 'hide=body, looking ahead'),
                 ('fp_body_left', 'yaw=90 pitch=-20', 'fp=1 hide=body near=0.05', '', 'hide=body, head 90 left, 20 down (shoulder)'),
                 ('fp_body_right', 'yaw=-100 pitch=-30', 'fp=1 hide=body near=0.05', '', 'hide=body, head 100 right, 30 down'),
                 ('fp_body_back', 'yaw=160 pitch=-10', 'fp=1 hide=body near=0.05', '', 'hide=body, looking behind'),
                 ('fp_head_down', 'pitch=-70', 'fp=1 hide=head near=0.05', '', 'hide=head, looking down'),
                 ('fp_head_left', 'yaw=90 pitch=-20', 'fp=1 hide=head near=0.05', '', 'hide=head, shoulder'),
                 ('fp_none_left', 'yaw=90 pitch=-20', 'fp=1 hide=none near=0.2', '', 'nothing hidden, near 0.2: the old look (shoulder)'),
                 ('fp_all_down', 'pitch=-70', 'fp=1 hide=all near=0.05', '', 'hide=all, looking down'),
                 ('fp_smooth', 'yaw=0', 'fp=1 hide=body near=0.1 turn=smooth turn_speed=90', 'turn', 'smooth turn right, stick held 1 s'),
                 ('fp_smooth_walk', 'yaw=0', 'fp=1 turn=smooth', 'walk', 'walking after it: along psi'),
                 ('fp_smooth_left', 'yaw=0', 'fp=1 turn=smooth', 'turnleft', 'smooth turn left 1 s'),
                 ('fp_smooth_left_walk', 'yaw=0', 'fp=1 turn=smooth', 'walk', 'walking after it: along psi'),
                 ('fp_snap2', 'yaw=0', 'fp=1 turn=snap', 'snap', 'snap right'),
                 ('fp_snap2_walk', 'yaw=0', 'fp=1 turn=snap', 'walk', 'walking after it: along psi'),
                 ('fp_head_walk', 'yaw=-40', 'fp=1 move=head', 'walk', 'head 40 right, move=head: walks toward the head'),
                 ('fp_stable_walk', 'yaw=0', 'fp=1 move=body anchor=stable', 'walk', 'anchor=stable walking'),
                 ('tp_end', 'yaw=0', 'fp=0 hide=none hide_tp=0 anchor=head', '', 'third person at the end: all back')])
# --steps arms (findings/24): the arm IK hook (BB_VR_ARMS=1, "VR Arms IK (AnimationToModel)") with the
# rest pose and scripted (fake) grips against the game's own arms, and the interim camera offset.
_FP = 'fp=1 hide=body near=0.08'
STEPS_ARMS = [
    ('tp', 'yaw=0', 'fp=0 arm=0', '', 'third person, the game arms'),
    ('tp_rest', 'yaw=0', 'fp=0 arm=1 arm_tp=1 arm_src=rest', '', 'third person, arms in the rest pose (arm_tp=1)'),
    ('fp_game', 'pitch=-30', f'{_FP} arm=0 arm_tp=0', '', 'first person, 30 deg down: the game arms'),
    ('fp_rest', 'pitch=-30', f'{_FP} arm=1 arm_src=rest', '', 'the rest pose'),
    ('fp_rest_ahead', 'yaw=0', f'{_FP} arm=1 arm_src=rest', '', 'the rest pose, looking ahead'),
    ('fp_fake', 'pitch=-30', f'{_FP} arm=1 arm_src=fake', '', 'scripted grips 0.38 m below, 0.32 m ahead of the head'),
    ('fp_fake_high', 'yaw=0', f'{_FP} arm=1 arm_src=fake arm_fake_r=0.2,-0.12,-0.4 arm_fake_l=-0.2,-0.12,-0.4',
     '', 'scripted grips raised ahead (0.12 m below the eyes)'),
    ('fp_fake_right_up', 'yaw=0', f'{_FP} arm=1 arm_src=fake arm_fake_r=0.25,0.1,-0.3,0,40,0 arm_fake_l=-0.2,-0.38,-0.32',
     '', 'right grip raised above the eyes, pitched up'),
    ('fp_rest_walk', 'pitch=-30', f'{_FP} arm=1 arm_src=rest', 'walk', 'walking with the rest pose'),
    ('fp_off', 'pitch=-30', f'{_FP} arm=0', '', 'arm=0: the game arms back'),
    ('fp_cam_offset', 'pitch=-30', f'{_FP} arm=0 up=-0.08 forward=-0.10', '', 'the interim camera offset (up -0.08, forward -0.10)'),
    ('tp_end', 'yaw=0', 'fp=0 arm=0 up=0 forward=0', '', 'third person at the end'),
]
STEP_SETS = {'full': STEPS, 'move': STEPS_MOVE, 'parts': STEPS_PARTS, 'verify': STEPS_VERIFY, 'arms': STEPS_ARMS}


def run_game(args, out):
    pad, head, control = out / 'pad', out / 'head.txt', out / 'control.txt'
    trigger, exit_file, dump = out / 'dump.trigger', out / 'exit', out / 'dump'
    if dump.exists():
        shutil.rmtree(dump)
    dump.mkdir(parents=True)
    for f in (trigger, exit_file):
        f.unlink(missing_ok=True)
    pad.write_text('')
    head.write_text('yaw=0\n')
    control.write_text('fp=0\n')
    user = Path(args.user) if args.user else out / 'user'
    if not args.user:
        if user.exists():
            shutil.rmtree(user)
        shutil.copytree(PORT / 'user', user)
    data = out / 'data'
    data.mkdir(exist_ok=True)
    env = dict(os.environ)
    for key in ('BB_VR', 'BB_XR_MODE', 'BB_VR_HEAD_FOV', 'BB_VR_HEAD_POS', 'BB_VR_HEAD_LEVEL', 'BB_VR_SNAP_DEG'):
        env.pop(key, None)
    env.update(BB_HOOKS=HOOKS, BB_VR_HEAD='1', BB_VR_FAKE_HEAD='@' + str(head), BB_VR_FP='1',
               BB_VR_FP_LOG='1', BB_VR_FP_CONTROL=str(control), BB_VR_FP_TRACE=str(out / 'trace.csv'),
               BB_VR_SNAP_DEG='30', BB_PAD_FILE=str(pad), BB_FRAME_STATS='1',
               BB_FRAME_DUMP_TRIGGER=str(trigger), BB_DUMP_DIR=str(dump), BB_EXIT_FILE=str(exit_file),
               BB_EXIT_AFTER_SECONDS=str(args.timeout), BB_USER_DIR=str(user), BB_DATA_DIR=str(data),
               BB_CONFIG=str(data / 'bbport.ini'), BB_PAD_KEYBOARD='0')
    if args.steps == 'arms':
        env.update(BB_HOOKS=HOOKS + ';VR Arms IK (AnimationToModel)', BB_VR_ARMS='1', BB_VR_ARMS_LOG='1')
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
    try:
        wait_for(lambda: log_text(log).count('Frame stats') >= 3 and 'pad opened' in log_text(log),
                 240, process, 'the title screen')
        time.sleep(args.title_wait)
        press(pad, 'cross')
        time.sleep(6)
        press(pad, 'cross')
        wait_for(lambda: draws_per_frame(log_text(log)) > 600, 180, process, 'the world (draws/frame > 600)')
        time.sleep(10)
        for index, (label, head_text, control_text, action, _) in enumerate(STEP_SETS[args.steps]):
            head.write_text(head_text + '\n')
            control.write_text(f'{control_text} step={index}\n')  # a change per step: re-read
            time.sleep(args.settle)
            if action in ('snap', 'snapleft'):
                press(pad, 'rx=255' if action == 'snap' else 'rx=0', 0.2)
                time.sleep(1.0)
            if action in ('turn', 'turnleft'):  # smooth turn: the stick held for 1 s
                press(pad, 'rx=255' if action == 'turn' else 'rx=0', 1.0)
                time.sleep(1.0)
            if action == 'walk':
                pad.write_text('ly=0\n')
                time.sleep(args.walk)
            before = set(dump.glob('output_*.raw'))
            trigger.write_text('')
            wait_for(lambda: set(dump.glob('output_*.raw')) - before, 20, process, f'the {label} capture')
            if action == 'walk':
                pad.write_text('')
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


def read_trace(path):
    rows = []
    if not path.is_file():
        return rows
    lines = path.read_text().splitlines()
    names = lines[0].split(',')
    for line in lines[1:]:
        parts = line.split(',')
        if len(parts) != len(names):
            continue
        row = {}
        for k, v in zip(names, parts):
            try:
                row[k] = float(v)
            except ValueError:
                row[k] = v  # the turn and move names
        rows.append(row)
    return rows


def analyze_trace(rows):
    """Each walk's direction against the first-person heading psi, the game camera's heading and
    the hunter's facing; the eye above and in front of the feet (findings/16 section 2)."""
    out = []
    if len(rows) < 10:
        return ['trace: too few frames']

    def wrap(x):
        return (x + math.pi) % (2 * math.pi) - math.pi

    def med(values):
        values = sorted(values)
        return math.degrees(values[len(values) // 2]) if values else float('nan')
    moving = []
    for a, b in zip(rows, rows[1:]):
        dx, dz, dt = b['px'] - a['px'], b['pz'] - a['pz'], b['t'] - a['t']
        speed = math.hypot(dx, dz) / dt if dt > 0 else 0.0
        if 3.0 < speed < 12.0:  # running (not sliding along a wall, not a teleport)
            moving.append((math.atan2(dx, dz), speed, b))
    # One walk = moving frames with gaps under 0.3 s; the first third is the turn into it.
    segments, current = [], []
    for m in moving:
        if current and m[2]['t'] - current[-1][2]['t'] > 0.3:
            segments.append(current)
            current = []
        current.append(m)
    if current:
        segments.append(current)
    for seg in segments:
        if len(seg) < 10:
            continue
        steady = seg[len(seg) // 3:]
        r0 = seg[0][2]
        out.append(f'walk t {r0["t"]:.1f}..{seg[-1][2]["t"]:.1f} s, first person {int(r0["active"])}: moved along '
                   f'{med([m[0] for m in steady]):+7.1f} deg, '
                   f'{med([wrap(m[0] - m[2]["psi"]) for m in steady]):+6.1f} from psi, '
                   f'{med([wrap(m[0] - m[2]["heading"]) for m in steady]):+6.1f} from the camera heading, '
                   f'{med([wrap(m[0] - m[2]["game_own"]) for m in steady]):+6.1f} from the game-built heading, '
                   f'{med([wrap(m[0] - m[2]["yaw"] - math.pi) for m in steady]):+6.1f} from the facing; '
                   f'speed {sorted(m[1] for m in steady)[len(steady) // 2]:.2f} m/s')
    act = [r for r in rows if r['eye_ry'] > 0.5 and r['active'] > 0]
    if act:
        ys = sorted(r['eye_ry'] for r in act)
        zs = sorted(r['eye_rz'] for r in act)
        hy = sorted(r['wy'] - r['py'] for r in act)
        out.append(f'eye in model space (raw): height median {ys[len(ys) // 2]:.3f} m (min {ys[0]:.3f} max {ys[-1]:.3f}), '
                   f'forward median {-zs[len(zs) // 2]:.3f} m; camera above the feet: median {hy[len(hy) // 2]:.3f} m')
        ang = [wrap(math.atan2(r['wx'] - r['px'], r['wz'] - r['pz']) - r['yaw'] - math.pi) for r in act
               if math.hypot(r['wx'] - r['px'], r['wz'] - r['pz']) > 0.02]
        if ang:
            out.append(f'camera offset from the feet: {med(ang):+.1f} deg from the facing (0 = in front of the face)')
    return out


def analyze(out):
    import numpy as np
    from PIL import Image
    captures = [line.split() for line in (out / 'captures.txt').read_text().splitlines() if line.strip()]
    images = {c[0]: load_gray(out / 'dump' / c[1]) for c in captures}
    png = out / 'png'
    png.mkdir(exist_ok=True)
    for label, (rgb, _) in images.items():
        image = Image.fromarray(rgb.clip(0, 255).astype(np.uint8))
        image.resize((image.width // 2, image.height // 2)).save(png / f'{label}.png')
    describe = {s[0]: s[4] for steps in STEP_SETS.values() for s in steps}
    lines = ['VR first-person test: mean absolute difference (0..255) to "fp" and the picture shift',
             '(dx > 0: the scene moved right; full-size pixels, phase correlation)', '']
    ref = 'fp' if 'fp' in images else captures[0][0]
    base_rgb, base = images[ref]
    for label, (rgb, gray) in images.items():
        if label == ref:
            continue
        diff = float(np.abs(rgb - base_rgb).mean())
        dx, dy, peak = phase_shift(base, gray)
        # Where the picture changed (a hidden part): the bounding box of pixels that moved > 40.
        changed = np.abs(rgb - base_rgb).max(axis=2) > 40
        box = ''
        if changed.sum() > 50:
            ys, xs = np.nonzero(changed)
            box = f'  changed {int(changed.sum())} px in x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}'
            mask = Image.fromarray((changed * 255).astype(np.uint8))
            mask.resize((mask.width // 2, mask.height // 2)).save(png / f'{label}_diff.png')
        lines.append(f'{label:20s} diff {diff:6.2f}  shift dx {dx:+5d} dy {dy:+5d} (peak {peak:.3f}){box}  {describe.get(label, "")}')
    lines += ['', 'trace (trace.csv):'] + ['  ' + x for x in analyze_trace(read_trace(out / 'trace.csv'))]
    text = log_text(out / 'session.log')
    parts = [l for l in text.splitlines() if 'VR FP:   part' in l]
    if parts:
        lines += ['', 'ChrAsm parts (the first listing):'] + ['  ' + l.split('VR FP:', 1)[1].strip() for l in parts[:24]]
    for key in ('VR FP: skeleton', 'VR FP: first person on', 'VR FP: snap turn', 'Hook: bb_hook_vr_fp_cam', 'VR FP: models'):
        found = [l for l in text.splitlines() if key in l]
        lines.append(f'{key}: {found[0] if found else "NOT FOUND"}')
    report = '\n'.join(lines) + '\n'
    (out / 'report.txt').write_text(report)
    print(report)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=PORT / 'out' / 'vr_fp_test', help='results folder')
    parser.add_argument('--probe', help='bb-probe.exe to run (default: run.py\'s choice)')
    parser.add_argument('--game-dir', help='the game dump (BB_GAME_DIR)')
    parser.add_argument('--user', help='user folder to run on (default: a copy of user/ in --out)')
    parser.add_argument('--timeout', type=int, default=600, help='BB_EXIT_AFTER_SECONDS safety net')
    parser.add_argument('--title-wait', type=float, default=6.0, help='seconds at the title screen')
    parser.add_argument('--settle', type=float, default=2.0, help='seconds between a setting and its capture')
    parser.add_argument('--walk', type=float, default=2.5, help='seconds of walking before a walk capture')
    parser.add_argument('--analyze-only', action='store_true', help='compare the captures of an earlier run')
    parser.add_argument('--steps', choices=sorted(STEP_SETS), default='full', help='which steps to run')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not args.analyze_only:
        run_game(args, out)
    return 0 if analyze(out) else 1


if __name__ == '__main__':
    sys.exit(main())
