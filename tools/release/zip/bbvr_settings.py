# SPDX-License-Identifier: GPL-2.0-or-later
"""Settings schema, profiles, launch builder, control-file writers and hardware detection for the
Bloodborne VR start-up menu (bbvr_menu.py) and launcher (bbvr_launcher.py). Standard library only
(no Tk), so it runs under the embeddable Python of the release zip.

Every option names where it goes (`target`):
    env    BB_* environment variable of the game process (read at start: "restart")
    hook   a hook of BB_HOOKS (start)            patch  a patch of BB_PATCHES (start)
    fp     key in out/fp.txt                     stereo key in out/stereo.txt
    iq     key in the image-quality control file (BB_XR_IQ_CONTROL, re-read within 250 ms)
    ui     path in ui_layouts.json "game" (hot-reloaded)
Control files are edited key by key: other tokens, order and spacing are kept, a .bak of the file
as it was before the first edit is written once. A value of None means "leave the game's own
default / the file as it is".
"""
import copy
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import sys

PUBLIC_BUILD = True  # tools/release/export_public.py rewrites this to True in the public tree
SCHEMA_VERSION = 1
# The start flow (dashboard "Play online" / "Play solo", a room code in the game) is part of every build, public too;
# only the dev options flagged online=True stay hidden there. The lobby address is never shipped: it is the player's
# online_server setting.
ONLINE_PLAY = True


def is_public():
    return PUBLIC_BUILD or os.environ.get('BB_PUBLIC_ALPHA') == '1'


TABS = [
    ('display', 'Display / VR'), ('dlss', 'DLSS / Upscaling'), ('comfort', 'Comfort'), ('combat', 'Combat'),
    ('body', 'Body / arms'), ('ui', 'UI'), ('patches', 'Patches'), ('advanced', 'Advanced'),
]

HOOKS_BASE = ['VR Camera Hook H1 (draw PersCam)', 'VR Camera Hook H2 (draw main context)', 'VR LOD Fix (SetCamera)',
              'VR FP Camera Heading (in-game step)', 'VR Stamina Cost (behavior)', 'VR Stamina Hits',
              'VR Arms IK (AnimationToModel)', 'VR UI Equip (HUD input)', 'VR Gun Aim (bullet spawn)',
              'VR Hitbox Scale (attack shape)', 'VR Attack Speed (chr dt)']
# (patch name in patches/Bloodborne.xml, option id, label, default, help); order = order in BB_PATCHES
PATCHES = [
    ('Disable Motion Blur (perf increase)', 'patch_blur', 'Disable motion blur', True, 'Recommended in VR.'),
    ('Disable AA', 'patch_aa', 'Disable the game\'s own AA', True, 'The VR path has its own AA / DLSS.'),
    ('Disable DoF', 'patch_dof', 'Disable depth of field', True, 'Recommended in VR.'),
    ('Disable Chromatic Aberration', 'patch_ca', 'Disable chromatic aberration', True, 'Recommended in VR.'),
    ('Model LOD 1 (Lower)', 'patch_lod1', 'Lower model detail (LOD 1)', False, 'Saves GPU time; objects pop in nearer.'),
    ('Model LOD 2 (Lowest)', 'patch_lod2', 'Lowest model detail (LOD 2)', False, 'More savings; do not combine with LOD 1.'),
    ('Disable Camera Auto Rotation via Movement', 'patch_autorot', 'No camera auto-rotation when walking', False,
     'Stops the game turning the camera by itself while you move.'),
    ('Sensitive Analog Input (easier to run)', 'patch_sens', 'Sensitive analog input', False, 'Running starts with less stick.'),
]


def _o(id, tab, label, kind, target, default, **kw):
    d = dict(id=id, tab=tab, label=label, kind=kind, target=target, default=default)
    d.update(kw)
    return d


def _build_schema():
    O = []
    a = O.append
    # ---- Display / VR ----
    a(_o('mode', 'display', 'Stereo mode', 'choice', ('env', 'BB_XR_MODE'), 'alt', choices=['alt', 'mono'],
         help='alt = alternate-eye stereo (true 3D); mono = a flat screen in the headset.'))
    a(_o('fps', 'display', 'Frame limit (fps)', 'int', ('env', 'BB_FPS_LIMIT'), 72, min=30, max=240,
         help='Match your headset refresh rate (72 for Quest).'))
    a(_o('stereo_scale', 'display', 'Stereo scale', 'float', ('stereo', 'scale'), 0.77, min=0.0, max=8.0, step=0.01,
         help='World scale in stereo; changes the apparent size of the world. Live.'))
    a(_o('ipd', 'display', 'IPD override (m)', 'float', ('stereo', 'ipd'), None, min=0.0, max=1.0, step=0.001,
         advanced=True, help='Blank = the headset\'s own eye distance.'))
    a(_o('stereo_invert', 'display', 'Swap eyes', 'bool', ('stereo', 'invert'), None, advanced=True,
         help='Debug: swaps left and right.'))
    a(_o('fp', 'display', 'First person', 'bool', ('fp', 'fp'), True, help='The camera is the hunter\'s eyes.'))
    a(_o('anchor', 'display', 'Camera anchor', 'choice', ('fp', 'anchor'), 'head',
         choices=['head', 'body', 'stable', 'raw'], help='head = follows the animated head; body/stable = steadier.'))
    a(_o('near', 'display', 'Near plane (m)', 'float', ('fp', 'near'), 0.08, min=0.0, max=1.0, step=0.01,
         help='Closer geometry is cut; raise it if the face/hood shows.'))
    a(_o('up', 'display', 'Camera height offset (m)', 'float', ('fp', 'up'), -0.08, min=-0.3, max=0.3, step=0.01))
    a(_o('forward', 'display', 'Camera forward offset (m)', 'float', ('fp', 'forward'), -0.10, min=-0.5, max=0.5, step=0.01))
    # ---- DLSS / Upscaling ----
    a(_o('upscale', 'dlss', 'Upscaler', 'choice', ('iq', 'upscale', 'BB_XR_UPSCALE'), 'fsr1',
         choices=['off', 'fsr1', 'dlss'], needs={'dlss': 'dlss'},
         help='How the eye images are scaled up. DLSS needs an NVIDIA RTX GPU and NVIDIA\'s nvngx_dlss.dll. Live once the game '
              'runs with a DLSS-capable executable.'))
    a(_o('dlss_quality', 'dlss', 'DLSS quality', 'choice', ('iq', 'dlss_quality', 'BB_XR_DLSS_QUALITY'), 'auto',
         choices=['auto', 'dlaa', 'quality', 'balanced', 'performance', 'ultra'], only_if={'upscale': 'dlss'},
         help='auto: the eye image is the Eye resolution below and the mode follows the ratio. Others: eye image = '
              '1080p x 1 / 1.5 / 1.72 / 2 / 3.'))
    a(_o('dlss_preset', 'dlss', 'DLSS preset', 'choice', ('iq', 'dlss_preset', 'BB_XR_DLSS_PRESET'), 'default',
         choices=['default', 'J', 'K', 'L', 'M'], only_if={'upscale': 'dlss'},
         help='default = the driver\'s choice; K is the transformer model.'))
    a(_o('dlss_sharpen', 'dlss', 'DLSS sharpen', 'float', ('iq', 'dlss_sharpen', 'BB_XR_DLSS_SHARPEN'), 0.0,
         min=0.0, max=1.0, step=0.05, only_if={'upscale': 'dlss'}, help='RCAS sharpening after DLSS, 0..1.'))
    a(_o('dlss_mask', 'dlss', 'DLSS arm/weapon mask', 'bool', ('iq', 'dlss_mask', 'BB_XR_DLSS_MASK'), True,
         only_if={'upscale': 'dlss'},
         help='The arms and weapon move with your hands but DLSS only knows camera motion, so they smear. On: surfaces '
              'nearer than the distance below are drawn from the current frame only (needs the matching bbport_dlss.dll).'))
    a(_o('dlss_mask_m', 'dlss', 'Arm mask distance (m)', 'float', ('iq', 'dlss_mask_m', 'BB_XR_DLSS_MASK_M'), 0.9,
         min=0.0, max=3.0, step=0.05, only_if={'upscale': 'dlss'},
         help='View distance of the masked surfaces from the camera. 0 turns the mask off. Raise it if the weapon tip still '
              'smears; lower it if nearby walls look noisy.'))
    a(_o('eye_res', 'dlss', 'Eye resolution', 'text', ('iq', 'res', 'BB_XR_EYE_RES'), None,
         suggestions=['1.5', '2', '3', '2880x1620', '3840x2160'],
         help='Size of each eye image: a factor of the 1080p frame (1.5, 2 = 3840x2160) or WxH. Blank = default.'))
    a(_o('jitter_sign', 'dlss', 'DLSS jitter sign', 'choice', ('env', 'BB_XR_DLSS_JITTER_SIGN'), '+1',
         choices=['+1', '-1'], advanced=True, only_if={'upscale': 'dlss'}, emit=lambda v: '-1' if v == '-1' else None,
         help='Debug: flips the sub-pixel jitter sign if DLSS shimmers. Needs a restart.'))
    a(_o('aa', 'dlss', 'FXAA on non-upscaled frames', 'choice', ('iq', 'aa', 'BB_XR_AA'), 'fxaa', choices=['fxaa', 'off'],
         help='Cheap anti-aliasing for FSR 1 and for menus under DLSS.'))
    a(_o('sharpen', 'dlss', 'FSR 1 sharpen', 'float', ('iq', 'sharpen', 'BB_XR_SHARPEN'), 0.5, min=0.0, max=1.0, step=0.05,
         help='RCAS sharpening of the FSR 1 upscale, 0..1.'))
    # ---- Comfort ----
    a(_o('turn', 'comfort', 'Turning', 'choice', ('fp', 'turn'), 'smooth', choices=['smooth', 'snap', 'off', 'game'],
         help='Right stick left/right. snap = fixed steps, smooth = continuous.'))
    a(_o('turn_speed', 'comfort', 'Smooth turn speed (deg/s)', 'float', ('fp', 'turn_speed'), 90, min=1, max=720, step=5))
    a(_o('snap', 'comfort', 'Snap angle (deg)', 'float', ('fp', 'snap'), None, min=1, max=180, step=5,
         help='Blank = the default step (30). Needs Turning = snap.'))
    a(_o('move', 'comfort', 'Walk direction', 'choice', ('fp', 'move'), 'head', choices=['head', 'body', 'controller'],
         help='head = where you look.'))
    a(_o('anim_lock', 'comfort', 'Camera animation lock', 'choice', ('fp', 'anim_lock'), '1', choices=['0', '1', '2'],
         labels={'0': '0 - camera follows every animation', '1': '1 - steady (no sway or flinch)',
                 '2': '2 - steady and eases lunges'},
         help='Reduces camera motion that is not yours (nausea).'))
    a(_o('anim_smooth', 'comfort', 'Lunge easing (s)', 'float', ('fp', 'anim_smooth'), 0.25, min=0.02, max=2.0, step=0.01,
         help='Used with lock 2.'))
    a(_o('smooth', 'comfort', 'Camera smoothing (s)', 'float', ('fp', 'smooth'), None, min=0.0, max=2.0, step=0.05))
    # ---- Combat ----
    a(_o('hit', 'combat', 'Hitboxes', 'choice', ('fp', 'hit'), 'big', choices=['big', 'original'],
         labels={'big': 'Big (default)', 'original': 'Original (as the game shipped)'},
         help='Your weapon hit shapes. Big: wider and a little longer, easier to land hits with motion controls. '
              'Original: exactly the game own shapes. Enemies are never changed. Live.'))
    a(_o('hit_scale', 'combat', 'Big hitboxes: radius scale', 'float', ('fp', 'hit_scale'), 1.5, min=0.25, max=4.0, step=0.05,
         advanced=True, only_if={'hit': 'big'}, help='Used when Hitboxes = Big.'))
    a(_o('hit_reach', 'combat', 'Big hitboxes: extra reach (m)', 'float', ('fp', 'hit_reach'), 1.0, min=-0.3, max=1.0, step=0.05,
         advanced=True, only_if={'hit': 'big'}, help='Used when Hitboxes = Big.'))
    a(_o('atk_speed', 'combat', 'Attack animation speed', 'float', ('fp', 'atk_speed'), 3.0, min=0.0, max=3.0, step=0.1,
         help='Your attacks play this many times faster so the body keeps up with your arm (0 / 1 = off).'))
    a(_o('face', 'combat', 'Body faces', 'choice', ('fp', 'face'), 'attack',
         choices=['off', 'view', 'head', 'controller', 'attack']))
    a(_o('face_dodge', 'combat', 'Body while dodging', 'choice', ('fp', 'face_dodge'), 'strafe',
         choices=['game', 'view', 'strafe']))
    a(_o('face_lockon', 'combat', 'Body follows view when locked on', 'bool', ('fp', 'face_lockon'), None))
    a(_o('stamina', 'combat', 'Stamina cost', 'choice', ('env', 'BB_VR_STAMINA'), 'defer', choices=['off', 'defer', 'refund'],
         help='defer / refund: stamina is charged by what your swing hits.'))
    a(_o('stamina_hits', 'combat', 'Stamina from hits', 'bool', ('hook', 'VR Stamina Hits'), True, advanced=True))
    a(_o('dash', 'combat', 'VR dodge (dash)', 'bool', ('env', 'BB_VR_DASH'), True))
    a(_o('gun', 'combat', 'Gun aiming', 'bool', ('env', 'BB_VR_GUN_AIM'), True))
    a(_o('hitscale_env', 'combat', 'Hit shape scaling (feature)', 'bool', ('env', 'BB_VR_HIT_SCALE'), True, advanced=True))
    # ---- Body / arms ----
    a(_o('hide', 'body', 'Hide parts of the body', 'choice', ('fp', 'hide'), 'body',
         choices=['none', 'head', 'body', 'hands', 'hands_skin', 'nohands', 'all'],
         help='body hides all but arms, hands and weapons. hands also hides the bare arms (armour sleeves and gloves stay). hands_skin also hides armour sleeves: bare hands only, gloves vanish. nohands also hides the bare hands (weapons only), for the custom VR hands (custom_hands in ui_layouts.json or BB_XR_CUSTOM_HANDS=1). hands/hands_skin/nohands untested in game.'))
    a(_o('arms', 'body', 'VR arms', 'bool', ('env', 'BB_VR_ARMS'), True, help='Arms follow the controllers (IK).'))
    a(_o('arm_attack', 'body', 'Arm weight while attacking', 'float', ('fp', 'arm_attack'), 1.0, min=0.0, max=1.0, step=0.05))
    a(_o('arm_weight', 'body', 'Arm weight (max)', 'float', ('fp', 'arm_weight'), None, min=0.0, max=1.0, step=0.05))
    a(_o('arm_offset', 'body', 'Hand offset (x,y,z,roll deg)', 'floats', ('fp', 'arm_offset'), '0,0.04,0.07,-105', n=(3, 4),
         help='Where the weapon sits in your hand.'))
    a(_o('arm_rot_l', 'body', 'Left hand rotation (deg)', 'floats', ('fp', 'arm_rot_l'), '30,25', n=(2, 2)))
    a(_o('arm_rot_r', 'body', 'Right hand rotation (deg)', 'floats', ('fp', 'arm_rot_r'), None, n=(2, 2)))
    a(_o('arm_body', 'body', 'Body turns with the arms', 'bool', ('fp', 'arm_body'), None))
    a(_o('arm_log', 'body', 'Log arm data', 'bool', ('fp', 'arm_log'), False, advanced=True))
    # ---- UI ----
    a(_o('ui', 'ui', 'VR menus and HUD', 'bool', ('env', 'BB_VR_UI'), True, help='Dashboard, quick wheel and wrist counters.'))
    a(_o('lock_on', 'ui', 'Right stick click locks on', 'bool', ('ui', 'lock_on'), False, live_ui=True))
    a(_o('r3_hold', 'ui', 'Hold time for the item wheel (s)', 'float', ('ui', 'r3_hold_s'), 0.5, min=0.15, max=3.0, step=0.05))
    a(_o('hud_head', 'ui', 'HUD follows your head', 'bool', ('ui', 'hud.head'), False))
    a(_o('hud_wrist', 'ui', 'Wrist HUD', 'bool', ('ui', 'hud.wrist'), True))
    a(_o('hud_distance', 'ui', 'HUD distance (m)', 'float', ('ui', 'hud.distance'), 0.9, min=0.2, max=5.0, step=0.05))
    a(_o('hud_down', 'ui', 'HUD angle below the view (deg)', 'float', ('ui', 'hud.down_deg'), 24, min=0, max=90, step=1))
    a(_o('hud_scale', 'ui', 'HUD size', 'float', ('ui', 'hud.scale'), 1.0, min=0.2, max=5.0, step=0.05))
    a(_o('wrist_scale', 'ui', 'Wrist HUD size', 'float', ('ui', 'hud.wrist_scale'), 0.7, min=0.2, max=5.0, step=0.05))
    # ---- Patches ----
    for name, oid, label, default, help_ in PATCHES:
        a(_o(oid, 'patches', label, 'bool', ('patch', name), default, help=help_))
    # ---- Advanced ----
    a(_o('exe', 'advanced', 'Game executable', 'text', ('launch', 'exe'), None, advanced=False,
         help='Blank = the one next to the launcher (bin\\bb-probe.exe, or out\\bb-probe.exe in a source tree).'))
    a(_o('runtime', 'advanced', 'OpenXR runtime', 'runtime', ('launch', 'runtime'), '',
         help='Blank = the system\'s active runtime. A choice only affects this game (XR_RUNTIME_JSON).'))
    a(_o('fp_log', 'advanced', 'First-person log', 'bool', ('env', 'BB_VR_FP_LOG'), False, advanced=True))
    a(_o('dash_log', 'advanced', 'Dash log', 'bool', ('env', 'BB_VR_DASH_LOG'), False, advanced=True))
    a(_o('stereo_log', 'advanced', 'Stereo log', 'bool', ('env', 'BB_VR_STEREO_LOG'), False, advanced=True))
    a(_o('stamina_log', 'advanced', 'Stamina log', 'bool', ('env', 'BB_VR_STAMINA_LOG'), False, advanced=True))
    a(_o('stamina_trace', 'advanced', 'Stamina trace', 'bool', ('env', 'BB_VR_STAMINA_TRACE'), False, advanced=True))
    a(_o('ui_probe', 'advanced', 'UI probe', 'bool', ('env', 'BB_VR_UI_PROBE'), False, advanced=True))
    a(_o('extra_env', 'advanced', 'Extra environment (KEY=VALUE per line, BB_ names only)', 'multiline',
         ('launch', 'extra_env'), '', advanced=True, help='Passed to the game as written. Only names starting with BB_ are used.'))
    a(_o('online', 'advanced', 'Private build option', 'bool', ('env', 'BB_ONLINE'), False, online=True,
         help='Not available in this build.'))
    a(_o('online_server', 'advanced', 'Lobby address', 'text', ('launch', 'online_server'), None, online=True,
         help='Base address of the room server (the CLI "online server" command sets the same value).'))
    a(_o('online_game_server', 'advanced', 'Game server address', 'text', ('launch', 'online_game_server'), None, online=True,
         help='host:port the game signs in to. Empty: taken from the lobby address.'))
    a(_o('online_autobell', 'advanced', 'Ring the bells automatically in a room', 'bool', ('env', 'BB_ONLINE_AUTOBELL'), True,
         online=True, help='In a room the game rings the Beckoning Bell (host) or the Small Resonant Bell (guest) by itself.'))
    a(_o('online_give_bells', 'advanced', 'Give the bells when missing', 'bool', ('env', 'BB_ONLINE_BELL_GIVE'), True,
         online=True, advanced=True, help='In a room, adds a missing Beckoning / Small Resonant Bell (key items, never used up) so a new character can co-op. Off: the game only logs that it is missing.'))
    return O


SCHEMA = _build_schema()
BY_ID = {o['id']: o for o in SCHEMA}


def visible_schema(public=None):
    public = is_public() if public is None else public
    return [o for o in SCHEMA if not (public and o.get('online'))]


def is_live(opt):
    return opt['target'][0] in ('fp', 'stereo', 'iq', 'ui')


# ---- profiles --------------------------------------------------------------------------------

PROFILES = {
    'Default': {},
    'Comfort': {'turn': 'snap', 'snap': 30, 'anim_lock': '2', 'anim_smooth': 0.35, 'move': 'head'},
    'Performance': {'eye_res': '1.5', 'patch_lod1': True, 'aa': 'fxaa', 'upscale': 'fsr1'},
    'Quality (DLSS)': {'upscale': 'dlss', 'dlss_quality': 'quality', 'dlss_preset': 'K', 'dlss_sharpen': 0.2,
                       'aa': 'fxaa'},
}
PROFILE_ORDER = ['Default', 'Comfort', 'Performance', 'Quality (DLSS)', 'Custom']
PROFILE_NOTES = {
    'Default': 'Balanced defaults: DLSS (quality, preset K) on an NVIDIA RTX GPU, otherwise FSR 1 upscaling; FXAA.',
    'Comfort': 'Snap turning, locked camera animation with lunge easing.',
    'Performance': 'FSR 1 at 1.5x the frame instead of 2x, lower model detail.',
    'Quality (DLSS)': 'DLSS Super Resolution, quality mode, preset K. Falls back to FSR 1 if DLSS is not available.',
    'Custom': 'Your own values; edit any option and this profile is used.',
}


def base_values():
    return {o['id']: o['default'] for o in SCHEMA}


def auto_defaults(caps=None):
    """Defaults that depend on the hardware: DLSS (quality, preset K, sharpen 0.2: the 'Quality (DLSS)' profile)
    when the DLSS check passes (NVIDIA RTX GPU, DLSS-capable exe, both DLLs present), else the schema's FSR 1.
    A profile or the saved Custom values are applied over it, so a choice is never overridden."""
    if ((caps or {}).get('dlss') or {}).get('ok'):
        return dict(PROFILES['Quality (DLSS)'])
    return {}


def effective_values(settings, caps=None):
    values = base_values()
    values.update(auto_defaults(caps))
    name = settings.get('profile', 'Default')
    if name == 'Custom':
        values.update({k: v for k, v in settings.get('custom', {}).items() if k in BY_ID})
    else:
        values.update(PROFILES.get(name, {}))
    return values


def coerce(opt, value):
    """Normalises a value from JSON / the page to the option's type; None / '' -> None (default)."""
    kind = opt['kind']
    if value is None or (isinstance(value, str) and value.strip() == '' and kind not in ('runtime', 'multiline')):
        return None
    if kind == 'bool':
        if isinstance(value, str):
            return value.strip().lower() in ('1', 'true', 'on', 'yes')
        return bool(value)
    if kind in ('int', 'float'):
        x = float(value)
        if 'min' in opt:
            x = max(opt['min'], x)
        if 'max' in opt:
            x = min(opt['max'], x)
        return int(round(x)) if kind == 'int' else x
    if kind == 'choice':
        s = str(value)
        return s if s in opt['choices'] else opt['default']
    if kind == 'floats':
        parts = [p for p in re.split(r'[,\s]+', str(value).strip()) if p]
        lo, hi = opt['n']
        try:
            nums = [float(p) for p in parts]
        except ValueError:
            return opt['default']
        return ','.join(fmt_num(x) for x in nums) if lo <= len(nums) <= hi else opt['default']
    return str(value)


def fmt_num(x):
    x = float(x)
    if x == int(x) and abs(x) < 1e9:
        return str(int(x))
    return ('%.6g' % x)


def to_text(opt, value):
    """The file / environment spelling of a value."""
    if value is None:
        return None
    if opt['id'] == 'hit':  # the runtime key is a flag: big = 1, original = 0 (no shape is touched)
        return '0' if value == 'original' else '1'
    if opt['kind'] == 'bool':
        return '1' if value else '0'
    if opt['kind'] in ('int', 'float'):
        if opt['id'] in ('hit_reach',):
            return '%.2f' % value
        if opt['id'] in ('atk_speed',):
            return '%.1f' % value
        return fmt_num(value)
    return str(value)


# ---- default settings file ------------------------------------------------------------------

def default_settings():
    return {'version': SCHEMA_VERSION, 'profile': 'Default', 'custom': {}, 'show_menu': True}


def load_settings(path):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        if not isinstance(data, dict):
            raise ValueError
    except (OSError, ValueError):
        return default_settings()
    s = default_settings()
    s.update({k: data[k] for k in ('profile', 'custom', 'show_menu') if k in data})
    if s['profile'] not in PROFILE_ORDER:
        s['profile'] = 'Default'
    if not isinstance(s['custom'], dict):
        s['custom'] = {}
    s['custom'] = {k: v for k, v in s['custom'].items() if k in BY_ID}
    return s


def save_settings(path, settings):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    out = {'version': SCHEMA_VERSION, 'profile': settings['profile'], 'custom': settings.get('custom', {}),
           'show_menu': bool(settings.get('show_menu', True))}
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def apply_edit(settings, values_from_page, profile=None, caps=None):
    """The page sent the full value set of what it shows. Same as the profile: keep the profile; any
    difference: the profile becomes Custom holding the values. Returns the new settings."""
    s = copy.deepcopy(settings)
    if profile:
        s['profile'] = profile if profile in PROFILE_ORDER else s['profile']
    if values_from_page is None:
        return s
    page = {}
    for oid, v in values_from_page.items():
        if oid in BY_ID:
            page[oid] = coerce(BY_ID[oid], v)
    reference = effective_values(s, caps)
    changed = {k: v for k, v in page.items() if reference.get(k) != v}
    if s['profile'] == 'Custom':
        merged = reference
        merged.update(page)
        s['custom'] = {k: merged[k] for k in merged if k in BY_ID}
    elif changed:
        merged = reference
        merged.update(page)
        s['profile'] = 'Custom'
        s['custom'] = {k: merged[k] for k in merged}
    return s


# ---- hardware / runtime detection ----------------------------------------------------------

def _winreg():
    try:
        import winreg
        return winreg
    except ImportError:
        return None


def detect_gpus():
    """[{'name': ..., 'rtx': bool}] from the display adapter class in the registry (no extra tools)."""
    wr = _winreg()
    gpus = []
    if wr:
        base = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
        try:
            with wr.OpenKey(wr.HKEY_LOCAL_MACHINE, base) as key:
                i = 0
                while True:
                    try:
                        sub = wr.EnumKey(key, i)
                    except OSError:
                        break
                    i += 1
                    if not re.fullmatch(r'\d{4}', sub):
                        continue
                    try:
                        with wr.OpenKey(key, sub) as k:
                            name = wr.QueryValueEx(k, 'DriverDesc')[0]
                    except OSError:
                        continue
                    if name and name not in [g['name'] for g in gpus]:
                        gpus.append({'name': name, 'rtx': bool(re.search(r'\bRTX\b|\bTITAN RTX\b', name, re.I))})
        except OSError:
            pass
    return gpus


_SCAN_CACHE = {}


def exe_supports_dlss(path):
    """True when the executable was built with the VR DLSS path (its string table has the key names)."""
    path = Path(path)
    try:
        stat = path.stat()
    except OSError:
        return False
    key = (str(path), stat.st_mtime_ns, stat.st_size)
    if key not in _SCAN_CACHE:
        found = False
        try:
            with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                found = m.find(b'dlss_sharpen') >= 0 and m.find(b'BB_XR_DLSS_QUALITY') >= 0
        except (OSError, ValueError):
            pass
        _SCAN_CACHE[key] = found
    return _SCAN_CACHE[key]


def newest_dev_build(port):
    """Source tree only: the newest out/bb-probe-all*.exe (the numbered builds of the VR work, what
    out/vr_all.sh runs), else out/bb-probe.exe."""
    builds = sorted((p for p in (port / 'out').glob('bb-probe-all*.exe') if re.fullmatch(r'bb-probe-all\d*\.exe', p.name)),
                    key=lambda p: p.stat().st_mtime)
    return builds[-1] if builds and (port / 'build.sh').is_file() else None


def main_exe(port, override=None):
    """The executable used without DLSS: the 'exe' option, BB_PROBE, bin/ (release) or out/ (source tree)."""
    for cand in (override, os.environ.get('BB_PROBE'), port / 'bin' / 'bb-probe.exe', newest_dev_build(port),
                 port / 'out' / 'bb-probe.exe'):
        if cand:
            p = Path(cand)
            p = p if p.is_absolute() else port / p
            if p.is_file():
                return p
    return None


def detect_dlss(port, exe_override=None, gpus=None):
    """{'ok', 'reason', 'exe', 'native', 'dir'}. ok = selecting DLSS can work; exe = the executable to start
    with it (the main one once it has the DLSS path and its DLLs, else the build in out/dlss-vr)."""
    gpus = detect_gpus() if gpus is None else gpus
    result = {'ok': False, 'reason': '', 'exe': None, 'native': False, 'dir': None}
    if not any(g['rtx'] for g in gpus):
        names = ', '.join(g['name'] for g in gpus) or 'none found'
        result['reason'] = f'DLSS needs an NVIDIA RTX GPU (found: {names}).'
        return result
    notes = []
    candidates = []
    main = main_exe(port, exe_override)
    if main:
        candidates.append((main, True))
    for other in (port / 'out' / 'bb-probe.exe', port / 'out' / 'dlss-vr' / 'bb-probe.exe'):
        if other.is_file() and all(other.resolve() != c[0].resolve() for c in candidates):
            candidates.append((other, False))
    if not candidates:
        result['reason'] = 'No game executable (bb-probe.exe) found.'
        return result
    for exe, native in candidates:
        folder = exe.parent
        if not exe_supports_dlss(exe):
            notes.append(f'{exe.name} in {folder.name} has no DLSS support (build the vr branch that includes dlss-vr)')
            continue
        missing = [n for n in ('bbport_dlss.dll', 'nvngx_dlss.dll') if not (folder / n).is_file()]
        if missing:
            hint = ' (NVIDIA\'s own DLL: copy it from the DLSS SDK, it is not redistributed)' if 'nvngx_dlss.dll' in missing else ''
            notes.append(f'{", ".join(missing)} not found in {folder}{hint}')
            continue
        return {'ok': True, 'reason': '', 'exe': exe, 'native': native, 'dir': folder}
    result['reason'] = 'DLSS files not found: ' + '; '.join(notes) + '.'
    return result


def _runtime_name(json_path):
    try:
        data = json.loads(Path(json_path).read_text(encoding='utf-8-sig', errors='replace'))
        return data.get('runtime', {}).get('name') or data.get('name') or Path(json_path).stem
    except (OSError, ValueError, AttributeError):
        return Path(json_path).stem


def classify_runtime(path, name=''):
    text = f'{path} {name}'.lower()
    if 'steam' in text:
        return 'steamvr'
    if 'pimax' in text:
        return 'pimax'
    if 'oculus' in text or 'meta' in text:
        return 'oculus'
    if 'windowsmixedreality' in text or 'mixed reality' in text or 'mixedreality' in text:
        return 'wmr'
    return 'other'


def detect_runtimes():
    """{'active': path or None, 'runtimes': [{'path','name','kind','active'}], 'warnings': [...]}"""
    seen = {}
    active = None
    wr = _winreg()
    if wr:
        for view in (wr.KEY_WOW64_64KEY, 0):
            try:
                with wr.OpenKey(wr.HKEY_LOCAL_MACHINE, r'SOFTWARE\Khronos\OpenXR\1', 0, wr.KEY_READ | view) as key:
                    if active is None:
                        try:
                            active = wr.QueryValueEx(key, 'ActiveRuntime')[0]
                        except OSError:
                            pass
                    try:
                        with wr.OpenKey(key, 'AvailableRuntimes') as sub:
                            i = 0
                            while True:
                                try:
                                    seen[wr.EnumValue(sub, i)[0]] = None
                                except OSError:
                                    break
                                i += 1
                    except OSError:
                        pass
            except OSError:
                continue
    # SteamVR's runtime manifest (also when SteamVR is not the active runtime) and Pimax Play.
    local = os.environ.get('LOCALAPPDATA')
    if local:
        try:
            paths = json.loads(Path(local, 'openvr', 'openvrpaths.vrpath').read_text(encoding='utf-8-sig'))
            for rt in paths.get('runtime', []):
                cand = Path(rt) / 'steamxr_win64.json'
                if cand.is_file():
                    seen[str(cand)] = None
        except (OSError, ValueError):
            pass
    for pf in filter(None, (os.environ.get('ProgramFiles'), os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramW6432'))):
        for cand in (Path(pf, 'Pimax', 'Runtime', 'PiRuntime.json'), Path(pf, 'Pimax', 'Runtime', 'PimaxXR.json'),
                     Path(pf, 'Steam', 'steamapps', 'common', 'SteamVR', 'steamxr_win64.json')):
            if cand.is_file():
                seen[str(cand)] = None
    if active:
        seen.setdefault(active, None)
    runtimes = []
    done = set()
    for path in seen:
        key = os.path.normcase(os.path.normpath(path))
        if key in done or not Path(path).is_file():
            continue
        done.add(key)
        name = _runtime_name(path)
        runtimes.append({'path': path, 'name': name, 'kind': classify_runtime(path, name),
                         'active': bool(active) and os.path.normcase(os.path.normpath(active)) == key})
    return {'active': active, 'runtimes': runtimes, 'warnings': []}


PIMAX_STEAMVR_WARNING = 'SteamVR on Pimax can show double images: use the Pimax OpenXR runtime for this game.'


def runtime_warning(runtime_info, chosen_path):
    """Warns when SteamVR will run on a Pimax setup."""
    kinds = {r['kind'] for r in runtime_info['runtimes']}
    chosen = chosen_path or runtime_info.get('active') or ''
    if 'pimax' in kinds and classify_runtime(chosen, '') == 'steamvr':
        return PIMAX_STEAMVR_WARNING
    return ''


def detect_all(port, exe_override=None):
    gpus = detect_gpus()
    return {'gpus': gpus, 'dlss': detect_dlss(port, exe_override, gpus), 'runtimes': detect_runtimes()}


# ---- launch builder ---------------------------------------------------------------------------

def parse_extra_env(text):
    out = {}
    for line in str(text or '').splitlines():
        m = re.fullmatch(r'\s*(BB_[A-Z0-9_]+)\s*=\s*(.*?)\s*', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def _flag(v):
    return '1' if v else '0'


def build_launch(values, paths, caps=None, public=None):
    """The launch plan for effective `values`.

    paths: dict(out=<out dir>, fp, stereo, iq, assets, game_dir, port)
    caps: detect_all() result (or None: no hardware known; DLSS is then taken as unavailable)
    Returns dict(env, exe, hooks, patches, files{fp,stereo,iq}, ui, warnings, upscale)."""
    public = is_public() if public is None else public
    v = dict(values)
    warnings = []
    if public and v.get('online'):
        v['online'] = False
    upscale = v.get('upscale') or 'fsr1'
    dlss = (caps or {}).get('dlss') or {'ok': False, 'reason': 'not checked'}
    exe = None
    port = Path(paths['port'])
    if upscale == 'dlss':
        if dlss['ok']:
            exe = dlss['exe']
        else:
            warnings.append('DLSS is not available (' + dlss['reason'] + ') - using FSR 1.')
            upscale = 'fsr1'
    if exe is None:
        exe = main_exe(port, v.get('exe'))
    env = {}
    e = env.__setitem__
    e('BB_GAME_DIR', str(paths['game_dir']))
    if exe:
        e('BB_PROBE', str(exe))
    e('BB_XR_AA', v['aa'] or 'fxaa')
    e('BB_XR_UPSCALE', upscale)
    e('BB_XR_SHARPEN', to_text(BY_ID['sharpen'], v['sharpen']) if v['sharpen'] is not None else '0.5')
    e('BB_VR', '1')
    e('BB_XR_MODE', v['mode'] or 'alt')
    e('BB_VR_HEAD', '1')
    e('BB_VR_HEAD_LEVEL', '1')
    e('BB_VR_FP', '1')
    e('BB_VR_FP_LOG', _flag(v['fp_log']))
    e('BB_VR_FP_CONTROL', str(paths['fp']))
    e('BB_VR_DASH', _flag(v['dash']))
    e('BB_VR_DASH_LOG', _flag(v['dash_log']))
    e('BB_VR_STEREO_CONTROL', str(paths['stereo']))
    e('BB_VR_STEREO_LOG', _flag(v['stereo_log']))
    e('BB_VR_STAMINA', v['stamina'] or 'defer')
    e('BB_VR_STAMINA_LOG', _flag(v.get('stamina_log')))
    e('BB_VR_STAMINA_TRACE', _flag(v['stamina_trace']))
    e('BB_VR_ARMS', _flag(v['arms']))
    e('BB_VR_UI', _flag(v['ui']))
    e('BB_VR_UI_PROBE', _flag(v['ui_probe']))
    e('BB_VR_GUN_AIM', _flag(v['gun']))
    e('BB_VR_HIT_SCALE', _flag(v['hitscale_env']))
    e('BB_UPSCALER', 'off')
    e('BB_FPS_LIMIT', str(v['fps']))
    patches = [name for name, oid, *_ in PATCHES if v.get(oid)]
    e('BB_PATCHES', ';'.join(patches))
    hooks = [h for h in HOOKS_BASE if h != 'VR Stamina Hits' or v.get('stamina_hits')]
    e('BB_HOOKS', ';'.join(hooks))
    e('BB_VR_ASSETS', str(paths['assets']))
    e('BB_XR_IQ_CONTROL', str(paths['iq']))
    if v.get('eye_res') is not None:
        e('BB_XR_EYE_RES', str(v['eye_res']))
    if upscale == 'dlss':
        e('BB_XR_DLSS_QUALITY', v['dlss_quality'] or 'auto')
        e('BB_XR_DLSS_PRESET', v['dlss_preset'] or 'default')
        e('BB_XR_DLSS_SHARPEN', fmt_num(v['dlss_sharpen'] or 0))
        e('BB_XR_DLSS_MASK', '0' if v.get('dlss_mask') is False else '1')
        e('BB_XR_DLSS_MASK_M', fmt_num(0.9 if v.get('dlss_mask_m') is None else v['dlss_mask_m']))
        if v.get('jitter_sign') == '-1':
            e('BB_XR_DLSS_JITTER_SIGN', '-1')
    if v.get('online') and not public:
        e('BB_ONLINE', '1')
    if v.get('runtime'):
        e('XR_RUNTIME_JSON', str(v['runtime']))
    if not public:
        env.update(parse_extra_env(v.get('extra_env')))
    # control files
    files = {'fp': {}, 'stereo': {}, 'iq': {}}
    ui = {}
    for o in SCHEMA:
        kind = o['target'][0]
        val = v.get(o['id'])
        if o['id'] == 'upscale':
            val = upscale
        if kind in ('fp', 'stereo'):
            if val is not None:
                files[kind][o['target'][1]] = to_text(o, val)
        elif kind == 'iq':
            key = o['target'][1]
            files['iq'][key] = None if val is None else to_text(o, val)
        elif kind == 'ui':
            if val is not None:
                ui[o['target'][1]] = val
    if upscale != 'dlss':  # the DLSS keys mean nothing then; keep them out of the file
        for k in ('dlss_quality', 'dlss_preset', 'dlss_sharpen', 'dlss_mask', 'dlss_mask_m'):
            files['iq'].pop(k, None)
    return {'env': env, 'exe': exe, 'hooks': hooks, 'patches': patches, 'files': files, 'ui': ui,
            'warnings': warnings, 'upscale': upscale}


# ---- control files (key-preserving) -----------------------------------------------------------

TOKEN = re.compile(r'\S+')


def _same_number(a, b):
    try:
        return float(a) == float(b)
    except (TypeError, ValueError):
        return False


def merge_control_text(text, updates):
    """Returns the text with `updates` ({key: value or None}) applied: the value of an existing key is
    replaced in place (all occurrences), None removes the token, new keys are appended; all other
    tokens, their order and the spacing stay."""
    remaining = dict(updates)
    done = set()

    def sub(m):
        tok = m.group(0)
        k, eq, _ = tok.partition('=')
        if eq and k in updates:
            done.add(k)
            if updates[k] is None:
                return '\0'
            if _same_number(tok.partition('=')[2], updates[k]):
                return tok  # 0.10 and 0.1: keep the user's spelling
            return f'{k}={updates[k]}'
        return tok

    new = TOKEN.sub(sub, text)
    if '\0' in new:
        new = re.sub(r'[ \t]*\0', '', new)
    additions = [f'{k}={v}' for k, v in remaining.items() if k not in done and v is not None]
    if additions:
        body = new.rstrip()
        tail = new[len(body):]
        new = (body + ' ' if body else '') + ' '.join(additions) + (tail or '\n')
    return new


def write_control(path, updates):
    """Applies `updates` to the control file; writes .bak once (the file as the menu first found it).
    Returns True when the file changed."""
    path = Path(path)
    old = path.read_text(encoding='utf-8') if path.is_file() else ''
    new = merge_control_text(old, updates)
    if new == old and path.is_file():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    bak = path.with_name(path.name + '.bak')
    if path.is_file() and not bak.exists():
        shutil.copyfile(path, bak)
    path.write_text(new, encoding='utf-8', newline='\n')
    return True


def read_control(path):
    out = {}
    try:
        for tok in Path(path).read_text(encoding='utf-8').split():
            k, eq, v = tok.partition('=')
            if eq:
                out[k] = v
    except OSError:
        pass
    return out


# ---- ui_layouts.json: sidecar overrides + merged effective copy ------------------------------

def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _set_path(d, dotted, value):
    parts = dotted.split('.')
    for p in parts[:-1]:
        d = d.setdefault(p, {})
    d[parts[-1]] = value


def _get_path(d, dotted):
    for p in dotted.split('.'):
        if not isinstance(d, dict) or p not in d:
            return None
        d = d[p]
    return d


def deep_merge(base, over):
    out = copy.deepcopy(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def apply_ui_overrides(assets_dir, ui_values):
    """Writes assets_dir/ui_layouts.json = base + the "game" overrides (ui_values: {dotted: value}).
    The untouched base is kept as ui_layouts.base.json and the overrides in ui_layouts.user.json; a
    hand edit of ui_layouts.json made since our last write becomes the new base (never clobbered).
    Returns True when ui_layouts.json was rewritten."""
    assets = Path(assets_dir)
    live = assets / 'ui_layouts.json'
    base_f = assets / 'ui_layouts.base.json'
    user_f = assets / 'ui_layouts.user.json'
    if not live.is_file():
        return False
    cur = live.read_bytes()
    meta = {}
    try:
        meta = json.loads(user_f.read_text(encoding='utf-8')) if user_f.is_file() else {}
    except ValueError:
        meta = {}
    if not base_f.is_file() or (meta.get('written_sha') and _sha(cur) != meta['written_sha']):
        base_f.write_bytes(cur)  # first run, or the file was edited by hand since we wrote it
    base = json.loads(base_f.read_text(encoding='utf-8'))
    game_base = base.get('game', {})
    over = {}
    for dotted, value in ui_values.items():
        if _get_path(game_base, dotted) != value:
            _set_path(over, dotted, value)
    if not over and not user_f.is_file():
        return False  # nothing to say: leave the user's file alone
    merged = copy.deepcopy(base)
    merged['game'] = deep_merge(game_base, over)
    text = json.dumps(merged, indent=2, ensure_ascii=False) + '\n'
    if not over:
        data = base_f.read_bytes()  # overrides gone: back to the pristine file
    else:
        data = text.encode('utf-8')
    changed = data != cur
    if changed:
        live.write_bytes(data)
    user_f.write_text(json.dumps({'game': over, 'written_sha': _sha(data)}, indent=2) + '\n', encoding='utf-8')
    return changed


def apply_files(plan, paths):
    """Writes the fp / stereo / iq control files and the UI layout for a launch plan. Returns the list of
    files changed. (The IQ file is the menu's own: its None keys are removed.)"""
    changed = []
    for key in ('fp', 'stereo', 'iq'):
        if plan['files'][key] and write_control(paths[key], plan['files'][key]):
            changed.append(str(paths[key]))
    if plan['ui'] or (Path(paths['assets']) / 'ui_layouts.user.json').is_file():
        if apply_ui_overrides(paths['assets'], plan['ui']):
            changed.append(str(Path(paths['assets']) / 'ui_layouts.json'))
    return changed


INSTALLED_FLAG = 'installed.flag'  # written by the Setup.exe next to BloodborneVR.exe; absent in the portable zip


def data_root(port, env=None):
    """Where the player's own files live (out/, user/ = saves + settings + crash reports + shader caches, logs/,
    bbvr_config.json). Portable zip and source tree: the port folder itself. Installed build (Setup.exe,
    installed.flag present): %LOCALAPPDATA%/BloodborneVR, so an uninstall / upgrade never touches them.
    BBVR_DATA_DIR overrides both."""
    env = os.environ if env is None else env
    if env.get('BBVR_DATA_DIR'):
        return Path(env['BBVR_DATA_DIR'])
    if (Path(port) / INSTALLED_FLAG).is_file():
        base = env.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
        return Path(base) / 'BloodborneVR'
    return Path(port)


def release_defaults(port):
    """defaults/release.json of the package (crash server, update-check address; written by make_release.py)."""
    try:
        data = json.loads((Path(port) / 'defaults' / 'release.json').read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _read_json(path):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def lobby_address(values, config, user_dir):
    """The lobby the player entered: the online_server setting, else the saved room's server, else the config's."""
    room = _read_json(Path(user_dir) / 'online_room.json')
    return str(values.get('online_server') or room.get('server') or (config or {}).get('online_server') or '').strip().rstrip('/')


def online_env(values, config, user_dir, public=None, solo=False):
    """Environment for a game started from a room (user/online_room.json + user/online_identity.json, both written
    by `bbvr_cli.py online ...`). {} for "Play solo" (solo=True), when there is no room and the dev `online` option is
    off, or when ONLINE_PLAY is off. In a public build only a room switches online on (the dev option is hidden)."""
    public = is_public() if public is None else public
    if solo or (public and not ONLINE_PLAY):
        return {}
    room = _read_json(Path(user_dir) / 'online_room.json')
    ident = _read_json(Path(user_dir) / 'online_identity.json')
    in_room = bool(room.get('code') and room.get('role') in ('host', 'guest') and not room.get('closed'))
    if not (in_room or (values.get('online') and not public)):
        return {}
    env = {'BB_ONLINE': '1'}
    if ident.get('npid'):
        env['BB_SHADNET_NPID'] = str(ident['npid'])
        env['BB_USER_NAME'] = str(ident.get('name') or ident['npid'])
        if ident.get('password'):
            env['BB_SHADNET_PASSWORD'] = str(ident['password'])
    lobby = lobby_address(values, config, user_dir)
    game = str(values.get('online_game_server') or '').strip()
    if not game and lobby:
        # the lobby shares its host with the game server; the game port is two below the lobby's (online_hosts.online_webapi)
        host, _, port = lobby.split('://', 1)[-1].partition('/')[0].rpartition(':')
        if host and port.isdigit() and int(port) > 2:
            game = f'{host}:{int(port) - 2}'
    if game:
        env['BB_ONLINE_SERVER'] = game
    if in_room:
        env['BB_ONLINE_ROLE'] = room['role']
        env['BB_ONLINE_ROOM'] = str(room['code'])
        env['BB_ONLINE_ROOM_FILE'] = str(Path(user_dir) / 'online_room.json')
        env['BB_ONLINE_STATUS_FILE'] = str(Path(user_dir) / 'online_bell_status.json')
        env['BB_ONLINE_AUTOBELL'] = '1' if values.get('online_autobell') is not False else '0'
        if values.get('online_give_bells') is not False:
            env['BB_ONLINE_BELL_GIVE'] = '1'  # a new character without the bells can still co-op
        if values.get('online_free_bells') is not False:
            env['BB_ONLINE_FREE_BELLS'] = '1'  # the co-op bells cost no Insight (src/vr_free_bells.h)
    return env


def make_paths(port, game_dir, data=None):
    data = Path(data) if data else Path(port)
    out = data / 'out'
    return {'port': Path(port), 'data': data, 'out': out, 'fp': out / 'fp.txt', 'stereo': out / 'stereo.txt',
            'iq': out / 'iq.txt', 'assets': out / 'vr_assets', 'game_dir': game_dir,
            'settings': data / 'user' / 'bbvr_settings.json'}


# ---- page data ----------------------------------------------------------------------------------

DLSS_NOTE = ('Measured cost: DLSS at 3840x2160 per eye ran about 57 fps against the 72 fps cap on an RTX 4090. '
             'Quality or Performance mode (lower eye resolution) should reach 72.')


def page_state(settings, caps, public=None):
    """JSON for the page. Online options are left out in a public build."""
    public = is_public() if public is None else public
    opts = []
    for o in visible_schema(public):
        d = {k: v for k, v in o.items() if k not in ('emit', 'target')}
        d['target'] = o['target'][0]
        d['live'] = is_live(o)
        opts.append(d)
    values = {k: v for k, v in effective_values(settings, caps).items() if not (public and BY_ID[k].get('online'))}
    runtimes = caps['runtimes']
    return {
        'public': public, 'online_play': ONLINE_PLAY, 'tabs': [{'id': t, 'label': l} for t, l in TABS], 'options': opts, 'values': values,
        'profile': settings['profile'], 'profiles': PROFILE_ORDER, 'profile_notes': PROFILE_NOTES,
        'show_menu': settings.get('show_menu', True),
        'caps': {'gpus': caps['gpus'],
                 'dlss': {k: (str(v) if isinstance(v, Path) else v) for k, v in caps['dlss'].items()},
                 'runtimes': runtimes['runtimes'], 'active_runtime': runtimes['active']},
        'dlss_note': DLSS_NOTE, 'runtime_warning_text': PIMAX_STEAMVR_WARNING,
    }
