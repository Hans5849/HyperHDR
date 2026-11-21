# Release-candidate acceptance run (CPU-only HDMI → HyperHDR → DDP → WLED)

The one procedure that decides whether a release candidate works on the Orange
Pi 5 Plus. It strings together the existing tools and checklist rows: the
details, commands and pass criteria are in the
[operator checklist](rk3588-operator-checklist.md) (rows A–C) and the
[operator guide](rk3588-hardware-validation.md). Nothing here runs by itself:
every board workflow run is dispatched and approved by the owner, and nothing
counts as passed without its evidence on the issue.

**Who does what**

| Mark | Meaning |
|---|---|
| 🧑 **Owner at the board** | Needs a person: cables, the source, the TV, the LEDs, or `sudo`. |
| 🖥 **Unattended** | Read-only, can run while you do something else (or be dispatched remotely after approval). Doesn't change the picture or interrupt capture. |
| ⛔ **Maintenance window** | Stops HyperHDR or needs the receiver exclusively. Only when you've planned for it. |

Set once per session: `svc_user=ryan` (the service user), and the receiver node
with the command at the top of the checklist (`RX=...`). Record everything in one
folder, `run=~/acceptance-$(date +%Y%m%d)`, and attach it to the issues in step 10.

**CI prerequisites.** Before this run, the candidate SHA must pass the release gates in [fork-scope.md](fork-scope.md#release-requirements): repository policy, the x86_64 and arm64 sanitizer suites, CodeQL, and the Debian 13 packages. Hosted CI does not replace any step below.

## 1. Record what is under test 🖥

`hyperhdr --version` (commit), `dpkg-query -W hyperhdr`, `uname -r`,
`cat /etc/os-release`, the receiver identity (checklist A2 summary: driver,
bus, `v4l2:snps_hdmirx@...`), and HyperHDR's configuration (export the settings
from the web UI, or copy `~/.hyperhdr` from the backup in step 2). The commit
must be the release-candidate master commit; every later result names it.

## 2. Back up and confirm the rollback 🧑 (stops the service for seconds)

[Back up before installing](rk3588-hardware-validation.md#back-up-before-installing),
then check the backup as described under
[Roll back](rk3588-hardware-validation.md#roll-back): the saved `.deb` has the
installed version and the archive lists `.hyperhdr/`. Then install the release
candidate ([Install and run the service](rk3588-hardware-validation.md#install-and-run-the-service)).

## 3. Read-only diagnostics and receiver probes 🖥

1. Doctor (A3): `scripts/diagnostics/hyperhdr-doctor.sh --user "$svc_user"`.
2. Board workflow `stage: tests` (A1), then `stage: probe` with `expbuf: false`
   and **the source on** (A2), each verified with `verify-board-artifact.py`
   ([session procedure](rk3588-board-session.md)). Or on the board, without the
   workflow: `scripts/rk3588/collect-evidence.sh` (read-only without `--stream`).
3. Receiver state (A4) and range evidence (A6).

## 4. Live preview at 1080p and each supported 4K mode 🧑

With the source at 1080p60, then each 4K mode the receiver reports in
`--query-dv-timings`: HyperHDR's preview is live, matches the TV, and the probe's
`signal` lines show source timing and configured format agreeing (step 3).

## 5. Orientation, channel order and RGB range 🧑

C1 (orientation and channel order) and C2 (full, limited and automatic source
range), with `color-patterns.py check` on a saved preview. `RESULT: PASS` in
every setting. The range rules are in the
[operator guide](rk3588-hardware-validation.md#pending-hardware-validation) (item 5).

## 6. DDP output and the LEDs 🧑 + 🖥

C5: the edge pattern, each solid colour, then video; photograph the strip. At the
same time (unattended): A8, packets on UDP 4048 (`ddp`) or 21324 (`wled`), and A9,
`LED output stats`. A correct preview alone does not validate DDP.

## 7. Rates and resources 🖥

With the V4L2 grabber and the LED device at debug log level, 15 minutes of video,
then `scripts/rk3588/stats-align.py` on the journal (A5 + A9): capture and LED
rates side by side, never combined. Plus A7 (CPU, RSS, fds, maps) and the SoC
temperatures (`cat /sys/class/thermal/thermal_zone*/temp`) at the start and end.

## 8. No signal, reconnect and recovery 🧑

C3: unplug/replug, source off/on, timing changes (1080p ↔ 4K), ten times each,
plus one HyperHDR start with the source off. Expected log lines and the
identity-only criterion are in C3. A7 again afterwards.

## 9. 30-minute stability 🧑 then 🖥

C4: at least 30 minutes of continuous video with a C3 event every ~10 minutes
(the events need you; the rest runs unattended). A7 and temperatures before and
after; stats every 60 s; no `ACCOUNTING MISMATCH`.

## 10. Attach the evidence 🖥

| Evidence | Issue |
|---|---|
| Steps 1–3: versions, doctor output, verified `tests` and `probe` artifacts | #27, #28 |
| Steps 4–6: preview screenshots, `color-patterns.py` output per range setting, LED photos | #8, #10 |
| Step 7: `stats-align.py` output, A7, temperatures | #9, #12 |
| Steps 8–9: recovery logs, A7 before and after, soak log | #11, #19 |
| Anything that failed, with the log lines | the issue for that step, and #7 |

⛔ Not part of acceptance: B1/B2 (kernel stream rate, DMA-BUF export, `expbuf:
true`) need HyperHDR stopped and a planned maintenance window. Optical latency
([plan](rk3588-latency-measurement.md)) needs sensors; it is a measurement for #9,
not a pass/fail gate for the CPU-only release.

## Done means

All of 1–9 passed with evidence attached, on one commit. Until then the release
candidate is **ready for validation**, not validated.
