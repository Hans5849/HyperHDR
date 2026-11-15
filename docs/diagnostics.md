# Installation and diagnostics checks

`scripts/diagnostics/hyperhdr-doctor.sh` checks whether a Linux system is ready
for HyperHDR and whether an installation is healthy (issue #27). It prints one
line per check with a fix hint:

```
[PASS] svc.active: hyperhdr@ryan.service is active (enabled: enabled)
[FAIL] grp.video: ryan is not in group video -> sudo usermod -aG video ryan, then restart the service
[WARN] capture.signal: /dev/video1: no source detected (power_present=0) -> connect and power on the HDMI source; HyperHDR waits and starts automatically
```

## Running it

```sh
scripts/diagnostics/hyperhdr-doctor.sh                     # all checks
scripts/diagnostics/hyperhdr-doctor.sh --checks install    # before or after installing
scripts/diagnostics/hyperhdr-doctor.sh --led-host 192.168.1.60   # also check a WLED controller
scripts/diagnostics/hyperhdr-doctor.sh --json              # machine-readable
scripts/diagnostics/hyperhdr-doctor.sh --archive .         # redacted tarball to attach to an issue
```

| Option | Meaning |
| --- | --- |
| `--checks LIST` | Comma-separated: `install`, `runtime`, `capture` or `all` (default) |
| `--user NAME` | Service user. Default: the `hyperhdr@NAME` unit (loaded or enabled), then the running process's owner, then the current user |
| `--userdata DIR` | User data directory. Default: `~NAME/.hyperhdr` |
| `--since 30min` | Journal window for the log checks |
| `--led-host ADDRESS` | Ping it and query WLED's `/json/info`. The only check that contacts another machine |
| `--db-check` | Run a read-only `PRAGMA quick_check` on the database (needs `sqlite3`) |
| `--json` | Print a JSON report instead of the checklist |
| `--archive DIR` | Also write `hyperhdr-doctor-<UTC>.tar.gz`: the checklist, JSON and raw evidence |
| `--no-color` | Plain output (automatic when not writing to a terminal) |

Exit status: `0` if there is no FAIL, `1` if there is at least one FAIL, `2` for a usage error.

**It only reads.** It never changes services, packages, groups, device settings or
firewall rules, never streams from a capture device, and never reads the settings
database except through `--db-check`'s read-only integrity check. `sudo` is not
needed: when a check can't see something without root, it reports SKIP or says
what is not visible.

**Output is redacted.** IPv4, IPv6 and MAC addresses, the hostname and the service
user name are redacted in `--json` and `--archive` output (shared with
`scripts/rk3588/collect-evidence.sh`). The checklist printed to the terminal is not
redacted. Review an archive before sharing it.

Missing optional tools (`v4l2-ctl`, `ss`, `curl`, `ldd`, `fuser`, `pgrep`,
`journalctl`, `sqlite3`) turn their checks into SKIP with an install hint, never
into FAIL.

## Checks

### install

| Id | Passes when | Otherwise |
| --- | --- | --- |
| `os.release` | `/etc/os-release` is Debian/Ubuntu, Fedora/RHEL or Arch family | WARN: no package for this distribution; build from source |
| `os.arch` | x86_64, aarch64, armv7l or armv6l | WARN: no package for this architecture |
| `kernel.version` | Always (informational) | |
| `sys.systemd` | systemd is the init system | WARN: `hyperhdr@<user>` can't be used |
| `sys.disk` | 200 MB or more free for the user data directory | WARN under 200 MB, FAIL under 100 MB |
| `sys.memory` | 256 MB or more available | WARN |
| `pkg.installed` | The `hyperhdr` package is installed (dpkg or rpm) | WARN: not installed, or a source install |
| `bin.version` | `hyperhdr --version` prints a version (exits without starting the daemon) | FAIL: often a missing library |
| `bin.libraries` | `ldd` resolves every shared library | FAIL: names the missing libraries |
| `pkg.version` | The package `Version` matches the application version, prerelease included (`24.0.0~beta.0`, or CI's `24.0.0~trixie~beta0`) | WARN: a pre-#20 package (`24.0.0` for a beta) needs `--allow-downgrades` once; other mismatches mean reinstall |

### runtime

| Id | Passes when | Otherwise |
| --- | --- | --- |
| `svc.active` | `hyperhdr@<user>` is active | FAIL if failed; WARN if inactive, with the start or enable command |
| `svc.restarts` | No automatic restarts | WARN: it is crashing or exiting |
| `proc.count` | Exactly one `hyperhdr` process | WARN if none; FAIL if more than one, since a second instance refuses to start |
| `data.dir` | The user data directory is owned by and writable for the service user | WARN if missing; FAIL with the `chown`/`chmod` fix |
| `data.db` | `db/hyperhdr.db` exists and is owned by the service user (`--db-check`: `quick_check` is `ok`) | WARN if missing; FAIL on wrong owner or a failed check |
| `data.lut` | `lut_lin_tables.3d` is in the user data directory or `/usr/share/hyperhdr/lut` | WARN: needed for HDR tone mapping |
| `net.port.8090`, `.8092`, `.19444`, `.19400` | The web, HTTPS, JSON and flatbuffers ports are held by `hyperhdr` | FAIL if another program holds a port; WARN if it isn't listening while HyperHDR runs |
| `net.webui` | `http://127.0.0.1:8090/` answers | WARN |
| `log.errors` | No `<ERROR>`/`<WARNING>` lines in the journal window | WARN with counts and the first error |
| `log.capture.no_signal`, `.timeout`, `.initialization_failed`, `.streaming_failed` | Only listed when such V4L2 recovery lines exist | WARN with what that category means |

### capture

| Id | Passes when | Otherwise |
| --- | --- | --- |
| `grp.video` | The service user is in `video` (when `/dev/video*` exists) | FAIL with the `usermod` fix |
| `grp.dialout` | In `dialout` (only listed when `/dev/ttyUSB*`/`ttyACM*` exist) | WARN: needed for serial LED controllers |
| `dev.spidev`, `dev.hidraw` | The device group is one of the user's groups (only when present) | WARN: needed for that LED type only |
| `capture.node.videoN` | One line per capture-capable node: driver, single-planar or MPLANE, `v4l2:driver@bus` identity. Uses the node's Device Caps, so codec, scaler and metadata nodes are excluded | |
| `capture.nodes` | | WARN if no node is capture-capable |
| `capture.autoselect` | Exactly one capture device | WARN: with `auto`, the choice depends on name order (#11); save the device explicitly |
| `capture.signal` | An HDMI receiver reports a detected timing | WARN when there is no source (`power_present=0`) or the signal is unlocked or out of range. A warning, not a failure: HyperHDR waits and starts automatically |
| `capture.range` | Always (informational): format quantization and the receiver RGB-range control (#24) | |
| `capture.holder` | Which process holds the receiver | |
| `led.ping`, `led.wled` | With `--led-host`: the host answers and runs WLED (reports its version) | WARN |

## JSON

```json
{"schema":1,"tool":"hyperhdr-doctor","user":"<user>","user_source":"systemd unit","userdata":"/home/<user>/.hyperhdr",
 "summary":{"pass":20,"warn":3,"fail":0,"skip":1},
 "checks":[{"id":"svc.active","group":"runtime","status":"PASS","finding":"…","hint":"","evidence":"evidence/svc.active.txt"}]}
```

Check ids are stable; new checks get new ids. The `evidence` field names a file
inside an `--archive` tarball, or is empty.

## Tests

`tests/diagnostics/doctor-test.sh` runs every check against stub commands and
fixture trees in CaptureTests. Scenarios:
- a healthy no-signal Orange Pi
- a missing library
- a failed service
- wrong ownership
- a port taken by another program
- a user missing from `video`
- two instances
- pre-#20 and CI package versions
- USB only
- no optional tools

It also checks the options, JSON validity, archive redaction and `--db-check`. The
stubs log every call, so the test proves only read-only verbs were used. On the
board, the `probe` stage of the RK3588 board workflow (#26) runs the doctor and
uploads its archive.
