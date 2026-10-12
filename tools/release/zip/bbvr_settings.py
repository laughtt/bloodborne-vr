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
import struct
import sys

PUBLIC_BUILD = True  # tools/release/export_public.py rewrites this to True in the public tree
SCHEMA_VERSION = 2
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


# Display size presets: (id, label, per-eye size the headset software recommends at its 100% / 1.0x, panel per eye,
# confidence of the recommended size). Sources and caveats: docs/vr/findings/41-display-size.md.
HEADSETS = [
    ('quest1', 'Meta Quest 1', (1440, 1600), (1440, 1600), 'low'),
    ('quest2', 'Meta Quest 2 / Quest 3S', (1832, 1920), (1832, 1920), 'medium'),
    ('quest3', 'Meta Quest 3', (2064, 2208), (2064, 2208), 'medium'),
    ('questpro', 'Meta Quest Pro', (1800, 1920), (1800, 1920), 'medium'),
    ('frame', 'Steam Frame', (2160, 2160), (2160, 2160), 'low'),
    ('index', 'Valve Index', (2016, 2240), (1440, 1600), 'high'),
    ('vive', 'HTC Vive', (1512, 1680), (1080, 1200), 'high'),
    ('vivepro', 'HTC Vive Pro', (2016, 2240), (1440, 1600), 'high'),
    ('rifts', 'Oculus Rift S', (1648, 1776), (1280, 1440), 'medium'),
    ('reverb', 'HP Reverb G2 (WMR)', (2160, 2160), (2160, 2160), 'medium'),
    ('crystal', 'Pimax Crystal', (2880, 2880), (2880, 2880), 'low'),
    ('dreamair', 'Pimax Dream Air', (3832, 3076), (3840, 3552), 'high'),
    ('aero', 'Varjo Aero', (2880, 2720), (2880, 2720), 'low'),
    ('beyond', 'Bigscreen Beyond', (2560, 2560), (2560, 2560), 'low'),
]
HEADSET_BY_ID = {h[0]: h for h in HEADSETS}
DISPLAY_SCALES = ('50', '70', '85', '100', '125', '150')


def _o(id, tab, label, kind, target, default, **kw):
    d = dict(id=id, tab=tab, label=label, kind=kind, target=target, default=default)
    d.update(kw)
    return d


def _build_schema():
    O = []
    a = O.append
    # ---- Display / VR ----
    a(_o('mode', 'display', 'VR render mode', 'choice', ('env', 'BB_XR_MODE'), 'stereo', choices=['stereo', 'mono', 'alt'],
         adv_choices=['alt'],
         labels={'stereo': 'Stereo (true 3D, both eyes rendered)', 'mono': 'Mono (one flat screen in the headset)',
                 'alt': 'Alternate-eye (older 3D method)'},
         help='Stereo renders both eyes for real. Mono is the fallback: use if you get eye strain or performance problems. '
              'Alternate-eye is the older method, kept for testing.'))
    a(_o('fps', 'display', 'Frame limit (fps)', 'int', ('env', 'BB_FPS_LIMIT'), 72, min=30, max=240,
         help='Match your headset refresh rate (72 for Quest).'))
    a(_o('fps_counter', 'display', 'Frames per second counter', 'bool', ('iq', 'fps', 'BB_XR_FPS'), False,
         help='A small fps readout in the headset. Live.'))
    a(_o('fps_pos', 'display', 'Counter corner', 'choice', ('iq', 'fps_pos', 'BB_XR_FPS_POS'), 'tr',
         choices=['tl', 'tr', 'bl', 'br'], only_if={'fps_counter': True},
         labels={'tl': 'Top left', 'tr': 'Top right', 'bl': 'Bottom left', 'br': 'Bottom right'}))
    a(_o('flat_fps', 'display', 'Frame cap in Flat mode', 'choice', ('flat', 'BB_FPS_LIMIT'), None,
         choices=['30', '60', '90', '120', '144', '165', '240', '0'], labels={'0': 'No limit'},
         help='Flat mode only (the VR frame limit above is for the headset). Blank = the display refresh.'))
    a(_o('stereo_scale', 'display', 'Stereo scale', 'float', ('stereo', 'scale'), 0.77, min=0.0, max=8.0, step=0.01,
         help='World scale in stereo; changes the apparent size of the world. Live.'))
    a(_o('ipd', 'display', 'IPD override (m)', 'float', ('stereo', 'ipd'), None, min=0.0, max=1.0, step=0.001,
         advanced=True, help='Blank = the headset\'s own eye distance.'))
    a(_o('stereo_invert', 'display', 'Swap eyes', 'bool', ('stereo', 'invert'), None, advanced=True,
         help='Debug: swaps left and right.'))
    # Display size (docs/vr/findings/41-display-size.md): sets the game's render size and the eye image size for a
    # headset before the game starts. display_size / display_custom are "sticky": kept when the profile changes.
    a(_o('display_size', 'display', 'Display size', 'choice', ('launch', 'display_size'), 'auto',
         choices=['auto', 'game'] + [h[0] for h in HEADSETS] + ['custom'],
         labels=dict({'auto': 'Auto (match my headset)', 'game': 'Game default (1080p frame)',
                      'custom': 'Custom size...'}, **{h[0]: h[1] for h in HEADSETS}),
         sticky=True,
         help='Sizes the game for your headset. Auto uses the size your headset software asked for last time (or a '
              'typical one for it), so its own resolution slider (Meta app, SteamVR, Virtual Desktop) counts; a named '
              'headset uses that headset\'s standard size and ignores the slider. Game default: 1080p frame, Eye '
              'resolution below.'))
    a(_o('display_scale', 'display', 'Display resolution', 'choice', ('launch', 'display_scale'), '100',
         choices=list(DISPLAY_SCALES), labels={'50': '50% (fastest)', '70': '70% (faster)', '85': '85%',
                                               '100': '100% (headset size)', '125': '125% (sharper)',
                                               '150': '150% (strong GPU)'},
         help='Percent of the headset size (per axis), on top of the Display size. Lower = more fps.'))
    a(_o('display_custom', 'display', 'Custom headset size (per eye)', 'text', ('launch', 'display_custom'), None,
         sticky=True, only_if={'display_size': 'custom'}, suggestions=['1832x1920', '2064x2208', '2016x2240'],
         help='WxH per eye, as your headset software reports it (game log: "VR: eye images: recommended WxH").'))
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
         help='Size of each eye image: a factor of the 1080p frame (1.5, 2 = 3840x2160) or WxH. Blank = default. '
              'Used only with Display size = Game default (other Display sizes set it).'))
    a(_o('jitter_sign', 'dlss', 'DLSS jitter sign', 'choice', ('env', 'BB_XR_DLSS_JITTER_SIGN'), '+1',
         choices=['+1', '-1'], advanced=True, only_if={'upscale': 'dlss'}, emit=lambda v: '-1' if v == '-1' else None,
         help='Debug: flips the sub-pixel jitter sign if DLSS shimmers. Needs a restart.'))
    a(_o('aa', 'dlss', 'FXAA on non-upscaled frames', 'choice', ('iq', 'aa', 'BB_XR_AA'), 'fxaa', choices=['fxaa', 'off'],
         help='Cheap anti-aliasing for FSR 1 and for menus under DLSS.'))
    a(_o('sharpen', 'dlss', 'FSR 1 sharpen', 'float', ('iq', 'sharpen', 'BB_XR_SHARPEN'), 0.5, min=0.0, max=1.0, step=0.05,
         help='RCAS sharpening of the FSR 1 upscale, 0..1.'))
    nr = {'dlssnr': True}
    a(_o('dlssnr', 'dlss', 'DLSS-NR neural rendering (experimental)', 'bool', ('iq', 'dlssnr', 'BB_XR_DLSSNR'), False,
         help='NVIDIA DLSS-NR neural rendering on each eye image. Experimental. Needs an RTX GPU and the model file '
              'nvngx_dlssnr.dll, which is NOT included: use your own copy and press "Find file..." to put it next to '
              'bb-probe.exe. Costs about 5 ms per eye after upscaling (placement post) at the default eye size on an RTX 4090.'))
    a(_o('dlssnr_place', 'dlss', 'DLSS-NR placement', 'choice', ('iq', 'dlssnr_place', 'BB_XR_DLSSNR_PLACE'), 'post',
         choices=['post', 'pre'], only_if=nr,
         labels={'post': 'post (on the finished eye image)', 'pre': 'pre (before the upscaler, on the 1080p image)'},
         help='Where DLSS-NR runs. pre: on the render-size image before DLSS / FSR 1 (cheaper, the Super High preset); '
              'post: on the finished eye image.'))
    a(_o('dlssnr_passes', 'dlss', 'DLSS-NR passes', 'int', ('iq', 'dlssnr_passes', 'BB_XR_DLSSNR_PASSES'), 1,
         min=1, max=3, step=1, only_if=nr,
         help='Run it again on the result. Each extra pass costs time and makes the look stronger.'))
    a(_o('dlssnr_style', 'dlss', 'DLSS-NR style', 'choice', ('iq', 'dlssnr_style', 'BB_XR_DLSSNR_STYLE'), '0',
         choices=['0', '1', '2'], advanced=True, only_if=nr, help='The look preset of the DLL, 0..2.'))
    a(_o('dlssnr_intensity', 'dlss', 'DLSS-NR intensity', 'float', ('iq', 'dlssnr_intensity', 'BB_XR_DLSSNR_INTENSITY'), 2.0,
         min=0.0, max=2.0, step=0.05, only_if=nr,
         help='Strength. NVIDIA\'s own GUI stops at 1; above 1 is experimental.'))
    a(_o('dlssnr_tone', 'dlss', 'DLSS-NR tone', 'float', ('iq', 'dlssnr_tone', 'BB_XR_DLSSNR_TONE'), 0.6,
         min=0.0, max=1.0, step=0.05, advanced=True, only_if=nr, help='Local tone strength.'))
    a(_o('dlssnr_structure', 'dlss', 'DLSS-NR structure', 'float', ('iq', 'dlssnr_structure', 'BB_XR_DLSSNR_STRUCTURE'), 1.0,
         min=0.0, max=1.0, step=0.05, advanced=True, only_if=nr, help='Local structure (detail) strength.'))
    a(_o('dlssnr_skin', 'dlss', 'DLSS-NR skin', 'float', ('iq', 'dlssnr_skin', 'BB_XR_DLSSNR_SKIN'), 1.0,
         min=0.0, max=1.0, step=0.05, advanced=True, only_if=nr, help='Skin structure strength.'))
    a(_o('dlssnr_fovea', 'dlss', 'DLSS-NR covered area', 'float', ('iq', 'dlssnr_fovea', 'BB_XR_DLSSNR_FOVEA'), 0.66,
         min=0.1, max=1.0, step=0.01, only_if=nr,
         help='Share of the eye image (centred) the first pass covers. 0.66 = the centre 66 %, 1 = all.'))
    a(_o('dlssnr_inner', 'dlss', 'DLSS-NR covered area, passes 2-3', 'float', ('iq', 'dlssnr_inner', 'BB_XR_DLSSNR_INNER'), 0.4,
         min=0.1, max=1.0, step=0.01, advanced=True, only_if=nr, help='Share of the eye image covered by passes 2 and 3.'))
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
    a(_o('evade_scale', 'comfort', 'Dodge distance multiplier', 'float', ('fp', 'evade_scale'), None, min=1.0, max=4.0, step=0.05,
         help='Rolls and quicksteps go this many times further. Game default 1.5, 1 = off. Forced off online. Live.'))
    a(_o('backstep', 'comfort', 'Backstep', 'bool', ('fp', 'backstep'), None,
         help='A dodge with the stick pulled back from the way the hunter faces is the backstep. Game default on. Live.'))
    a(_o('backstep_scale', 'comfort', 'Backstep distance multiplier', 'float', ('fp', 'backstep_scale'), None, min=1.0, max=4.0,
         step=0.05, help='Game default 2.0, 1 = off. Forced off online. Live.'))
    a(_o('backstep_angle', 'comfort', 'Backstep angle (deg)', 'float', ('fp', 'backstep_angle'), None, min=30, max=180, step=5,
         advanced=True, help='How far behind the facing the stick must point. Game default 120.'))
    a(_o('backstep_lock', 'comfort', 'Backstep while locked on', 'bool', ('fp', 'backstep_lock'), None, advanced=True,
         help='Game default on.'))
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
    a(_o('hide', 'body', 'Hide parts of the body', 'choice', ('fp', 'hide'), 'hands_skin',
         choices=['none', 'head', 'body', 'hands', 'hands_skin', 'nohands', 'all'],
         help='body hides all but arms, hands and weapons. hands also hides the bare arms (armour sleeves and gloves stay). hands_skin (default) also hides armour sleeves: bare hands only, gloves vanish. nohands also hides the bare hands (weapons only), for the custom VR hands (custom_hands in ui_layouts.json or BB_XR_CUSTOM_HANDS=1). hands/nohands untested in game.'))
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
    a(_o('item_wheel', 'ui', 'Item wheel (hold the right stick click)', 'bool', ('ui', 'active'), True,
         help='The quick wheel on the right stick click hold, next to the dashboard and the wrist counters.'))
    a(_o('lock_on', 'ui', 'Right stick click locks on', 'bool', ('ui', 'lock_on'), False, live_ui=True))
    a(_o('r3_hold', 'ui', 'Hold time for the item wheel (s)', 'float', ('ui', 'r3_hold_s'), 0.15, min=0.1, max=3.0, step=0.05))
    a(_o('hud_head', 'ui', 'HUD follows your head', 'bool', ('ui', 'hud.head'), False))
    a(_o('hud_wrist', 'ui', 'Wrist HUD', 'bool', ('ui', 'hud.wrist'), True))
    a(_o('hud_distance', 'ui', 'HUD distance (m)', 'float', ('ui', 'hud.distance'), 0.9, min=0.2, max=5.0, step=0.05))
    a(_o('hud_down', 'ui', 'HUD angle below the view (deg)', 'float', ('ui', 'hud.down_deg'), 24, min=0, max=90, step=1))
    a(_o('hud_scale', 'ui', 'HUD size', 'float', ('ui', 'hud.scale'), 1.0, min=0.2, max=5.0, step=0.05))
    a(_o('wrist_scale', 'ui', 'Wrist HUD size', 'float', ('ui', 'hud.wrist_scale'), 0.7, min=0.2, max=5.0, step=0.05))
    a(_o('hud_center', 'ui', 'Centre HUD screen', 'choice', ('iq', 'hud_center', 'BB_XR_HUD_CENTER'), 'menus',
         choices=['menus', 'subtitles', 'auto', 'on', 'off'],
         labels={'menus': 'Menus only', 'subtitles': 'Menus and subtitles', 'auto': 'Auto (menus, messages, prompts)',
                 'on': 'Always on', 'off': 'Off'},
         help='When the flat game image (menus, subtitles, prompts) floats in front of you. Live.'))
    a(_o('hp_color', 'ui', 'Wrist HP bar colour', 'text', ('iq', 'hp_color'), None,
         suggestions=['green', 'red', 'blue', 'cyan', 'yellow', 'orange', 'purple', 'white', '#33cc55'],
         help='A name, #rrggbb or r,g,b. Blank = green. Live.'))
    a(_o('stamina_color', 'ui', 'Wrist stamina bar colour', 'text', ('iq', 'stamina_color'), None,
         suggestions=['green', 'red', 'blue', 'cyan', 'yellow', 'orange', 'purple', 'white', '#33cc55'],
         help='A name, #rrggbb or r,g,b. Blank = green. Live.'))
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

# ---- the pages of the window ---------------------------------------------------------------------
# Sidebar pages and their sections; every option appears on exactly one page (test_every_option_has_a_page).
# 'vr' pages are hidden while the view is Flat. Pages without 'sections' are drawn by the window itself.
PAGES = [
    {'id': 'play', 'label': 'Play'},
    {'id': 'graphics', 'label': 'Graphics', 'vr': True, 'sections': [
        ('Display size (your headset)', ['display_size', 'display_custom', 'display_scale']),
        ('Upscaler', ['upscale', 'dlss_quality', 'dlss_preset', 'dlss_sharpen', 'eye_res', 'aa', 'sharpen']),
        ('DLSS arm and weapon mask', ['dlss_mask', 'dlss_mask_m', 'jitter_sign']),
        ('Neural rendering (experimental)', ['dlssnr', 'dlssnr_passes', 'dlssnr_place', 'dlssnr_style', 'dlssnr_intensity', 'dlssnr_tone',
                                             'dlssnr_structure', 'dlssnr_skin', 'dlssnr_fovea', 'dlssnr_inner']),
        ('Game effects', ['patch_blur', 'patch_aa', 'patch_dof', 'patch_ca', 'patch_lod1', 'patch_lod2'])]},
    {'id': 'display', 'label': 'Display & FPS', 'sections': [
        ('Frame rate', ['fps', 'fps_counter', 'fps_pos']),
        ('VR render mode', ['mode', 'stereo_scale', 'ipd', 'stereo_invert']),
        ('Flat mode', ['flat_fps'])]},
    {'id': 'comfort', 'label': 'VR comfort', 'vr': True, 'sections': [
        ('Turning and walking', ['turn', 'turn_speed', 'snap', 'move', 'patch_autorot', 'patch_sens']),
        ('Camera', ['fp', 'anchor', 'near', 'up', 'forward', 'anim_lock', 'anim_smooth', 'smooth']),
        ('Dodging', ['dash', 'evade_scale', 'backstep', 'backstep_scale', 'backstep_angle', 'backstep_lock']),
        ('HUD and menus', ['ui', 'item_wheel', 'lock_on', 'r3_hold', 'hud_center', 'hud_head', 'hud_wrist', 'hud_distance',
                           'hud_down', 'hud_scale', 'wrist_scale']),
        ('Wrist bars', ['hp_color', 'stamina_color'])]},
    {'id': 'combat', 'label': 'Combat & hands', 'vr': True, 'sections': [
        ('Combat', ['hit', 'hit_scale', 'hit_reach', 'atk_speed', 'face', 'face_dodge', 'face_lockon', 'stamina',
                    'stamina_hits', 'gun', 'hitscale_env']),
        ('Hands and body', ['hide', 'arms', 'arm_attack', 'arm_weight', 'arm_offset', 'arm_rot_l', 'arm_rot_r', 'arm_body',
                            'arm_log'])]},
    {'id': 'controls', 'label': 'Controls', 'vr': True},
    {'id': 'online', 'label': 'Online co-op', 'sections': [
        ('Bells', ['online_autobell', 'online_give_bells']),
        ('Private build', ['online', 'online_server', 'online_game_server'])]},
    {'id': 'advanced', 'label': 'Advanced', 'sections': [
        ('Game', ['exe', 'runtime']),
        ('Logs and debugging', ['fp_log', 'dash_log', 'stereo_log', 'stamina_log', 'stamina_trace', 'ui_probe']),
        ('Extra environment', ['extra_env'])]},
    {'id': 'log', 'label': 'Log'},
    {'id': 'about', 'label': 'About'},
]
PAGE_OF = {oid: (page['id'], title) for page in PAGES for title, ids in page.get('sections', []) for oid in ids}


def visible_schema(public=None):
    public = is_public() if public is None else public
    return [o for o in SCHEMA if not (public and o.get('online'))]


def is_live(opt):
    return opt['target'][0] in ('fp', 'stereo', 'iq', 'ui')


# ---- profiles --------------------------------------------------------------------------------

# ui_layouts.json game.active with and without the item wheel (the bridge's built-in default has none)
WHEEL_MENUS = ['dashboard', 'quick_wheel', 'wrist_counters']
NO_WHEEL_MENUS = ['dashboard', 'wrist_counters']

PROFILES = {
    'Default': {},
    'Comfort': {'turn': 'snap', 'snap': 30, 'anim_lock': '2', 'anim_smooth': 0.35, 'move': 'head'},
    'Performance': {'eye_res': '1.5', 'patch_lod1': True, 'aa': 'fxaa', 'upscale': 'fsr1', 'display_scale': '70'},
    'Quality (DLSS)': {'upscale': 'dlss', 'dlss_quality': 'quality', 'dlss_preset': 'K', 'dlss_sharpen': 0.2,
                       'aa': 'fxaa'},
}
PROFILE_ORDER = ['Default', 'Comfort', 'Performance', 'Quality (DLSS)', 'Custom']
PROFILE_NOTES = {
    'Default': 'Balanced defaults: DLSS (quality, preset K) on an NVIDIA RTX GPU, otherwise FSR 1 upscaling; FXAA.',
    'Comfort': 'Snap turning, locked camera animation with lunge easing.',
    'Performance': 'FSR 1 at 70% display resolution, lower model detail.',
    'Quality (DLSS)': 'DLSS Super Resolution, quality mode, preset K. Falls back to FSR 1 if DLSS is not available.',
    'Custom': 'Your own values; edit any option and this profile is used.',
}


# ---- play modes and VR quality presets ---------------------------------------------------------
#
# The start-up window has three modes (settings["mode"]): VR (solo, in the headset), Co-op (online: the room the
# player hosts or joins, played in VR or on the flat screen: settings["coop_view"]) and Flat (solo, on the monitor).
# The quality preset (settings["quality"]) is a layer of option values under the profile and under the player's own
# edits (settings["overrides"]); choosing a preset drops the overrides of the options it sets.

MODES = ['vr', 'coop', 'flat']
MODE_LABELS = {'vr': 'VR', 'coop': 'Co-op', 'flat': 'Flat'}
MODE_NOTES = {
    'vr': 'Play alone in the headset.',
    'coop': 'Play online with up to two friends (host a room or join one with its code), in VR or on the flat screen.',
    'flat': 'Play alone on the monitor, no headset needed.',
}
COOP_VIEWS = ['vr', 'flat']

QUALITY_ORDER = ['Low', 'Medium', 'High', 'Super High']
# The presets are TRUE STEREO presets: none of them sets the render mode (option "mode", default Stereo = BB_XR_MODE=stereo
# + BB_STEREO_FULL=1, both eyes rendered). Mono is the player's manual fallback; the hybrid / alternate-eye modes are advanced.
# A saved file with a preset that no longer exists (the removed Ultra) loads as High.
# What a preset sets. The game renders the scene at 1920x1080 and the upscaler makes the eye image: the eye image is
# 1080p x 1 / 1.5 / 1.72 / 2 / 3 for dlss_quality dlaa / quality / balanced / performance / ultra (auto: the Eye
# resolution option). A "higher" DLSS mode is therefore MORE eye pixels and costs more, not less, so the presets climb
# the eye image size and Low is the cheapest. eye_res None = the game's default (2x for FSR 1).
_PRESET_COMMON = {'aa': 'fxaa', 'dlss_preset': 'K', 'dlss_mask': True, 'dlss_mask_m': 0.9, 'dlssnr': False,
                  'dlssnr_passes': 1, 'dlssnr_place': 'post'}
QUALITY_PRESETS = {
    'Low': {
        'dlss': {'upscale': 'dlss', 'dlss_quality': 'dlaa', 'dlss_sharpen': 0.3, 'eye_res': None, 'patch_lod1': True,
                 'display_scale': '70'},
        'fsr1': {'upscale': 'fsr1', 'sharpen': 0.6, 'eye_res': '1.5', 'patch_lod1': True, 'display_scale': '70'},
    },
    'Medium': {
        'dlss': {'upscale': 'dlss', 'dlss_quality': 'auto', 'dlss_sharpen': 0.25, 'eye_res': '1.25', 'patch_lod1': False,
                 'display_scale': '85'},
        'fsr1': {'upscale': 'fsr1', 'sharpen': 0.55, 'eye_res': '1.75', 'patch_lod1': False, 'display_scale': '85'},
    },
    'High': {  # what the project has played with so far
        'dlss': {'upscale': 'dlss', 'dlss_quality': 'quality', 'dlss_sharpen': 0.2, 'eye_res': None, 'patch_lod1': False},
        'fsr1': {'upscale': 'fsr1', 'sharpen': 0.5, 'eye_res': None, 'patch_lod1': False},
    },
    'Super High': {  # High plus DLSS-NR placed before the upscaler (the player's own nvngx_dlssnr.dll); without DLSS it is High's FSR 1 values
        'dlss': {'upscale': 'dlss', 'dlss_quality': 'quality', 'dlss_sharpen': 0.2, 'eye_res': None, 'patch_lod1': False,
                 'dlssnr': True, 'dlssnr_passes': 1, 'dlssnr_place': 'pre'},
        'fsr1': {'upscale': 'fsr1', 'sharpen': 0.5, 'eye_res': None, 'patch_lod1': False},
    },
}
QUALITY_NOTES = {
    'Low': 'Cheapest: eye image at the render size (1920x1080), lower model detail, Display resolution 70%. For GPUs that miss the frame limit.',
    'Medium': 'A step up: eye image 2400x1350 (DLSS) or 3360x1890 (FSR 1), Display resolution 85%.',
    'High': 'The reference look: DLSS quality (2880x1620 per eye) or FSR 1 at 2x. The default.',
    'Super High': "High plus NVIDIA's neural rendering (DLSS-NR, placed before the upscaler), 2880x1620 per eye. The model file "
                  "nvngx_dlssnr.dll is NOT included: you supply your own copy (press Find file... to pick it on this PC; it is copied "
                  "next to bb-probe.exe). About 50-54 fps on an RTX 4090, full stereo.",
}
# Measured on an RTX 4090 in full stereo (shown on the card). Presets without a number have not been measured.
QUALITY_FPS = {'Super High': 'about 50-54 fps on an RTX 4090, full stereo'}
NR_PRESETS = ('Super High',)  # presets that need DLSS-NR
NR_DLL = 'nvngx_dlssnr.dll'
NR_DLL_MISSING = ("nvngx_dlssnr.dll (NVIDIA's DLSS-NR model file) is not included with the mod: use your own copy and press "
                  "\"Find file...\" to put it next to bb-probe.exe.")
QUALITY_DEFAULT = 'High'
DLSS_EYE_FACTOR = {'dlaa': 1.0, 'quality': 1.5, 'balanced': 1.0 / 0.58, 'performance': 2.0, 'ultra': 3.0}
PRESET_KEYS = sorted({k for p in QUALITY_PRESETS.values() for t in p.values() for k in t} | set(_PRESET_COMMON))


def dlss_ok_of(caps):
    return bool(((caps or {}).get('dlss') or {}).get('ok'))


def preset_values(name, dlss_ok):
    """The option values of a quality preset for this PC (DLSS where it works, else FSR 1)."""
    p = QUALITY_PRESETS.get(name) or QUALITY_PRESETS[QUALITY_DEFAULT]
    out = dict(_PRESET_COMMON)
    out.update(p['dlss' if dlss_ok else 'fsr1'])
    return out


def eye_image(values):
    """(width, height) of the eye image a set of values gives (the scene is rendered at 1920x1080)."""
    if values.get('upscale') == 'dlss' and values.get('dlss_quality') in DLSS_EYE_FACTOR:
        f = DLSS_EYE_FACTOR[values['dlss_quality']]
    else:
        text = str(values.get('eye_res') or '2')
        m = re.fullmatch(r'(\d+)x(\d+)', text)
        if m:
            return int(m.group(1)), int(m.group(2))
        try:
            f = float(text)
        except ValueError:
            f = 2.0
    return int(round(1920 * f)), int(round(1080 * f))


def nr_state(caps):
    """(ok, reason) of DLSS-NR on this PC (detect_dlssnr: DLSS works, the player's own nvngx_dlssnr.dll is there)."""
    nr = (caps or {}).get('dlssnr') or {}
    return bool(nr.get('ok')), str(nr.get('reason') or NR_DLL_MISSING)


def preset_table(caps=None):
    """The "what does each preset set" table: {'dlss': bool, 'presets': [{name, note, fps, available, reason, values,
    rows}]}. A preset that needs DLSS-NR is not available without it (the window greys it out with the reason)."""
    dlss_ok = dlss_ok_of(caps)
    nr_ok, nr_reason = nr_state(caps)
    out = []
    for name in QUALITY_ORDER:
        v = preset_values(name, dlss_ok)
        w, h = eye_image(v)
        dlss = v['upscale'] == 'dlss'
        mode = ('from the eye size' if v['dlss_quality'] == 'auto' else v['dlss_quality']) if dlss else '-'
        rows = [['Upscaler', 'DLSS' if dlss else 'FSR 1'], ['DLSS mode', mode], ['Eye image (each eye)', f'{w}x{h}'],
                ['Display resolution', (v['display_scale'] + '% of the headset size') if v.get('display_scale') else 'headset size (100%)'],
                ['Sharpening', fmt_num(v['dlss_sharpen'] if dlss else v['sharpen'])],
                ['Anti-aliasing', 'FXAA' if v['aa'] == 'fxaa' else str(v['aa'])],
                ['Model detail', 'lower (LOD 1)' if v.get('patch_lod1') else 'as the game'],
                ['DLSS-NR', ('on, %d pass%s' % (v['dlssnr_passes'], ', before upscaling' if v.get('dlssnr_place') == 'pre' else ''))
                 if v.get('dlssnr') else 'off']]
        needs = name in NR_PRESETS
        out.append({'name': name, 'note': QUALITY_NOTES[name], 'fps': QUALITY_FPS.get(name, ''),
                    'available': nr_ok or not needs, 'reason': '' if (nr_ok or not needs) else nr_reason,
                    'values': v, 'rows': rows})
    return {'dlss': dlss_ok, 'presets': out}


def base_values():
    return {o['id']: o['default'] for o in SCHEMA}


STICKY = {o['id'] for o in SCHEMA if o.get('sticky')}  # hardware choices: kept in settings['hw'], over any profile


def auto_defaults(caps=None):
    """The hardware-dependent part of the defaults: the default quality preset (High) for this PC (DLSS when the DLSS check
    passes - NVIDIA RTX GPU, DLSS-capable exe, both DLLs present - else FSR 1), and Display resolution 85% when the GPUs
    found are not NVIDIA RTX (findings/41). A profile, the saved Custom values and the player's edits go over it, so a
    choice is never overridden."""
    out = preset_values(QUALITY_DEFAULT, dlss_ok_of(caps))
    out.update(hw_defaults(caps))
    return out


def hw_defaults(caps=None):
    """Display resolution 85% unless an NVIDIA RTX GPU is found (an empty / unknown GPU list gives 100%)."""
    if caps and caps.get('gpus') and not any(g.get('rtx') for g in caps['gpus']):
        return {'display_scale': '85'}
    return {}


def effective_values(settings, caps=None):
    values = base_values()
    values.update(hw_defaults(caps))  # under the preset: Low / Medium set their own Display resolution
    q = settings.get('quality')
    if q in QUALITY_PRESETS:
        values.update(preset_values(q, dlss_ok_of(caps)))
    else:
        values.update(auto_defaults(caps))
    name = settings.get('profile', 'Default')
    if name == 'Custom':
        values.update({k: v for k, v in settings.get('custom', {}).items() if k in BY_ID and k not in STICKY})
    else:
        values.update(PROFILES.get(name, {}))
    values.update({k: v for k, v in (settings.get('overrides') or {}).items() if k in BY_ID})
    values.update({k: v for k, v in (settings.get('hw') or {}).items() if k in STICKY})
    return values


def reference_values(settings, caps=None):
    """effective_values without the player's own edits: what the edits are measured against."""
    return effective_values(dict(settings, overrides={}), caps)


def play_view(settings):
    """'flat' or 'vr': what the game window is. Flat mode, and Co-op played flat, are flat."""
    mode = settings.get('mode', 'vr')
    return 'flat' if mode == 'flat' or (mode == 'coop' and settings.get('coop_view') == 'flat') else 'vr'


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
    return {'version': SCHEMA_VERSION, 'profile': 'Default', 'custom': {}, 'show_menu': True,
            'mode': 'vr', 'coop_view': 'vr', 'quality': QUALITY_DEFAULT, 'overrides': {}, 'hw': {}}


def load_settings(path):
    """Older files (profile, custom, show_menu only) load as they were: mode VR, quality High, no overrides."""
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        if not isinstance(data, dict):
            raise ValueError
    except (OSError, ValueError):
        return default_settings()
    s = default_settings()
    s.update({k: data[k] for k in ('profile', 'custom', 'show_menu', 'mode', 'coop_view', 'quality', 'overrides') if k in data})
    if s['profile'] not in PROFILE_ORDER:
        s['profile'] = 'Default'
    if not isinstance(s['custom'], dict):
        s['custom'] = {}
    s['custom'] = {k: v for k, v in s['custom'].items() if k in BY_ID}
    if data.get('version', 1) < 2 and s['custom'].get('mode') == 'alt':
        del s['custom']['mode']  # alt was the default of version 1 (a Custom profile saved it with everything else): Stereo is the default now
    if s['mode'] not in MODES:
        s['mode'] = 'vr'
    if s['coop_view'] not in COOP_VIEWS:
        s['coop_view'] = 'vr'
    if s['quality'] not in QUALITY_PRESETS:
        s['quality'] = QUALITY_DEFAULT
    if not isinstance(s['overrides'], dict):
        s['overrides'] = {}
    s['overrides'] = {k: v for k, v in s['overrides'].items() if k in BY_ID}
    hw = data.get('hw')
    s['hw'] = {k: coerce(BY_ID[k], v) for k, v in hw.items() if k in STICKY} if isinstance(hw, dict) else {}
    return s


def save_settings(path, settings):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    out = {'version': SCHEMA_VERSION, 'profile': settings['profile'], 'custom': settings.get('custom', {}),
           'show_menu': bool(settings.get('show_menu', True)), 'mode': settings.get('mode', 'vr'),
           'coop_view': settings.get('coop_view', 'vr'), 'quality': settings.get('quality', QUALITY_DEFAULT),
           'overrides': settings.get('overrides', {})}
    if settings.get('hw'):
        out['hw'] = settings['hw']
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def apply_edit(settings, values_from_page, profile=None, caps=None):
    """The page sent the full value set of what it shows. Same as the profile: keep the profile; any
    difference: the profile becomes Custom holding the values. Returns the new settings. (The older pages' way of
    saving; the new windows send only the changed options, see apply_changes.)"""
    s = copy.deepcopy(settings)
    if profile:
        s['profile'] = profile if profile in PROFILE_ORDER else s['profile']
    if values_from_page is None:
        return s
    page = {}
    for oid, v in values_from_page.items():
        if oid in BY_ID:
            page[oid] = coerce(BY_ID[oid], v)
    # Hardware choices (Display size) go to settings['hw']: they never turn the profile into Custom.
    hw = {k: page.pop(k) for k in list(page) if k in STICKY}
    if hw:
        s['hw'] = dict(s.get('hw') or {}, **hw)
        s['hw'] = {k: v for k, v in s['hw'].items() if v is not None and v != BY_ID[k]['default']}
    reference = effective_values(s, caps)
    changed = {k: v for k, v in page.items() if reference.get(k) != v}
    if s['profile'] == 'Custom':
        merged = reference
        merged.update(page)
        s['custom'] = {k: merged[k] for k in merged if k in BY_ID and k not in STICKY}
    elif changed:
        merged = reference
        merged.update(page)
        s['profile'] = 'Custom'
        s['custom'] = {k: merged[k] for k in merged if k not in STICKY}
    return s


def apply_changes(settings, changes, caps=None):
    """The player changed some options: {id: value}. They are kept as overrides over the quality preset and the
    profile (an option that gets back to the reference value is dropped from the overrides). Returns new settings."""
    s = copy.deepcopy(settings)
    ref = reference_values(s, caps)
    ov = dict(s.get('overrides') or {})
    for oid, v in (changes or {}).items():
        if oid not in BY_ID:
            continue
        v = coerce(BY_ID[oid], v)
        if oid in STICKY:  # Display size is a hardware choice: kept in settings['hw'], never an override of a preset
            hw = dict(s.get('hw') or {})
            if v is None or v == BY_ID[oid]['default']:
                hw.pop(oid, None)
            else:
                hw[oid] = v
            s['hw'] = hw
            continue
        if ref.get(oid) == v:
            ov.pop(oid, None)
        else:
            ov[oid] = v
    s['overrides'] = ov
    return s


def set_quality(settings, name):
    """Choose a quality preset: the overrides (and Custom values) of the options it sets are dropped, and the older
    Performance / Quality (DLSS) profiles, which the presets replace, go back to Default."""
    s = copy.deepcopy(settings)
    if name not in QUALITY_PRESETS:
        return s
    s['quality'] = name
    s['overrides'] = {k: v for k, v in (s.get('overrides') or {}).items() if k not in PRESET_KEYS}
    s['custom'] = {k: v for k, v in (s.get('custom') or {}).items() if k not in PRESET_KEYS}
    if s.get('profile') in ('Performance', 'Quality (DLSS)'):
        s['profile'] = 'Default'
    return s


def modified_preset_keys(settings):
    """The options of the quality preset that the player changed (the Graphics page says "Custom" then)."""
    return sorted(k for k in (settings.get('overrides') or {}) if k in PRESET_KEYS)


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
                            try:
                                raw_driver = str(wr.QueryValueEx(k, 'DriverVersion')[0])
                            except OSError:
                                raw_driver = ''
                    except OSError:
                        continue
                    if name and name not in [g['name'] for g in gpus]:
                        gpus.append({'name': name, 'rtx': bool(re.search(r'\bRTX\b|\bTITAN RTX\b', name, re.I)),
                                     'driver': friendly_driver(name, raw_driver)})
        except OSError:
            pass
    return gpus


def friendly_driver(name, raw):
    """The driver version as the vendor shows it. NVIDIA's registry value 31.0.15.5186 is driver 551.86; others as stored."""
    m = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)\.(\d+)', raw or '')
    if m and re.search(r'nvidia|geforce|rtx|quadro', name, re.I):
        digits = m.group(3) + m.group(4)
        digits = digits[-5:]
        return f'{int(digits[:-2])}.{digits[-2:]}'
    return raw or ''


def detect_vulkan():
    """True when the Vulkan loader (vulkan-1.dll) can be loaded."""
    try:
        ctypes.WinDLL('vulkan-1.dll')
        return True
    except (OSError, AttributeError):
        return False


_SCAN_CACHE = {}


def _file_has(path, *needles):
    """True when the file contains every byte string in `needles` (cached by path, mtime and size)."""
    path = Path(path)
    try:
        stat = path.stat()
    except OSError:
        return False
    key = (str(path), stat.st_mtime_ns, stat.st_size, needles)
    if key not in _SCAN_CACHE:
        found = False
        try:
            with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                found = all(m.find(n) >= 0 for n in needles)
        except (OSError, ValueError):
            pass
        _SCAN_CACHE[key] = found
    return _SCAN_CACHE[key]


def exe_supports_dlss(path):
    """True when the executable was built with the VR DLSS path (its string table has the key names)."""
    return _file_has(path, b'dlss_sharpen', b'BB_XR_DLSS_QUALITY')


def exe_supports_dlssnr(path):
    """True when the executable was built with the DLSS-NR pass."""
    return _file_has(path, b'BB_XR_DLSSNR')


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


def detect_dlssnr(port, exe_override=None, gpus=None, dlss=None):
    """{'ok', 'reason', 'dir'}. ok = DLSS-NR can run: DLSS works, the DLSS folder holds the player's own
    nvngx_dlssnr.dll (never shipped), and the executable and bbport_dlss.dll both know DLSS-NR."""
    if dlss is None:
        dlss = detect_dlss(port, exe_override, gpus)
    result = {'ok': False, 'reason': '', 'dir': None}
    if not dlss['ok']:
        result['reason'] = 'DLSS-NR needs DLSS: ' + dlss['reason']
        return result
    folder = Path(dlss['dir'])
    result['dir'] = folder
    if not exe_supports_dlssnr(dlss['exe']):
        result['reason'] = 'This build has no DLSS-NR support.'
    elif not _file_has(folder / 'bbport_dlss.dll', b'BbDlssGetNrApi'):
        result['reason'] = 'bbport_dlss.dll is too old for DLSS-NR (use the one from this build).'
    elif not (folder / 'nvngx_dlssnr.dll').is_file():
        result['reason'] = f"nvngx_dlssnr.dll is not included with the mod: use your own copy (press 'Find file...' to copy it next to bb-probe.exe in {folder})."
    else:
        result['ok'] = True
    return result


# ---- the DLSS-NR model file (the player's own copy) -----------------------------------------------
# nvngx_dlssnr.dll is NVIDIA's and is NOT part of the public DLSS SDK: the mod neither ships it nor links to a download.
# "Find file..." lets the player pick a copy they already have; it is copied next to the game executable on their PC.

NR_DOC = 'dlss-nr.md'
NR_DOC_GITHUB = 'https://github.com/{repo}/blob/HEAD/docs/vr/dlss-nr.md'


def pick_file(title='Select a file', filter_text='All files (*.*)|*.*'):
    """A file dialog through Windows PowerShell; None when it is unavailable or cancelled."""
    import subprocess
    script = ("Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; "
              "$d.Title = '%s'; $d.Filter = '%s'; if ($d.ShowDialog() -eq 'OK') { $d.FileName }"
              % (title.replace("'", "''"), filter_text.replace("'", "''")))
    try:
        result = subprocess.run(['powershell', '-NoProfile', '-STA', '-Command', script], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=300)
        return result.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def nr_target_dir(port, exe_override=None, gpus=None):
    """The folder the model file belongs in: next to the game executable that DLSS uses (else the main one)."""
    dlss = detect_dlss(port, exe_override, gpus)
    if dlss.get('dir'):
        return Path(dlss['dir'])
    main = main_exe(port, exe_override)
    return main.parent if main else None


def install_nr_dll(src, port, exe_override=None, gpus=None):
    """Copies the player's own nvngx_dlssnr.dll next to the game executable. Returns {'ok', 'message', 'dest'}.
    Only a file named nvngx_dlssnr.dll that looks like a Windows DLL is accepted; nothing is downloaded."""
    text = str(src or '').strip().strip('"')
    if not text:
        return {'ok': False, 'message': 'No file chosen.', 'dest': ''}
    path = Path(text)
    if path.name.lower() != NR_DLL:
        return {'ok': False, 'message': f'Pick the file named {NR_DLL} (this one is {path.name}).', 'dest': ''}
    try:
        head = path.open('rb').read(2) if path.is_file() else b''
        size = path.stat().st_size if path.is_file() else 0
    except OSError as e:
        return {'ok': False, 'message': f'Cannot read that file: {e.strerror or e}.', 'dest': ''}
    if not path.is_file():
        return {'ok': False, 'message': 'That file does not exist.', 'dest': ''}
    if head != b'MZ' or size < 64 * 1024:
        return {'ok': False, 'message': f'That is not a usable {NR_DLL} (it is not a Windows DLL, or it is far too small).', 'dest': ''}
    folder = nr_target_dir(port, exe_override, gpus)
    if folder is None:
        return {'ok': False, 'message': 'No game executable (bb-probe.exe) found to put it next to.', 'dest': ''}
    dest = folder / NR_DLL
    try:
        if dest.exists() and dest.resolve() == path.resolve():
            return {'ok': True, 'message': f'{NR_DLL} is already in {folder}.', 'dest': str(dest)}
        tmp = folder / (NR_DLL + '.tmp')
        shutil.copyfile(path, tmp)
        os.replace(tmp, dest)
    except OSError as e:
        return {'ok': False, 'message': f'Could not copy it to {folder}: {e.strerror or e} (close the game first).', 'dest': ''}
    return {'ok': True, 'message': f'Copied {NR_DLL} to {folder}. It stays on this PC; it is not part of the mod.', 'dest': str(dest)}


def _md_to_html(text):
    """The help page as a small HTML page (headings, paragraphs, bullet lists, **bold**, `code`)."""
    import html

    def inline(s):
        s = html.escape(s)
        s = re.sub(r'\*\*(.+?)\*\*', r'<b>\1</b>', s)
        return re.sub(r'`(.+?)`', r'<code>\1</code>', s)

    out, para, items = [], [], []

    def flush():
        if para:
            out.append('<p>' + inline(' '.join(para)) + '</p>')
            para.clear()
        if items:
            out.append('<ul>' + ''.join('<li>' + inline(i) + '</li>' for i in items) + '</ul>')
            items.clear()

    for line in text.splitlines():
        m = re.match(r'(#{1,3})\s+(.*)', line)
        if m:
            flush()
            out.append(f'<h{len(m.group(1))}>{inline(m.group(2))}</h{len(m.group(1))}>')
        elif line.startswith(('- ', '* ')):
            if para:
                flush()
            items.append(line[2:].strip())
        elif not line.strip():
            flush()
        else:
            if items:
                flush()
            para.append(line.strip())
    flush()
    return ('<!doctype html><meta charset="utf-8"><title>DLSS-NR (Super High)</title>'
            '<style>body{font:16px/1.55 system-ui,sans-serif;max-width:46em;margin:2em auto;padding:0 1em;color:#222;background:#fff}'
            'code{background:#eee;padding:0 .25em;border-radius:3px}@media(prefers-color-scheme:dark){body{color:#ddd;background:#181818}'
            'code{background:#333}}</style>' + ''.join(out))


def nr_doc_source(port):
    """The local copy of the DLSS-NR help page (release: docs/dlss-nr.md; source tree: docs/vr/dlss-nr.md), or None."""
    for cand in (Path(port) / 'docs' / NR_DOC, Path(port) / 'docs' / 'vr' / NR_DOC):
        if cand.is_file():
            return cand
    return None


def nr_doc_url(port, data=None):
    """{'url', 'where'}: a file:// URL of the local page (rendered to a small HTML file in the data folder), or the GitHub copy
    of docs/vr/dlss-nr.md on the public repo when the local file is missing."""
    src = nr_doc_source(port)
    if src is not None:
        try:
            folder = Path(data) if data else Path(port) / 'out'
            folder.mkdir(parents=True, exist_ok=True)
            page = folder / 'dlss-nr-help.html'
            page.write_text(_md_to_html(src.read_text(encoding='utf-8', errors='replace')), encoding='utf-8')
            return {'url': page.resolve().as_uri(), 'where': 'local'}
        except OSError:
            pass
    repo = (release_defaults(port) or {}).get('update_repo') or 'laughtt/bloodborne-vr'
    return {'url': NR_DOC_GITHUB.format(repo=repo), 'where': 'github'}


def open_nr_doc(port, data=None):
    """Opens the DLSS-NR help page in the default browser. Returns {'ok', 'where', 'url'}."""
    import webbrowser
    info = nr_doc_url(port, data)
    try:
        ok = bool(webbrowser.open(info['url']))
    except webbrowser.Error:
        ok = False
    return dict(info, ok=ok)


def _runtime_name(json_path):
    try:
        data = json.loads(Path(json_path).read_text(encoding='utf-8-sig', errors='replace'))
        return data.get('runtime', {}).get('name') or data.get('name') or Path(json_path).stem
    except (OSError, ValueError, AttributeError):
        return Path(json_path).stem


def classify_runtime(path, name=''):
    text = f'{path} {name}'.lower()
    if 'virtualdesktop' in text or 'virtual desktop' in text or 'vdxr' in text:
        return 'vdxr'
    if 'steam' in text:
        return 'steamvr'
    if 'pimax' in text:
        return 'pimax'
    if 'oculus' in text or 'meta' in text:
        return 'oculus'
    if 'windowsmixedreality' in text or 'mixed reality' in text or 'mixedreality' in text:
        return 'wmr'
    if 'varjo' in text:
        return 'varjo'
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


# ---- display size: headset detection and sizes (docs/vr/findings/41-display-size.md) -------------

# A typical headset for a runtime when no game log says more (the conservative, common case).
RUNTIME_TYPICAL = {'oculus': 'quest2', 'vdxr': 'quest2', 'steamvr': 'index', 'pimax': 'crystal', 'wmr': 'reverb',
                   'varjo': 'aero'}
RUNTIME_LABELS = {'oculus': 'Meta Quest Link / Air Link', 'vdxr': 'Virtual Desktop', 'steamvr': 'SteamVR',
                  'pimax': 'Pimax', 'wmr': 'Windows Mixed Reality', 'varjo': 'Varjo', 'other': 'OpenXR'}
# Headset names in the game log ("VR: headset <systemName>, ...") -> preset; first match wins.
HEADSET_NAMES = [(r'quest\s*3s', 'quest2'), (r'quest\s*3', 'quest3'), (r'quest\s*pro', 'questpro'),
                 (r'quest\s*2', 'quest2'), (r'quest', 'quest1'), (r'dream\s*air', 'dreamair'), (r'crystal', 'crystal'),
                 (r'\bindex\b', 'index'), (r'vive\s*pro', 'vivepro'), (r'\bvive\b', 'vive'), (r'rift\s*s', 'rifts'),
                 (r'reverb', 'reverb'), (r'aero', 'aero'), (r'beyond', 'beyond'), (r'steam\s*frame', 'frame')]
LOG_READ_BYTES = 4 << 20  # the lines come early in a session; logs can be long


def headset_preset(name):
    text = str(name or '').lower()
    return next((pid for pat, pid in HEADSET_NAMES if re.search(pat, text)), None)


def last_session(logs_dir):
    """{'runtime', 'headset', 'rec': (w, h), 'file'} from the newest launcher log that has the bridge's
    "VR: eye images: recommended WxH" line, else None. Reads only the files (the game is not started)."""
    try:
        logs = sorted(Path(logs_dir).glob('bbvr-*.log'), key=lambda p: p.stat().st_mtime, reverse=True)[:6]
    except OSError:
        return None
    for log in logs:
        try:
            with open(log, 'rb') as f:
                text = f.read(LOG_READ_BYTES).decode('utf-8', errors='replace')
        except OSError:
            continue
        recs = list(re.finditer(r'VR: eye images: recommended (\d+)x(\d+)', text))
        if not recs:
            continue
        last = recs[-1]
        before = text[:last.start()]
        runtime = re.findall(r'VR: runtime ([^\r\n]+)', before)
        headset = re.findall(r'VR: headset ([^\r\n,]+)', before)
        return {'runtime': runtime[-1].strip() if runtime else '', 'headset': headset[-1].strip() if headset else '',
                'rec': (int(last.group(1)), int(last.group(2))), 'file': str(log)}
    return None


def detect_headset(runtimes, logs_dir=None):
    """What Display size = Auto sizes for, without starting the game: the recommended per-eye size of the last
    session (game log) when it ran on the same kind of runtime as the active one now, else a typical headset of
    the active runtime. {'kind', 'runtime', 'source': 'log'|'runtime'|None, 'rec', 'name', 'preset'}"""
    runtimes = runtimes or {}
    active = runtimes.get('active') or ''
    rt = next((r for r in runtimes.get('runtimes', []) if r.get('active')), None)
    name = rt['name'] if rt else (Path(active).stem if active else '')
    kind = classify_runtime(active, name) if active else None
    out = {'kind': kind, 'runtime': name, 'source': None, 'rec': None, 'name': '', 'preset': None}
    log = last_session(logs_dir) if logs_dir else None
    if log and (kind is None or classify_runtime('', log['runtime']) == kind):
        out.update(source='log', rec=list(log['rec']), name=log['headset'] or log['runtime'],
                   preset=headset_preset(log['headset']), log=log['file'])
        if kind is None:
            out['kind'], out['runtime'] = classify_runtime('', log['runtime']), log['runtime']
        return out
    typical = RUNTIME_TYPICAL.get(kind)
    if typical:
        h = HEADSET_BY_ID[typical]
        out.update(source='runtime', rec=list(h[2]), name=h[1], preset=typical)
    return out


NATIVE_FRAME = (1920, 1080)
AREA_CAP = 2064 * 2208      # 100% = the headset's pixel count up to a Quest 3 eye (4.56 Mpx): today's cost
EYE_CAP_H = 2160            # 100% eye image height for FSR 1, up to today's 3840x2160
EYE_H = (720, 2880)         # eye image height limits (16:9: 1280x720 .. 5120x2880)
RENDER_H = (540, 1440)      # render height limits (1440p measured in VR, findings/30 5; lower: flat 720p path)
NATIVE_SNAP = 0.04          # a render size within 4% of 1080p stays the game's own (no startup patch)
DLSS_RATIOS = {'dlaa': 1.0, 'quality': 1.5, 'balanced': 1 / 0.58, 'performance': 2.0, 'ultra': 3.0, 'auto': 1.5}


def parse_size(text):
    m = re.fullmatch(r'\s*(\d{3,5})\s*[xX*]\s*(\d{3,5})\s*', str(text or ''))
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    return (w, h) if 256 <= w <= 8192 and 256 <= h <= 8192 else None


def size_16x9(height):
    """A 16:9 size of about this height, both sides even (the game has half-resolution buffers)."""
    h = max(2, int(round(height / 2)) * 2)
    return (int(round(h * 16 / 9 / 2)) * 2, h)


def fmt_size(size):
    return f'{size[0]}x{size[1]}' if size else ''


def display_sizes(values, caps=None, upscale=None):
    """The render and eye image sizes for the Display size options. None when the game's own sizes are used
    (Display size = Game default, or Auto without a known headset). The frame stays 16:9 (non-16:9 render
    sizes are untested with this game, findings/23 P4.7): the image is stretched over the eye's field of view
    as today. Rule (findings/41):
      ref    = the headset's recommended per-eye size (Auto: last session / runtime; preset; custom)
      s      = Display resolution / 100;  A = s^2 * min(ref area, AREA_CAP)
      DLSS:  eye = 16:9 of area A; render = eye / mode ratio (auto: 1.5)
      FSR 1 / off: render = 16:9 of area A / 1.5^2; eye height = s * min(max(ref h, ref w * 9/16), 2160)
    then clamped (eye 720..2880 tall, render 540..1440 tall, render <= eye), and a render within 4% of 1080p
    stays native (no BB_RENDER_RES)."""
    choice = values.get('display_size') or 'auto'
    if choice == 'game':
        return None
    label, source = '', choice
    if choice == 'custom':
        ref = parse_size(values.get('display_custom'))
        label = 'Custom'
    elif choice in HEADSET_BY_ID:
        ref, label, source = HEADSET_BY_ID[choice][2], HEADSET_BY_ID[choice][1], 'preset'
    else:
        det = (caps or {}).get('headset') or {}
        ref = tuple(det['rec']) if det.get('rec') else None
        label, source = det.get('name') or '', det.get('source') or 'auto'
    if not ref:
        return None
    upscale = upscale or values.get('upscale') or 'fsr1'
    try:
        s = max(0.25, min(2.0, float(values.get('display_scale') or 100) / 100))
    except ValueError:
        s = 1.0
    area = s * s * min(ref[0] * ref[1], AREA_CAP)
    area_h = (area * 9 / 16) ** 0.5
    quality = values.get('dlss_quality') or 'auto'
    if upscale == 'dlss':
        ratio = DLSS_RATIOS.get(quality, 1.5)
        eye_h = max(EYE_H[0], min(EYE_H[1], area_h))
        render_h = eye_h / ratio
    else:
        eye_h = max(EYE_H[0], min(EYE_H[1], s * min(max(ref[1], ref[0] * 9 / 16), EYE_CAP_H)))
        render_h = area_h / 1.5
    render_h = max(RENDER_H[0], min(RENDER_H[1], render_h, eye_h))
    render = size_16x9(render_h)
    native = abs(render[1] - NATIVE_FRAME[1]) <= NATIVE_FRAME[1] * NATIVE_SNAP
    if native:
        render = NATIVE_FRAME
    eye = size_16x9(eye_h)
    if upscale == 'dlss' and quality in DLSS_RATIOS and quality != 'auto':
        r = DLSS_RATIOS[quality]  # an explicit DLSS mode sizes the eye image from the render size (xr_dlss_logic.h)
        eye = (min(8192, int(round(render[0] * r))), min(8192, int(round(render[1] * r))))
    return {'choice': choice, 'label': label, 'source': source, 'ref': list(ref), 'scale': int(round(s * 100)),
            'upscale': upscale, 'render': list(render), 'native': native, 'eye': list(eye)}


def detect_all(port, exe_override=None, data=None):
    gpus = detect_gpus()
    dlss = detect_dlss(port, exe_override, gpus)
    runtimes = detect_runtimes()
    data = Path(data) if data else data_root(port)
    return {'gpus': gpus, 'dlss': dlss, 'dlssnr': detect_dlssnr(port, exe_override, gpus, dlss), 'runtimes': runtimes,
            'headset': detect_headset(runtimes, data / 'logs')}


# ---- the game folder and the ready check ----------------------------------------------------

GAME_TITLE_ID = 'CUSA03173'
GAME_APP_VERSION = '01.09'


def read_sfo(path):
    """Minimal param.sfo reader: {key: value} of the string / integer entries."""
    data = Path(path).read_bytes()
    magic, _, key_table, data_table, count = struct.unpack_from('<4sIIII', data, 0)
    if magic != b'\0PSF':
        raise ValueError('not a param.sfo')
    result = {}
    for i in range(count):
        key_off, fmt, length, _, data_off = struct.unpack_from('<HHIII', data, 20 + i * 16)
        key = data[key_table + key_off:data.index(b'\0', key_table + key_off)].decode('ascii', 'replace')
        raw = data[data_table + data_off:data_table + data_off + length]
        result[key] = int.from_bytes(raw[:4], 'little') if fmt == 0x0404 else raw.split(b'\0')[0].decode('utf-8', 'replace')
    return result


def game_info(game):
    """Title, version and the banner picture of the player's own game folder (sce_sys/param.sfo, sce_sys/pic1.png).
    The picture is read from there when the window opens; it is never copied or shipped."""
    info = {'title_id': '', 'version': '', 'name': '', 'banner': ''}
    if not game:
        return info
    try:
        sfo = read_sfo(Path(game) / 'sce_sys' / 'param.sfo')
        info.update(title_id=str(sfo.get('TITLE_ID', '')), version=str(sfo.get('APP_VER', '')), name=str(sfo.get('TITLE', '')))
    except (OSError, ValueError, KeyError, struct.error):
        pass
    pic = Path(game) / 'sce_sys' / 'pic1.png'
    if pic.is_file():
        info['banner'] = str(pic)
    return info


def ready_check(app, caps, view, info, vr_data_built):
    """The ready check of the Play page: [{id, title, status, detail}] with status ok | warn | bad | info | na.
    Cheap facts only (files, registry); nothing is started. In the Flat view the headset rows say they are not needed."""
    flat = view == 'flat'
    rows = []
    if app['eboot']:
        ver = info.get('version', '')
        text = (f'{info["title_id"]} {ver}  |  ' if info.get('title_id') else '') + app['game_dir']
        if info.get('title_id') and info['title_id'] != GAME_TITLE_ID:
            rows.append({'id': 'game', 'title': 'Game folder', 'status': 'bad', 'detail': text + ': this is not ' + GAME_TITLE_ID})
        elif ver and ver != GAME_APP_VERSION:
            rows.append({'id': 'game', 'title': 'Game folder', 'status': 'warn',
                         'detail': text + f': the patches need version {GAME_APP_VERSION}'})
        else:
            rows.append({'id': 'game', 'title': 'Game folder', 'status': 'ok', 'detail': text})
    else:
        rows.append({'id': 'game', 'title': 'Game folder', 'status': 'bad',
                     'detail': 'No eboot.bin in ' + app['game_dir'] if app['game_dir']
                     else 'Not set. Choose the folder of YOUR dump that contains eboot.bin.'})
    active = caps.get('active_runtime')
    if flat:
        rows.append({'id': 'headset', 'title': 'Headset (OpenXR runtime)', 'status': 'na', 'detail': 'Not needed in Flat mode.'})
    elif active:
        rows.append({'id': 'headset', 'title': 'Headset (OpenXR runtime)', 'status': 'ok', 'detail': 'Active: ' + str(active)})
    elif caps.get('runtimes'):
        rows.append({'id': 'headset', 'title': 'Headset (OpenXR runtime)', 'status': 'warn',
                     'detail': 'Installed, but none is the active runtime. Set your headset software as the OpenXR runtime.'})
    else:
        rows.append({'id': 'headset', 'title': 'Headset (OpenXR runtime)', 'status': 'bad',
                     'detail': 'None found. Install your headset software and set it as the OpenXR runtime.'})
    # virtual and remote display adapters (Parsec, Microsoft Basic Display, ...) are not graphics cards
    gpus = [g for g in (caps.get('gpus') or []) if not re.search(r'virtual|parsec|basic (display|render)|remote|indirect|mirror|citrix', g['name'], re.I)]
    if gpus:
        names = ', '.join(g['name'] + (' (driver ' + g['driver'] + ')' if g.get('driver') else '') for g in gpus)
        rows.append({'id': 'gpu', 'title': 'Graphics card', 'status': 'ok', 'detail': names})
    else:
        rows.append({'id': 'gpu', 'title': 'Graphics card', 'status': 'warn', 'detail': 'Not detected'})
    rows.append({'id': 'vulkan', 'title': 'Vulkan', 'status': 'ok' if detect_vulkan() else 'bad',
                 'detail': 'Vulkan loader found.' if detect_vulkan() else 'vulkan-1.dll not found: update your graphics driver (Vulkan 1.3 is required).'})
    if flat:
        rows.append({'id': 'dlss', 'title': 'DLSS', 'status': 'na', 'detail': 'VR only.'})
    elif caps['dlss'].get('ok'):
        rows.append({'id': 'dlss', 'title': 'DLSS', 'status': 'ok', 'detail': 'Available: the quality presets use it.'})
    elif any(g.get('rtx') for g in gpus):
        rows.append({'id': 'dlss', 'title': 'DLSS', 'status': 'warn', 'detail': str(caps['dlss'].get('reason') or 'Files missing') + ' FSR 1 is used instead.'})
    else:
        rows.append({'id': 'dlss', 'title': 'DLSS', 'status': 'info', 'detail': 'Needs an NVIDIA RTX GPU. The presets use FSR 1 instead.'})
    if not flat:
        rows.append({'id': 'vrdata', 'title': 'VR menu data', 'status': 'ok' if vr_data_built else 'info',
                     'detail': 'Built from your game folder.' if vr_data_built else 'Built from your game folder on the first start (a few minutes, once).'})
    return rows



# ---- launch builder ---------------------------------------------------------------------------

def parse_extra_env(text):
    out = {}
    for line in str(text or '').splitlines():
        m = re.fullmatch(r'\s*(BB_[A-Z0-9_]+)\s*=\s*(.*?)\s*', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


NR_SUB = ('dlssnr_passes', 'dlssnr_place', 'dlssnr_style', 'dlssnr_intensity', 'dlssnr_tone', 'dlssnr_structure', 'dlssnr_skin',
          'dlssnr_fovea', 'dlssnr_inner')
NO_NR = {'ok': False, 'reason': 'not checked', 'dir': None}


def _flag(v):
    return '1' if v else '0'


def build_flat_launch(values, paths):
    """The plan for the Flat view (the game on the monitor, no headset): none of the VR variables, hooks or control
    files, so the port runs as its flat self and keeps its own settings (bbport.ini, the in-game menu)."""
    v = dict(values)
    env = {'BB_GAME_DIR': str(paths['game_dir'])}
    exe = main_exe(Path(paths['port']), v.get('exe'))
    if exe:
        env['BB_PROBE'] = str(exe)
    if v.get('flat_fps') is not None:
        env['BB_FPS_LIMIT'] = str(v['flat_fps'])
    if not is_public():
        env.update(parse_extra_env(v.get('extra_env')))
    return {'env': env, 'exe': exe, 'hooks': [], 'patches': [], 'files': {'fp': {}, 'stereo': {}, 'iq': {}}, 'ui': {},
            'warnings': [], 'upscale': 'flat', 'view': 'flat'}


def build_launch(values, paths, caps=None, public=None, view='vr'):
    """The launch plan for effective `values`.

    paths: dict(out=<out dir>, fp, stereo, iq, assets, game_dir, port)
    caps: detect_all() result (or None: no hardware known; DLSS is then taken as unavailable)
    view: 'vr' (the headset) or 'flat' (the monitor, see build_flat_launch)
    Returns dict(env, exe, hooks, patches, files{fp,stereo,iq}, ui, warnings, upscale, view)."""
    if view == 'flat':
        return build_flat_launch(values, paths)
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
    nr = (caps or {}).get('dlssnr') or NO_NR
    if v.get('dlssnr') and not nr['ok']:
        warnings.append('DLSS-NR is not available (' + nr['reason'] + ') - off.')
        v['dlssnr'] = False
    env = {}
    e = env.__setitem__
    e('BB_GAME_DIR', str(paths['game_dir']))
    if exe:
        e('BB_PROBE', str(exe))
    e('BB_XR_AA', v['aa'] or 'fxaa')
    e('BB_XR_UPSCALE', upscale)
    e('BB_XR_SHARPEN', to_text(BY_ID['sharpen'], v['sharpen']) if v['sharpen'] is not None else '0.5')
    e('BB_VR', '1')
    e('BB_XR_MODE', v['mode'] or 'stereo')
    if (v['mode'] or 'stereo') == 'stereo':
        e('BB_STEREO_FULL', '1')  # full stereo: eye R rendered for real (the game's default, kept explicit)
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
    display = display_sizes(v, caps, upscale)
    if display:  # Display size: replaces Eye resolution; a render size other than 1080p is a startup patch
        v['eye_res'] = fmt_size(display['eye'])
        if not display['native']:
            e('BB_RENDER_RES', fmt_size(display['render']))
            e('BB_OUTPUT_RES', fmt_size(display['render']))
            if display['render'][0] * display['render'][1] > NATIVE_FRAME[0] * NATIVE_FRAME[1]:
                e('BB_DMEM_MB', '9152')  # the game faults at boot above 1080p without it (findings/30 5)
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
    if v.get('dlssnr'):
        e('BB_XR_DLSSNR', '1')
        for oid in NR_SUB:
            o = BY_ID[oid]
            e(o['target'][2], to_text(o, o['default'] if v.get(oid) is None else v[oid]))
    else:
        e('BB_XR_DLSSNR', '0')
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
            if o['id'] == 'item_wheel' and val is not None:
                val = WHEEL_MENUS if val else NO_WHEEL_MENUS
            if val is not None:
                ui[o['target'][1]] = val
    if upscale != 'dlss':  # the DLSS keys mean nothing then; keep them out of the file
        for k in ('dlss_quality', 'dlss_preset', 'dlss_sharpen', 'dlss_mask', 'dlss_mask_m'):
            files['iq'].pop(k, None)
    if not v.get('dlssnr'):  # only dlssnr=0 stays, so a running game switches it off
        files['iq']['dlssnr'] = '0'
        for k in NR_SUB:
            files['iq'].pop(k, None)
    return {'env': env, 'exe': exe, 'hooks': hooks, 'patches': patches, 'files': files, 'ui': ui,
            'warnings': warnings, 'upscale': upscale, 'display': display, 'view': 'vr'}


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
    if plan.get('view') == 'flat':  # the flat game reads none of them
        return changed
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


def default_lobby(port):
    """The lobby that comes with a release (defaults/release.json "default_lobby", injected by make_release.py
    --default-lobby; never in the source tree). '' when the package has none."""
    return str(release_defaults(port).get('default_lobby') or '').strip().rstrip('/') if port else ''


def lobby_address(values, config, user_dir, port=None):
    """The lobby to use: env BBVR_LOBBY_URL, the player's online_server setting, the saved room's server, the
    config's online_server, and only then the release's default lobby (defaults/release.json)."""
    room = _read_json(Path(user_dir) / 'online_room.json')
    own = os.environ.get('BBVR_LOBBY_URL') or values.get('online_server') or room.get('server') or (config or {}).get('online_server')
    return str(own or default_lobby(port) or '').strip().rstrip('/')


def online_env(values, config, user_dir, public=None, solo=False, port=None):
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
    lobby = lobby_address(values, config, user_dir, port)
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


def dlss_note_text(caps):
    nr = caps.get('dlssnr') or NO_NR
    return DLSS_NOTE + ' DLSS-NR (experimental): ' + ('available.' if nr['ok'] else 'not available. ' + nr['reason'])


def page_state(settings, caps, public=None):
    """JSON for the page. Online options are left out in a public build."""
    public = is_public() if public is None else public
    opts = []
    for o in visible_schema(public):
        d = {k: v for k, v in o.items() if k not in ('emit', 'target')}
        d['target'] = o['target'][0]
        d['live'] = is_live(o)
        d['page'], d['section'] = PAGE_OF.get(o['id'], ('advanced', 'Other'))
        opts.append(d)
    values = {k: v for k, v in effective_values(settings, caps).items() if not (public and BY_ID[k].get('online'))}
    runtimes = caps['runtimes']
    shown = {o['id'] for o in opts}
    pages = [{'id': p['id'], 'label': p['label'], 'vr': bool(p.get('vr')),
              'sections': [{'title': t, 'options': [i for i in ids if i in shown]} for t, ids in p.get('sections', [])]}
             for p in PAGES]
    for p in pages:  # a section with no option left (the private ones in a public build) is dropped
        p['sections'] = [sec for sec in p['sections'] if sec['options']]
    upscale = values.get('upscale') or 'fsr1'
    if upscale == 'dlss' and not (caps.get('dlss') or {}).get('ok'):
        upscale = 'fsr1'
    det = caps.get('headset') or {}
    return {
        'public': public, 'online_play': ONLINE_PLAY, 'tabs': [{'id': t, 'label': l} for t, l in TABS], 'options': opts, 'values': values,
        'profile': settings['profile'], 'profiles': PROFILE_ORDER, 'profile_notes': PROFILE_NOTES,
        'show_menu': settings.get('show_menu', True),
        'mode': settings.get('mode', 'vr'), 'coop_view': settings.get('coop_view', 'vr'), 'view': play_view(settings),
        'modes': [{'id': m, 'label': MODE_LABELS[m], 'note': MODE_NOTES[m]} for m in MODES],
        'quality': settings.get('quality', QUALITY_DEFAULT), 'quality_modified': modified_preset_keys(settings),
        'quality_table': preset_table(caps), 'overrides': sorted((settings.get('overrides') or {})),
        'pages': pages,
        'caps': {'gpus': caps['gpus'],
                 'dlss': {k: (str(v) if isinstance(v, Path) else v) for k, v in caps['dlss'].items()},
                 'dlssnr': {k: (str(v) if isinstance(v, Path) else v) for k, v in (caps.get('dlssnr') or NO_NR).items()},
                 'runtimes': runtimes['runtimes'], 'active_runtime': runtimes['active']},
        'dlss_note': dlss_note_text(caps), 'runtime_warning_text': PIMAX_STEAMVR_WARNING,
        # Play page "Display size": what Auto found and the sizes the current values give (null: game default)
        'display': {'detected': dict(det, runtime_label=RUNTIME_LABELS.get(det.get('kind') or '', '')),
                    'sizes': display_sizes(values, caps, upscale)},
    }
