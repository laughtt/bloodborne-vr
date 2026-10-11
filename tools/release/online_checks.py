#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Release checks for a public build that ships the online CLIENT (docs/vr/public-online-build.md). Standard library only.

    python tools/release/online_checks.py exe  out/release-test/bb-probe.exe [--variant online|offline]
    python tools/release/online_checks.py tree <unpacked release folder>

What may be in a public package: the online client (shadNet login, NP / Matching2 / signaling, HTTP and SSL
libraries, the room bells) and nothing of the server or the maintainer's setup. The checks (exe = the game host exe
and our other binaries; tree = the files of the package):

REJECT (any hit fails the release)
  server code        names of the room server, its helpers and its private checkout paths, an online/ folder or a
                     server-looking file in the package, server-only libraries (SERVER_CODE below)
  server addresses   host names of the community / game servers (SERVER_HOSTS), any IPv4 literal that is not a generic
                     loopback / broadcast / multicast / netmask value (IP_OK), any host:port or ip:port, the known
                     server ports as numbers (SERVER_PORTS)
  test accounts      the dev test account names and default passwords (TEST_ACCOUNTS), BB_SHADNET_PASSWORD=<value>
  dev tools          (public variant) BB_DEBUG_GIVE, BB_DEBUG_USE, BB_NET_TRACE, BB_NET_BREAK, the speffect use-file
REQUIRE (exe, variant online)
  online client      shadNet client, Matching2, HTTP libraries, the server setting variable
  room bells         BB_ONLINE_AUTOBELL / ROLE / FREE_BELLS / BELL_GIVE
  maintainer path    (public builds) the builder's source path compiled into the exe; gpu/CMakeLists.txt adds -ffile-prefix-map

Variant offline (BBPORT_ONLINE=OFF): in place of REQUIRE, the exe must hold none of the online client.
"""
import argparse
from pathlib import Path
import re
import sys

# The needles are split with + where a whole word would put this file itself on a deny list.
TEST_ACCOUNTS = re.compile(rb'(?i)hunter' + b'(?:one|two|three)\\b|change' + b'me')
PASSWORD_VALUE = re.compile(rb'BB_SHADNET_PASSWORD=[^\s%{]')
SERVER_HOSTS = re.compile(rb'(?i)thehunters' + b'dream|scej-' + b'network|shadps4\\.' + b'net')
SERVER_CODE = re.compile(
    rb'(?i)shadnet-' + b'server|bb-' + b'lobby|bb-' + b'server\\b|bb-coop-' + b'local|coop' + b'd\\.py|'
    rb'[A-Za-z]:[\\/]+game[\\/]+(?:shadnet|server)|[\\/]online[\\/](?:bb-|server|bots|coop)|'
    rb'\bargon2\b|\bsqlx\b|\btokio::|\baxum\b|\brusqlite\b')
DEV_TOOLS = re.compile(rb'BB_DEBUG_(?:GIVE|USE)|BB_NET_TRACE|BB_NET_BREAK|\bspeffect:')
IPV4 = re.compile(rb'(?<![\d.])((?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3})(?![\d.])(:\d{2,5})?')
# Generic values every network client has: not the address of a server.
IP_OK = {'0.0.0.0', '127.0.0.1', '255.255.255.255', '255.255.255.0', '255.255.0.0', '255.0.0.0', '224.0.0.1',
         '239.255.255.250', '223.255.255.255', '1.0.0.0', '1.1.1.1', '8.8.8.8', '8.8.4.4', '169.254.0.0'}
HOST_PORT = re.compile(rb'(?<![\w.-])((?:[A-Za-z0-9-]+\.)+(?:com|net|org|io|jp|de|eu|tv|gg|xyz|app|dev|me))(:\d{2,5})(?!\d)')
SERVER_PORTS = re.compile(rb'(?<![\d.])(?:3131[345]|20443|18671)(?![\d])')
MAINTAINER_PATH = re.compile(rb'(?i)[A-Za-z]:[\\/]+(?:bloodborne_vr|Users[\\/]+User)\b')

# Strings that exist only when the corresponding code is built in.
ONLINE_CLIENT = [(b'shadnet', 'shadNet client'), (b'sceNpMatching2', 'Matching2 (rooms, signaling)'),
                 (b'sceHttp', 'HTTP libraries'), (b'BB_SHADNET_SERVER', 'server setting variable')]
ROOM_BELLS = [(b'BB_ONLINE_AUTOBELL', 'auto-bell'), (b'BB_ONLINE_ROLE', 'room role'),
              (b'BB_ONLINE_FREE_BELLS', 'free bells'), (b'BB_ONLINE_BELL_GIVE', 'bell give')]
OFFLINE_MARKERS = [b'shad' + b'net', b'thehunters' + b'dream', b'scej-' + b'network', b'summon_' + b'messenger',
                   b'bb_' + b'online', b'bb_net_' + b'trace', b'mini' + b'upnp', b'sceNpMatching2']


def _hits(regex, data, limit=3):
    """Distinct matches of `regex` in `data` (text, at most `limit`)."""
    seen = []
    for m in regex.finditer(data):
        text = m.group(0)[:60].decode('latin1')
        if text not in seen:
            seen.append(text)
            if len(seen) >= limit:
                break
    return seen


def forbidden(data, public=True, bare_ips=True):
    """[(check, [examples])] for the REJECT list on a blob (binary or text). bare_ips=False: only ip:port counts
    (the dashboard and DLLs carry version numbers that look like addresses)."""
    found = []
    for name, regex in (('test account', TEST_ACCOUNTS), ('password value', PASSWORD_VALUE),
                        ('server host name', SERVER_HOSTS), ('server code / private path', SERVER_CODE),
                        ('host:port', HOST_PORT), ('server port number', SERVER_PORTS)):
        hits = _hits(regex, data)
        if hits:
            found.append((name, hits))
    ips = []
    for m in IPV4.finditer(data):
        text = m.group(1).decode()
        if m.group(2):  # ip:port is an address, loopback or not
            text += m.group(2).decode()
        elif text in IP_OK or not bare_ips:
            continue
        if text not in ips:
            ips.append(text)
        if len(ips) >= 3:
            break
    if ips:
        found.append(('hard-coded IP address', ips))
    if public:
        hits = _hits(DEV_TOOLS, data)
        if hits:
            found.append(('dev tool', hits))
    return found


def check_exe(data, variant='online', public=True):
    """(errors, warnings, passes) for the bytes of a game host exe / our own binary. Errors are strings."""
    errors, warnings, passes = [], [], []
    for name, examples in forbidden(data, public):
        errors.append(f'{name}: {", ".join(examples)}')
    if not errors:
        passes.append('no server code, server address, test account or dev tool in it')
    if variant == 'online':
        missing = [label for needle, label in ONLINE_CLIENT if needle not in data]
        (errors.append(f'online client missing: {", ".join(missing)} (build with -DBBPORT_ONLINE=ON -DBBPORT_PUBLIC=ON)')
         if missing else passes.append('online client present (' + ', '.join(l for _, l in ONLINE_CLIENT) + ')'))
        missing = [label for needle, label in ROOM_BELLS if needle not in data]
        (errors.append(f'room bells missing: {", ".join(missing)}') if missing
         else passes.append('room bells present (' + ', '.join(l for _, l in ROOM_BELLS) + ')'))
    else:
        low = data.lower()
        leaked = [m.decode() for m in OFFLINE_MARKERS if m.lower() in low]
        (errors.append(f'offline variant holds online client strings: {leaked}') if leaked
         else passes.append('offline variant: no online client in it'))
    hits = _hits(MAINTAINER_PATH, data, 1)
    if hits:
        (errors if public else warnings).append(f'maintainer path compiled in ({hits[0]}); build with the public configuration (-ffile-prefix-map)')
    return errors, warnings, passes


TEXT_SUFFIXES = {'.py', '.json', '.txt', '.md', '.bat', '.cfg', '.ini', '.xml', '.html', '.js', '.css', '.iss', '.cmd', '.yml', '.yaml'}
OWN_BINARIES = {'bb-probe.exe', 'bbport_xr.dll', 'bbport_dlss.dll', 'bb-gpu-capabilities.exe', 'BloodborneVR.exe'}
SKIP_DIRS = {'python', 'licenses', 'patches', '__pycache__'}
SERVER_FILES = re.compile(r'(?i)(?:^|/)(?:online|shadnet-' + r'server|bb-coop-' + r'local|coopd[^/]*|coop_server[^/]*)(?:/|$|\.py$|\.sh$)')


def check_tree(root, variant='online', public=True):
    """(errors, warnings, passes) for an unpacked package folder: every text file, our own binaries, file names."""
    root = Path(root)
    errors, warnings, passes = [], [], []
    scanned = 0
    for path in sorted(root.rglob('*')):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if SERVER_FILES.search(rel):
            errors.append(f'server file in the package: {rel}')
            continue
        top = rel.split('/')[0]
        if top in SKIP_DIRS:
            continue
        if path.name in OWN_BINARIES:
            data = path.read_bytes()
            if path.name == 'bb-probe.exe':
                e, w, _ = check_exe(data, variant, public)
                errors += [f'{rel}: {x}' for x in e]
                warnings += [f'{rel}: {x}' for x in w]
            else:
                errors += [f'{rel}: {n}: {", ".join(x)}' for n, x in forbidden(data, public, bare_ips=False)]
                if public and _hits(MAINTAINER_PATH, data, 1):
                    errors.append(f'{rel}: maintainer path compiled in; build with the public configuration (-ffile-prefix-map)')
            scanned += 1
        elif path.suffix.lower() in TEXT_SUFFIXES:
            data = path.read_bytes()
            scanned += 1
            # SOURCES-THIRD-PARTY.txt lists package versions (a.b.c.d looks like an address): only ip:port counts there
            bare = path.name != 'SOURCES-THIRD-PARTY.txt'
            for name, examples in forbidden(data, public=False, bare_ips=bare):
                if name == 'hard-coded IP address' and path.suffix == '.md':
                    continue
                errors.append(f'{rel}: {name}: {", ".join(examples)}')
    if not any(e.startswith(('server file',)) for e in errors):
        passes.append('no server files or online/ folder in the package')
    passes.append(f'{scanned} text files and own binaries scanned')
    return errors, warnings, passes


def report(title, errors, warnings, passes, out=print):
    for text in passes:
        out(f'PASS  {title}: {text}')
    for text in warnings:
        out(f'warn  {title}: {text}')
    for text in errors:
        out(f'FAIL  {title}: {text}')
    return not errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('kind', choices=('exe', 'tree'))
    ap.add_argument('path', type=Path)
    ap.add_argument('--variant', choices=('online', 'offline'), default='online')
    ap.add_argument('--private', action='store_true', help='a private dev build: dev tools are allowed')
    args = ap.parse_args(argv)
    if args.kind == 'exe':
        result = check_exe(args.path.read_bytes(), args.variant, not args.private)
    else:
        result = check_tree(args.path, args.variant, not args.private)
    return 0 if report(args.kind, *result) else 1


if __name__ == '__main__':
    sys.exit(main())
