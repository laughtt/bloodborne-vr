# Crash reports

When the game crashes or hangs, it can write a small report that helps fix the bug. Nothing is sent
without your agreement, and you can turn all of it off.

## What is collected

Only when the game ends abnormally (a crash, a nonzero exit code, or a hang if you enabled the
hang watchdog):

- a small Windows crash dump (`crash.dmp`: the call stacks and the list of loaded modules, no full
  memory, no screenshots);
- `crash.json`: the build id, the address and module where it failed, the CPU registers, a short
  call chain;
- the last 500 lines of the game's console log;
- the exit code and the time.

It never includes game files, saves, your PS4 content, or your configuration files.

## What is removed before sending

On your PC, before anything leaves it: your Windows user name and `C:\Users\<name>` folders
(replaced with `%USER%`), your PC name, any IP address, PSN / online ids, account ids and
tokens. The user name is also overwritten inside the crash dump. No scrubber is perfect, so the
report is saved on your PC first (`<user folder>\crashes\<time>\report.zip`); open it if you want
to check.

## Consent

The first time the game crashes you are asked once, on the console:
"Send crash reports automatically from now on? [y/N]". Your answer is saved in
`<user folder>\crash_config.json` as `crash_reports`:

- `ask` (default): ask at the next crash. If there is no console window, nothing is sent.
- `on`: send automatically after a crash, to the server in `crash_server_url`.
- `off`: no reports are built, sent, or even written by the game.

Reports go only to the server address in `crash_server_url` (https). If it is empty, the report just
stays on your PC and the game tells you where.

## Turning it off

Any one of these:

- set `"crash_reports": "off"` in `<user folder>\crash_config.json`;
- set the environment variable `BB_CRASH_REPORTS=off`;
- to also stop the game from writing local crash dumps and to leave the console output untouched,
  that is the same setting (`off`); `BB_CRASH_CAPTURE=0` stops only the dump files.

To delete what exists, remove the `<user folder>\crashes` folder.

## Options (for testers and developers)

| Setting | Meaning |
|---|---|
| `crash_config.json` `crash_server_url`, `upload_token` | where and with which token to send (env: `BB_CRASH_SERVER_URL`, `BB_CRASH_TOKEN`) |
| `BB_HANG_SECONDS=<n>` | write a report if no frame was shown for n seconds (off by default) |
| `BB_LOG_TEE=0` | do not keep the log tail (the game's output goes straight to the console) |
| `BB_TEST_CRASH=1` / `hang` | deliberately crash (or stall) 8 seconds after start, to test the report pipeline |

The server that receives reports is in `tools/crash_server/`. Sending is limited to a few seconds
and never delays closing the game for longer than that.
