# SPDX-License-Identifier: GPL-2.0-or-later
"""Host overrides for online play (run.py imports this when BB_ONLINE=1).

This public version has no server address in it. The lobby the player enters serves the redirects of the game's own
HTTP calls (GET <lobby>/bbvr/v1/hosts); the launcher saves them as user/host_overrides.lobby.json before an online
launch (tools/release/zip/bbvr_online.py sync_host_overrides). A player's own user/host_overrides.custom.json
({"<address the game asks>": "<address to use>"}) wins and is handed to the HTTP library as it is. Without either
nothing is redirected, and the room flow (shadNet login, matchmaking, signaling) uses the lobby address alone.
"""


def online_overrides(env, user_dir):
    custom = user_dir / 'host_overrides.custom.json'
    if custom.exists():
        env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(custom)
