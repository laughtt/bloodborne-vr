# Public build with the online client

Decision: public release builds ship the online CLIENT. "Play online" works in the public zip. The room server never ships.

## Build switches (gpu/CMakeLists.txt)

Two independent switches replace the old single `BBPORT_ONLINE` meaning:

| Switch | Default | Macro | Meaning |
|---|---|---|---|
| `BBPORT_ONLINE` | ON | `BB_NO_ONLINE=1` when OFF | the online client: shadNet login, NP / Matching2 / signaling, HTTP and SSL libraries, room bells |
| `BBPORT_PUBLIC` | OFF | `BB_PUBLIC_ALPHA=1` when ON | a public build: dev tools compiled out |

* Public release: `-DBBPORT_ONLINE=ON -DBBPORT_PUBLIC=ON` (output dir `out/gpu-public-online`).
* Private dev build: both defaults (online, dev tools).
* `BBPORT_ONLINE=OFF` alone is the old single-player public variant (implies public). `make_release.py --variant offline` checks it.
* `BB_PUBLIC_ALPHA` now means only "no dev tools". Code that is the online client is guarded by `BB_NO_ONLINE`.
* Compiled out of public builds: `BB_DEBUG_GIVE` / `BB_DEBUG_USE` (and the give of anything but one bell), the speffect use file, `BB_NET_TRACE`, `BB_NET_BREAK` and the net state monitor, insight changes. The room bells (auto-bell, bell give, free bells) stay.
* No address is built in: `GetShadNetServer()` is empty unless `BB_SHADNET_SERVER` / `BB_ONLINE_SERVER` is set (the launcher passes the player's `online_server` setting), the WebAPI address is derived from it, and the default ports are gone from the client code.
* `gpu/xr_bridge` keeps `BBPORT_ONLINE=OFF` for the public bridge: its flag only controls the sandbox pose sync, which is not part of the game.

## Export (tools/release/export_public.py)

The online client sources (`gpu/bbnet`, `gpu/shadps4/shadnet`, `np`, `network`, ...) are exported (the exe contains them, GPL). Still denied: `online/`, dev tool sources, `scripts/online_hosts.py` (replaced by an address-free overlay in `tools/release/public/scripts/`). `BB_PUBLIC_ALPHA` blocks are resolved away, `BB_NO_ONLINE` blocks kept, and the export's `gpu/CMakeLists.txt` gets `set(BBPORT_ONLINE ON)` / `set(BBPORT_PUBLIC ON)`.

## Release checks (tools/release/online_checks.py)

Run by `make_release.py` on the exe (before staging) and on the staged package, by `verify_install.py` on the installed files, and alone: `make_release.py --exe X --check-only`.

* Reject: server code names and private paths, online/ folder or server files in the package; server host names; IPv4 literals that are not loopback / broadcast / multicast / netmask / public DNS; any host:port or ip:port; the known server ports as numbers; dev test account names and default passwords, `BB_SHADNET_PASSWORD=<value>`; dev tools (`BB_DEBUG_GIVE`, `BB_DEBUG_USE`, `BB_NET_TRACE`, `BB_NET_BREAK`, speffect).
* Require (online variant): shadNet client, Matching2, HTTP libraries, the server setting variable, and the four room bell variables.
* Reject (public builds): the maintainer source path compiled into the exe (gpu/CMakeLists.txt adds `-ffile-prefix-map`, `-fmacro-prefix-map`, `-fdebug-prefix-map` for public configurations).
* `make_release.py` also writes `PUBLIC_BUILD = True` into the shipped `bbvr_settings.py`, so dev options stay hidden.

## Game-server redirect from the lobby

The player enters ONE address (the lobby). Before an online launch the launcher (`bbvr_launcher.py` -> `bbvr_online.sync_host_overrides`) asks `GET <lobby>/bbvr/v1/hosts`. The answer is `{"ok": true, "hosts": {"<address the game asks>": "<address to use>"}}` (or the bare mapping). It is validated (at most 32 entries and 8 KB; keys `*`, `host[:port]` or `http(s)://host[:port][/path]`; values `http(s)://host[:port]` only; no credentials, queries or spaces), saved as `user/host_overrides.lobby.json` and handed to the game as `SHADPS4_HTTP_HOST_OVERRIDES_JSON`. A player's own `user/host_overrides.custom.json` always wins and is never touched. A lobby without the endpoint (or an invalid or unreachable answer) keeps the old behaviour: nothing is redirected, a log line says so, and a stale lobby file is removed. The room server side is private (`online/`, not committed).
