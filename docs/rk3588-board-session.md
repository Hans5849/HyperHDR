# Next board session: operator procedure

For the full release-candidate acceptance run (backup, preview, colour, LEDs,
recovery, 30-minute soak, evidence), see [acceptance run](rk3588-acceptance-run.md).
This page covers the board workflow stages and artifact verification in detail.

The exact steps for the next Orange Pi 5 Plus session. Issues: #28 (runner),
#27 (diagnostics), #9 and #12 (benchmark), #8 and #10 (capture and colour),
#11 and #19 (recovery), #18 (release readiness).

Nothing here runs automatically. Every board workflow run needs your manual
dispatch and your approval of the `rk3588-hil` environment. Results only count
once the artifact has passed verification (step 2) and the evidence is attached
to the issue. **Don't close #27 or #28 from this session alone.** Check the
issue's own acceptance criteria first (last table).

## Readiness (checked 2026-11-16, master `609be92`)

| Requirement | Status | How it's enforced |
|---|---|---|
| Routine CI is GitHub-hosted | Yes. Every workflow except `rk3588-board.yml` runs on hosted Ubuntu runners. This fork is Linux-only: the Windows and macOS CI jobs were removed on 2026-11-21 ([ci-scope.md](ci-scope.md)). The hardware-free suites run on hosted x86_64 **and native ARM64** | `tests/ci/board-workflow-policy-test.py` fails if any other workflow targets a self-hosted runner |
| Board workflow is manual only | `workflow_dispatch` only; `master` of `Hans5849/HyperHDR` only; token `contents: read` | same policy test |
| Environment and hardware labels required | `environment: rk3588-hil`; labels `self-hosted, Linux, ARM64, rk3588, orange-pi-5-plus, hyperhdr-hil`; the preflight refuses other boards, root and passwordless sudo | same policy test. **Whether `rk3588-hil` has required reviewers is a repository setting: confirm it in Settings → Environments** (#28) |
| New tests where expected | Colour references (#41), benchmark reporting (#40), recovery (#43) and the LED output counters (#45, once merged) run in hosted CI on both architectures. The board runs only the suites that need native timing or hardware context, as decided in #28 (2026-11-15): the board isn't general CI | policy test: every board suite also runs hosted |
| Benchmark artifacts keep their metadata | Each JSON records commit, label, compiler, build type, affinity and pinned CPU, MIDR and core, governor, min/max/current frequency, load and thermal before and after, configuration, raw samples. `metadata.json` records board, OS, firmware, toolchain and the workflow inputs; `cmake-flags.txt` records the build. Saved-frame input identity (SHA-256) is added by #46 | `tests/ci/board-workflow-policy-test.py`, `tests/ci/bench-summarize-test.py`, and #46's validator |
| Diagnostics read-only without EXPBUF | `expbuf` defaults to false. Without it, the evidence collector never streams or allocates buffers, and the doctor never opens a device for streaming | `tests/rk3588/collect-evidence-test.sh`, `tests/diagnostics/doctor-test.sh`; the artifact check below refuses a bundle with a stream or EXPBUF probe when `expbuf` was false |
| Artifact verification, tested with fixtures | `scripts/rk3588/verify-board-artifact.py`. It also runs at the end of every board job (`verify.txt` in the artifact) | `tests/ci/verify-board-artifact-test.py`: a complete artifact plus 20 single defects |

## Before you start

- [ ] `RK3588 board checks` workflow: the latest `master` contains the
  benchmark-path fix (`dd7caa0`) and the change that added this page.
- [ ] The repository settings below are confirmed (screenshot each for #28).
- [ ] Settings → Actions → Runners: `opi5plus-hyperhdr-01` is online.
- [ ] Note what else is running on the board (other runners, services). The
  benchmark summary flags load above 1.0, but it can't tell you why.

### Repository settings to confirm (not visible from the code)

The workflow file asks for these protections, but whether they are enforced is
set in GitHub, under Settings. **Nobody has inspected these from this repository
or from a Claude session.** Confirm each one yourself before the first approval:

| Setting | Where | Expected |
|---|---|---|
| Environment exists | Settings → Environments | `rk3588-hil` is listed. A job naming a missing environment creates it **without** protection, so check that it exists before the first run |
| Required reviewers | `rk3588-hil` → Deployment protection rules | **Required reviewers** on, with your account. Every run then waits for your approval |
| Deployment branches | `rk3588-hil` → Deployment branches and tags | **Selected branches and tags**, with only `master`. The workflow also refuses other refs, but this enforces it in GitHub |
| Environment secrets | `rk3588-hil` → Environment secrets | None needed. The workflow uses only the read-only `GITHUB_TOKEN` |
| Runner registration | Settings → Actions → Runners | `opi5plus-hyperhdr-01` is registered to **this repository only**, not to an organisation or another repository |
| Runner labels | the runner's page | exactly `self-hosted, Linux, ARM64, rk3588, orange-pi-5-plus, hyperhdr-hil`. No generic labels such as `ubuntu-latest` that another workflow could match |
| Workflow token | Settings → Actions → General → Workflow permissions | **Read repository contents** (the default token is read-only). "Allow GitHub Actions to create and approve pull requests" is off |
| Fork pull requests | Settings → Actions → General → Fork pull request workflows | **Require approval for all external contributors** |

What the repository itself enforces, which the policy test fails on if changed:

- **Triggers:** only `rk3588-board.yml` targets the board labels, and only
  through `workflow_dispatch`. No pull-request or push event can start it, so
  pull-request code never reaches the board.
- **Checks in the workflow:** the job checks that the ref is `master` of
  `Hans5849/HyperHDR`, uses no third-party actions, and has no `sudo`.
- **Preflight:** refuses other boards, root and passwordless sudo.

## 1. `stage: tests` (safe while HyperHDR runs)

Actions → **RK3588 board checks** → Run workflow:

- branch `master`
- `stage: tests`
- leave `expbuf` and `host_idle_confirmed` **off**

Then approve the environment.

The run occupies one A76 and one A55 core for a few minutes. HyperHDR's frame
timing may degrade in that window.

## 2. Download and verify the artifact

```sh
gh run download <run-id> -R Hans5849/HyperHDR -n rk3588-board-tests-<run-number> -D board-tests
python3 scripts/rk3588/verify-board-artifact.py --commit <sha> --hostname <runner host name> --user <board user> board-tests
```

`--commit` is the run's commit from the run page. Without it, the verifier warns
that the commit wasn't compared with an approved SHA. `--hostname` and `--user`
are the board's host and user names, which must not appear anywhere in the
artifact. The verifier can't recognise a host name it isn't given.

`RESULT: OK` means:

- the run came from master, on the Orange Pi 5 Plus;
- both cores exited 0 and name the same commit everywhere;
- each result is pinned to the CPU the preflight chose, and decoded as the right
  core;
- each pinned CPU is in the board's own cluster for that core (A76: 4–7,
  A55: 0–3 on this board);
- the compiler, a 64-bit ARM build and the kernel and toolchain are recorded;
- every statistic is recomputed from its raw samples, 24 cases per core;
- no tokens, keys, unredacted IPv4, IPv6 or MAC addresses, or the given host and
  user names were found.

`WARN baseline` lines (governor not `performance`, load above 1.0, frequency
change or unreadable, temperature at or above 80 °C or unreadable, an
unoptimised build or assertions) mean the numbers are **indicative, not a
baseline**. Say so when you post them. A `FAIL` means don't use the results.
Attach the verify output, and fix or report the cause first.

Attach to #12 and #9: `bgr24-bench-summary.md`, both JSON files and the
verifier output. Use the comparison from the
[benchmark reporting](rk3588-benchmark-reporting.md) hypotheses: are the
`random` and `gradient`/`solid` LUT medians within about 1.5x of each other?

For #28: link the run, and confirm the `tests` criterion with the artifact.

## 3. `stage: probe`, `expbuf: false` (read-only)

Same workflow:

- `stage: probe`
- `expbuf` **off**

Download and verify as in step 2, with `-n rk3588-board-probe-<run-number>`.
The verifier also checks that both archives are present and readable, and that
there's **no stream or EXPBUF probe** in the bundle. It also reads `receiver-signal.txt`:
the source timing (size, scan and frame rate, from `VIDIOC_QUERY_DV_TIMINGS`),
the configured capture format (`VIDIOC_G_FMT`), whether their sizes agree, and
the configured quantization. These are `WARN`s, never `FAIL`s: a probe with the
source off is valid for #27, but it isn't timing evidence for #8, and a
quantization of `Default (...)` is the driver's default, not evidence of the
source's RGB range (that comes from A6 and C2). It reports
`WARN probe EXPBUF not tested (read-only probe): this run is not EXPBUF evidence`,
so a read-only probe can't be cited as an EXPBUF result.

Attach to #27:

- `doctor.txt`, and the `hyperhdr-doctor-*.tar.gz` after reviewing it;
- a note on whether each result matches what you know about the board: service
  state, permissions, receiver node, signal.

If possible, run once with the source on and once with it off. #27 needs both
the healthy and the no-signal state.

Attach to #8 and #10: `receiver-signal.txt` and `evidence-summary.txt` (source
timing vs configured format, quantization).

## 4. Logs while capturing (safe)

With video playing for at least 15 minutes, use checklist rows:

- **A5:** `capture stats`
- **A6:** range evidence
- **A7:** resources, now and again after step 5
- **A9:** `LED output stats`

Set the V4L2 grabber's and the LED device's log level to debug first: healthy
intervals of both lines are logged at debug level. Then pair them offline:

```sh
journalctl -u hyperhdr@ryan -o short-iso-precise --since "-20 min" > journal.txt
python3 scripts/rk3588/stats-align.py journal.txt
```

It prints each capture interval next to the LED interval that covers the same
time, per LED device, and names the driver's transport (`ddp`: DDP on UDP 4048;
`wled`: WLED realtime UDP, 21324 by default). Post that output. The two are
independent rates and aren't a pipeline throughput (see
[frame stages](rk3588-frame-stages.md)). The tool reports capture-to-LED latency
and WLED receipt as not measured.

## 5. User-visible checks (the TV picture changes)

Checklist rows **C1, C2, C5** with the
[colour reference patterns](rk3588-color-patterns.md):

- one `check` run per source RGB-range setting
- LED photos

Then **C3 and C4** (unplug and replug, power cycle, timing changes, soak) for #11
and #19.

## 6. Exclusive receiver (only in a planned maintenance window)

Rows **B1–B3**: these need HyperHDR stopped on the host. A container runner
can't see host processes. If you use the workflow's `expbuf: true`, setting
`host_idle_confirmed` is **your attestation** that you stopped the service;
nothing checks it independently. Note the stop and start times in #13 and #28.
Skip this step if no maintenance window is planned.

Only `PASS probe EXPBUF succeeded (expbuf=ok)` from the verifier counts as
EXPBUF evidence. A probe that ran but failed (for example
`expbuf=Device or resource busy`, because the receiver was still in use) or
couldn't be built is a WARN that names the result. It is evidence of the
failure, not of EXPBUF support.

## Evidence audit (2026-11-17, master `57ca21be`)

What each kind of evidence the issues ask for comes from, and what checks it
before it's used. "Gap" is what neither the tools nor this procedure provide.

| Evidence | Produced by | Checked by | Gap |
|---|---|---|---|
| Commit and build | `metadata.json`, `cmake-flags.txt`, each benchmark JSON and text | verifier `run`, `benchmark`: full SHA, master, `--commit`, the same commit everywhere | The installed package's commit (A2 `version`) is the operator's to compare with the run's commit |
| Kernel, receiver identity | `metadata.json`; A2 summary (`driver/bus`, receiver found by driver name) | verifier `run` (kernel recorded); collector test | — |
| A55/A76 affinity, thermal | preflight's pinned CPUs; JSON affinity, MIDR, governor, frequency, thermal before and after | verifier `run`, `benchmark`, `baseline` | Thermal is sampled before and after the run, not continuously |
| Source timing vs configured format | A2 `dv timings` (now size, scan and rate), `format`; `receiver-signal.txt` | verifier `signal` (WARN if missing, incomplete or different) | Needs the source on during the probe |
| Full/limited range | A6 `RGB range evidence` lines; C2 pattern check per source setting; A2 quantization | `color-patterns.py check`; verifier `signal` flags a driver-default quantization | The source's own setting is recorded by the operator (C2) |
| `wled` vs `ddp` | logger `LEDDEVICE_<TYPE>` on each `LED output stats` line; A8 port filter | `stats-align.py` names the transport per device | — |
| Capture and LED stats, same interval | A5 + A9 lines | `stats-align.py`: pairs intervals overlapping ≥ 90%, states the capture interval's rounding uncertainty | Both need debug log level (above) |
| Benchmark and probe failures | exit status in each text file; EXPBUF result line | verifier: non-zero exit is FAIL; failed or skipped EXPBUF is WARN, never evidence | — |
| Privacy | collector and doctor redaction | verifier `privacy` (tokens, keys, addresses, given host and user names) | Host names only with `--hostname` |
| Unknown or unmeasured | `n/a` in both stats lines; summary disclaimers | `stats-align.py` reports missing lines as missing, not zero, and latency and WLED receipt as not measured | Optical and capture-to-LED latency: not instrumented; measurement plan for #9 and #12 in a separate PR |

## What this session can and can't close

| Issue | Criteria this session can provide evidence for | Still open after it |
|---|---|---|
| #28 | runner online with labels (screenshot); `tests` passes with artifact (steps 1–2); `probe` read-only (step 3); benchmark artifact with A55/A76 metadata linked from #9/#12 | trust boundaries and contention documented and reviewed; environment reviewer setting confirmed; any EXPBUF run with exclusive access |
| #27 | on-board doctor output, healthy and no-signal (step 3) | linking from README/BUILDING if not done; review of the archive |
| #12 | first controlled A76/A55 baseline with conditions | hotspot report, any optimisation, before/after |
| #9 | capture stats + LED output stats, same minutes | stage timing with frame identifiers, optical latency |
| #10, #8 | C1/C2/C5 results | HDR matrix, WLED colour accuracy |
| #11, #19 | C3/C4 results | soak duration targets |

Close an issue only when **every** checkbox in its own acceptance criteria has
attached evidence.
