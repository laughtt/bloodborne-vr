# SPDX-License-Identifier: GPL-2.0-or-later
"""Update check of the dashboard: asks the GitHub releases API (no login) whether a newer version exists. Standard
library only. Never raises and never shows an error: a private / missing repo (HTTP 404), no network or a
malformed answer all give status "unavailable" and the dashboard just says so quietly.

The address comes from defaults/release.json ("update_api_url", "update_page_url"; make_release.py --update-repo),
or the BB_UPDATE_API_URL environment variable. The user can switch the check off ("update_check": false in
bbvr_config.json; the dashboard's Options page has the switch). The last answer is cached in update_check.json
(data folder) so every state the dashboard reads carries it.
"""
import json
from pathlib import Path
import re
import time
import urllib.request

DEFAULT_REPO = 'laughtt/bloodborne-vr'
TIMEOUT = 6
RANK = {'dev': 0, 'pre': 0, 'a': 1, 'alpha': 1, 'b': 2, 'beta': 2, 'rc': 3}


def api_url(defaults, env=None):
    import os
    env = os.environ if env is None else env
    if env.get('BB_UPDATE_API_URL'):
        return env['BB_UPDATE_API_URL']
    url = defaults.get('update_api_url') or ''
    repo = defaults.get('update_repo') or DEFAULT_REPO
    return url or f'https://api.github.com/repos/{repo}/releases'


def page_url(defaults):
    repo = defaults.get('update_repo') or DEFAULT_REPO
    return defaults.get('update_page_url') or f'https://github.com/{repo}/releases'


def parse_version(text):
    """Sortable key of '0.1.0-alpha5' / 'v0.1.0' / '0.1.0-alpha5-pre': a final release beats its alphas,
    alpha < beta < rc, and a '-pre' / '-dev' build is older than the same version without it."""
    text = (text or '').strip().lstrip('vV')
    head, _, tail = text.partition('-')
    nums = tuple(int(n) for n in re.findall(r'\d+', head)[:4])
    nums += (0,) * (4 - len(nums))
    stages, prebuild = [], 1
    for word, number in re.findall(r'([a-zA-Z]+)(\d*)', tail):
        rank = RANK.get(word.lower())
        if rank == 0:
            prebuild = 0
        elif rank:
            stages.append((rank, int(number or 0)))
    return nums, 0 if tail and stages else 1, tuple(stages), prebuild


def is_newer(latest, current):
    return parse_version(latest) > parse_version(current)


def pick_latest(releases):
    """(tag, url) of the newest published release in a releases API answer, or None."""
    best = None
    for rel in releases if isinstance(releases, list) else []:
        if not isinstance(rel, dict) or rel.get('draft'):
            continue
        tag = str(rel.get('tag_name') or '')
        if not tag:
            continue
        if best is None or parse_version(tag) > parse_version(best[0]):
            best = (tag, str(rel.get('html_url') or ''))
    return best


def fetch(url, timeout=TIMEOUT):
    request = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'BloodborneVR-update-check'})
    with urllib.request.urlopen(request, timeout=timeout) as response:  # noqa: S310 (https only, checked by the caller)
        return json.loads(response.read(2_000_000).decode('utf-8'))


def check(current, defaults, fetcher=None, env=None):
    """{'status': 'newer'|'current'|'unavailable', 'latest', 'url', 'checked'} - never raises."""
    result = {'status': 'unavailable', 'latest': '', 'url': page_url(defaults), 'checked': int(time.time())}
    url = api_url(defaults, env)
    if not url.lower().startswith('https://'):
        return result
    try:
        found = pick_latest((fetcher or fetch)(url))
    except Exception:  # noqa: BLE001 - 404 on a private repo, offline, timeouts, bad JSON: all just "unavailable"
        return result
    if not found:
        return result
    tag, link = found
    result.update(latest=tag.lstrip('vV'), url=link or result['url'],
                  status='newer' if is_newer(tag, current) else 'current')
    return result


def cache_path(data):
    return Path(data) / 'update_check.json'


def load_cache(data):
    try:
        value = json.loads(cache_path(data).read_text(encoding='utf-8'))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def save_cache(data, result):
    try:
        Path(data).mkdir(parents=True, exist_ok=True)
        cache_path(data).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    except OSError:
        pass


def summary(enabled, current, defaults, data):
    """The 'update' part of the dashboard state. The cached answer is re-compared with the running version."""
    cached = load_cache(data) if enabled else {}
    status = cached.get('status') if cached.get('status') in ('newer', 'current', 'unavailable') else 'unchecked'
    if status in ('newer', 'current') and cached.get('latest'):
        status = 'newer' if is_newer(cached['latest'], current) else 'current'
    return {'enabled': bool(enabled), 'status': status if enabled else 'disabled', 'latest': cached.get('latest', ''),
            'url': cached.get('url') or page_url(defaults)}
