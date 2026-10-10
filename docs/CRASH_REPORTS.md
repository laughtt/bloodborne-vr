# Crash reports

When the game crashes, the launcher saves a small report on your PC. **Nothing is ever sent
automatically** and there is no setting, server address or upload code: if you want to help, you
send the file to the developer yourself.

## Where it is

`%LOCALAPPDATA%\BloodborneVR\crashes\<date-time>-<id>\report.zip`

This is a private per-user folder for both the Setup.exe install and the portable zip (it is not in
the install folder, Desktop or Documents). The dashboard's About tab shows the path, the number of
reports and has an "Open crash reports folder" button. After a crash the dashboard (and the console
launcher) says: "The game crashed. A crash report was saved to <path>. If you want to help, send that
file to the developer." with an "Open folder" button.

## What a report holds

Only when the game ends abnormally (a crash, a nonzero exit code, or a hang if you enabled the hang
watchdog):

- a small Windows crash dump (`crash.dmp`: the call stacks and the list of loaded modules, no full
  memory, no screenshots);
- `crash.json`: the build id, the address and module where it failed, the CPU registers, a short
  call chain;
- `log_tail.txt`: the last 500 lines of the game's console log;
- `meta.json`: the exit code and the time.

It never includes game files, saves, your PS4 content, or your configuration files.

## What is removed

Before the report is written: your Windows user name and `C:\Users\<name>` folders (replaced with
`%USER%`), your PC name, any IP address, PSN / online ids, account ids and tokens. The user name is
also overwritten inside the crash dump. No scrubber is perfect, so open the zip and look before you
send it. The raw dump the game writes under `user\crashes` is deleted once the scrubbed report is saved.

## Deleting

Delete the report folders (or the whole `crashes` folder) any time; the Setup.exe uninstaller keeps
them (it keeps all your data), delete them by hand if you want them gone.

## Options (for testers and developers)

| Setting | Meaning |
|---|---|
| `BB_CRASH_DIR=<folder>` | save reports here instead of `%LOCALAPPDATA%\BloodborneVR\crashes` |
| `BB_CRASH_CAPTURE=0` | the game does not write crash dumps at all |
| `BB_HANG_SECONDS=<n>` | write a report if no frame was shown for n seconds (off by default) |
| `BB_LOG_TEE=0` | do not keep the log tail (the game's output goes straight to the console; no report is built) |
| `BB_TEST_CRASH=1` / `hang` | deliberately crash (or stall) 8 seconds after start, to test the report pipeline |

`tools/crash_server/` is a leftover from an earlier design in which reports could be uploaded; no
client code uses it any more.
