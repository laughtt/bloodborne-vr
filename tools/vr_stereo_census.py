#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the stereo census of a frame (BB_STEREO_CENSUS=1 with BB_CAPTURE_TRIGGER: one JSON line per
draw and dispatch with its shaders, targets, sampled images and bound buffers; vk_stereo_census.h,
docs/vr/findings/23).

    python tools/vr_stereo_census.py passes CENSUS.jsonl [--index shaders.json]
    python tools/vr_stereo_census.py camera CENSUS.jsonl
    python tools/vr_stereo_census.py camdep YAW0.jsonl YAW10.jsonl YAW0_LATER.jsonl [--index ...]

passes  the frame's passes in order: draws, targets, shaders (named with --index, written by
        tools/vr_shader_reflect.py --json), and which stage binds which 864-byte camera block.
camera  the 864-byte blocks (cbSceneParam: the main camera has far 3000, the shadow cascades
        their own) and the main camera block decoded by member.
camdep  bound buffers whose bytes change when only the camera turns (yaw 0 -> 10 degrees with
        the H1 fake head) but not over the same time with the camera still: the camera inputs of
        every shader, by buffer size and changed dwords.
"""
import argparse
import collections
import json
import struct
import sys

STAGES = ['fs', 'tcs', 'tes', 'vs', 'gs', 'cs']
SCENE = [('FC_FarClipInfo', 0, 4), ('FC_ScreenSize', 4, 4), ('FC_MatrixView', 8, 12),
         ('FC_MatrixInvViewProj', 20, 16), ('FC_MatrixInvProj', 36, 16), ('FC_MatrixProj', 52, 16),
         ('FC_DepthComputeParam', 68, 4), ('FC_ShadowMapParam', 72, 4), ('FC_ShadowColor', 76, 4),
         ('FC_ShadowDir', 80, 4), ('VC_ShadowMapMatrix', 84, 16), ('FC_Shadow* scalars', 100, 8),
         ('FC_ShadowViewKernelScale', 108, 4), ('FC_ProjSpaceToShadowMatrix', 112, 64),
         ('FC_CascadeSelectDist', 176, 4), ('FC_MatrixInvView', 180, 16), ('VC_ClipPlane', 196, 4),
         ('VC_MatrixViewProj', 200, 16)]


def load(path):
    recs = [json.loads(line) for line in open(path)]
    for r in recs:
        for x in r['res']:
            if x['t'] == 'buf':
                data = bytes.fromhex(x['data']) if x['data'] else b''
                x['bytes'] = data
                x['f'] = struct.unpack(f'<{len(data) // 4}f', data[:len(data) // 4 * 4])
    return [r for r in recs if r['t'] in ('draw', 'dispatch')]


def is_camera_block(x):
    return x['size'] == 864 and len(x['f']) >= 8 and x['f'][4] >= 64 and x['f'][5] >= 64


def is_main_camera(x):
    # far 3000; the value moves with the near plane (3001.49 in first person), so within 100
    return is_camera_block(x) and abs(x['f'][0] - 3000.0) <= 100.0


def targets(t):
    if not t:
        return '-'
    cs = ','.join(f'{c[1][:12]} {c[2]}x{c[3]}' if c else '-' for c in t['colors'])
    d = t['depth']
    return f'[{cs}] depth {d[1][:10] + " " + str(d[2]) + "x" + str(d[3]) if d else "-"}'


def name_of(index, h):
    e = index.get(h[-8:]) if index else None
    return e['file'].split('/')[-1] if e else h[-8:]


def passes(recs, index):
    blocks = {}
    out = []
    for r in recs:
        k = ('cs', r['cs']) if r['t'] == 'dispatch' else ('draw', json.dumps(r['targets'], sort_keys=True))
        if not out or out[-1][0] != k:
            out.append((k, []))
        out[-1][1].append(r)
    for _, rs in out:
        used = collections.Counter()
        for r in rs:
            for x in r['res']:
                if x['t'] == 'buf' and is_camera_block(x):
                    b = blocks.setdefault(x['bytes'], len(blocks))
                    used[f'{STAGES[x["stage"]]}:{"main" if is_main_camera(x) else "cam" + str(b)}'] += 1
        first = rs[0]
        if first['t'] == 'dispatch':
            print(f'#{first["i"]}-{rs[-1]["i"]} compute {name_of(index, first["cs"])} x{len(rs)} '
                  f'groups {first["groups"]}')
            continue
        shaders = []
        for r in rs:
            for h in (r['vs'], r['fs']):
                n = name_of(index, h)
                if int(h, 16) and n not in shaders:
                    shaders.append(n)
        print(f'#{first["i"]}-{rs[-1]["i"]} {len(rs)} draws {targets(first["targets"])}')
        print(f'    {", ".join(shaders[:4])}{" ..." if len(shaders) > 4 else ""}'
              f'{"  camera: " + ", ".join(f"{k} x{v}" for k, v in used.items()) if used else ""}')


def camera(recs):
    seen = {}
    for r in recs:
        for x in r['res']:
            if x['t'] == 'buf' and is_camera_block(x):
                seen.setdefault(x['bytes'], []).append(r['i'])
    for data, idx in seen.items():
        f = struct.unpack('<216f', data)
        print(f'block far {f[0]:.4g}, P00 {f[52]:.4g}, camera ({f[183]:.3f} {f[187]:.3f} {f[191]:.3f}): '
              f'{len(idx)} bindings, draws {min(idx)}..{max(idx)}')
    main = next((d for d in seen if abs(struct.unpack('<f', d[:4])[0] - 3000.0) <= 100.0), None)
    if main:
        f = struct.unpack('<216f', main)
        print('main camera (cbSceneParam):')
        for name, o, n in SCENE:
            rows = [f[o + i:o + i + 4] for i in range(0, n, 4)]
            print(f'  {name:28s} ' + ' | '.join(' '.join(f'{v:9.4g}' for v in row) for row in rows))


def keyed(recs):
    occ, out = collections.Counter(), {}
    for r in recs:
        k = (r.get('vs'), r.get('fs'), r.get('cs'), r.get('indices'), r.get('instances'),
             json.dumps(r.get('targets'), sort_keys=True))
        occ[k] += 1
        out[k + (occ[k],)] = r
    return out


def changed(a, b):
    if b is None or a['size'] != b['size'] or not a['bytes'] or not b['bytes']:
        return None
    n = min(len(a['bytes']), len(b['bytes'])) // 4
    return {i for i in range(n) if a['bytes'][4 * i:4 * i + 4] != b['bytes'][4 * i:4 * i + 4]}


def camdep(a, b, c, index):
    ka, kb, kc = keyed(a), keyed(b), keyed(c)
    stats = collections.defaultdict(lambda: [0, set(), set()])
    for k, ra in ka.items():
        rb, rc = kb.get(k), kc.get(k)
        if rb is None or rc is None:
            continue
        bufs_b = {(x['stage'], x['binding']): x for x in rb['res'] if x['t'] == 'buf'}
        bufs_c = {(x['stage'], x['binding']): x for x in rc['res'] if x['t'] == 'buf'}
        for x in ra['res']:
            if x['t'] != 'buf':
                continue
            cb, cc = changed(x, bufs_b.get((x['stage'], x['binding']))), changed(x, bufs_c.get((x['stage'], x['binding'])))
            if cb is None or cc is None:
                continue
            kind = 'cbSceneParam' if is_main_camera(x) else ('camera block' if is_camera_block(x) else str(x['size']))
            s = stats[(STAGES[x['stage']], name_of(index, x['hash']), kind)]
            s[0] += 1
            s[1] |= cb - cc
            s[2] |= cc
    for (stage, shader, kind), (n, cam, timed) in sorted(stats.items(), key=lambda kv: (-len(kv[1][1]), kv[0])):
        if cam:
            d = sorted(cam)
            print(f'{stage:3s} {shader[:48]:48s} {kind:13s} x{n:4d} camera dwords '
                  f'{d[:16]}{" +" + str(len(d) - 16) if len(d) > 16 else ""}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mode', choices=['passes', 'camera', 'camdep'])
    ap.add_argument('files', nargs='+')
    ap.add_argument('--index', help='shader index JSON (tools/vr_shader_reflect.py --json)')
    a = ap.parse_args()
    index = json.load(open(a.index)) if a.index else {}
    recs = [load(f) for f in a.files]
    if a.mode == 'passes':
        passes(recs[0], index)
    elif a.mode == 'camera':
        camera(recs[0])
    else:
        if len(recs) != 3:
            sys.exit('camdep needs three census files: yaw 0, yaw 10, yaw 0 later')
        camdep(*recs, index)


if __name__ == '__main__':
    main()
