# Crash report server

Receives the crash reports that `run.py` (scripts/crash_report.py) uploads from testers who agreed.
One file, Python standard library only (3.9+), no framework to patch. It is separate from the co-op
server code and has no connection to it.

## Run

```
export BB_CRASH_UPLOAD_TOKEN=<random, 16+ chars>   # goes into the testers' crash_config.json
export BB_CRASH_ADMIN_TOKEN=<another random>       # for the list page only
python server.py --port 8787 --data ./data --retention-days 60
```

Docker:

```
docker build -t bb-crash tools/crash_server
docker run -d --restart=always -p 127.0.0.1:8787:8787 -v bbcrash:/data \
  -e BB_CRASH_UPLOAD_TOKEN=... -e BB_CRASH_ADMIN_TOKEN=... bb-crash
```

Settings (env or flag): `BB_CRASH_DATA`/`--data`, `BB_CRASH_RETENTION_DAYS` (default 60, 0 = keep
forever; swept every 6 hours), `BB_CRASH_MAX_BYTES` (20 MiB), `BB_CRASH_RATE` (uploads per client IP
per hour, 30), `--trust-proxy` (use X-Forwarded-For from the reverse proxy for the rate limit; only
behind a proxy that sets it; the Dockerfile enables it, so publish the port on 127.0.0.1 only).

## TLS (required: the upload token travels in a header)

Caddy (automatic certificates):

```
crash.example.com {
    request_body {
        max_size 21MB
    }
    reverse_proxy 127.0.0.1:8787
}
```

Tester config (`<user dir>/crash_config.json`, or env `BB_CRASH_SERVER_URL` / `BB_CRASH_TOKEN`):

```
{"crash_reports": "ask", "crash_server_url": "https://crash.example.com", "upload_token": "<upload token>"}
```

The client only accepts `https://` (plain `http://` just for localhost). The upload token ships with
the build, so it is a spam filter, not a secret: keep the size limit and the rate limit on.

## Endpoints

- `POST /upload`: multipart, field `report` = zip (`meta.json`, `log_tail.txt`, `crash0/crash.json`,
  `crash0/crash.dmp`). Stored as `data/<build>/<date>/<id>.zip`; `data/index.jsonl` gets one line
  (build, time, kind, fault module + offset, first error line).
- `GET /`: HTML list grouped by build, then by fault signature (module+offset), most frequent first.
  Admin token as `Authorization: Bearer`, `?token=`, or the password of the browser's login box.
- `GET /report/<id>.zip`: download (admin token). `GET /health`: no auth.

## Reading a dump

Unzip a report. `crash0/crash.json` already has the fault module+offset, registers and a
frame-pointer backtrace. For the dump:

- WinDbg: open `crash.dmp`, `.sympath C:\path\to\pdbs`, `.reload`, `!analyze -v`. Keep the `.pdb`
  that the build writes beside `bb-probe.exe` (`bb-probe.pdb`) for every released build, filed by
  the build id from `meta.json`. It covers the loader and runtime (C); the GPU library carries
  DWARF only, so its frames show as module+offset (resolve with `llvm-symbolizer` against the
  matching exe).
- Linux: `minidump-stackwalk crash.dmp ./symbols` (rust-minidump). Guest frames (`guest+0x...`)
  are offsets into the PS4 game image, not host code.

## Privacy

Reports are scrubbed on the player's PC (user name and folders, IPs, PSN ids, tokens) before upload
and hold no game files. Treat the data folder as sensitive anyway and set a retention you can defend.
