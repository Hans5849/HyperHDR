# Orange Pi 5 Plus hardware-in-the-loop runner

How to provision, use and remove the self-hosted GitHub Actions runner that
`.github/workflows/rk3588-board.yml` targets (issue #28). The runner is a real
board, possibly the one driving your LEDs, so the setup favours isolation and
never touches the production HyperHDR service.

Two deployments are supported and the preflight handles both:

- **Docker container** (section 3, in use on the project's Orange Pi 5 Plus):
  unprivileged, only the devices the workflow needs, no Docker socket.
- **Native service** (section 4): a dedicated unprivileged account.

An existing working container does not need to be replaced by a native runner.

## Trust boundaries

| Boundary | How it is enforced | Checked by |
| --- | --- | --- |
| Only trusted code runs on the board | `workflow_dispatch` is the only trigger (no `pull_request`, `push` or `schedule`), so only users with write access can start a run | `tests/ci/board-workflow-policy-test.py` |
| Only reviewed code runs | The job runs on `master` only, in the `rk3588-hil` environment; add yourself as a required reviewer | policy test |
| Only this board | `runs-on: [self-hosted, Linux, ARM64, rk3588, orange-pi-5-plus, hyperhdr-hil]`, and the preflight refuses any other board model or CPU topology | policy test, `tests/rk3588/board-preflight-test.sh` |
| The runner can't change the OS | Dedicated unprivileged account; the preflight fails on root, passwordless sudo or a writable Docker socket, and in a container on `CAP_SYS_ADMIN` in the bounding set (`--privileged`); workflow steps contain no `sudo`, service control, package or `docker` commands, and the job has no `container:` or `services:` | preflight, policy test (the gate condition is evaluated, not string-matched) |
| No production disruption | Read-only stages are safe while HyperHDR runs. EXPBUF and streaming need the receiver, and the preflight refuses them while a `hyperhdr` process exists. A container can't see host processes, so there it also needs the `host_idle_confirmed` input (you stopped the host service yourself). The workflow never stops, restarts or reconfigures HyperHDR, WLED, HDMI, the kernel or the board | preflight |
| Token scope | `permissions: contents: read` | policy test |

The runner itself still executes whatever a write-access user dispatches from
`master`. The environment's required reviewers are the last gate.

## 1. Repository settings (GitHub web UI)

1. **Settings → Environments → New environment** `rk3588-hil`:
   - Required reviewers: yourself.
   - Deployment branches: *Selected branches* → `master`.
2. **Settings → Actions → Runners → New self-hosted runner**: Linux, ARM64. Keep
   the page open for the download URL and the registration token. **Don't commit
   or paste the token anywhere.**

## 2. Dedicated runner account on the Orange Pi

Run once, as an administrator on the board:

```sh
sudo adduser --system --group --home /srv/github-runner github-runner
sudo usermod -aG video github-runner          # HDMI receiver; nothing else yet
# Build and probe dependencies the workflow checks for (it never installs anything):
sudo apt install build-essential cmake pkg-config qt6-base-dev libturbojpeg0-dev \
  v4l-utils iproute2 procps psmisc curl python3
```

Don't add the account to `sudo`, `docker`, `render` or any accelerator group.
Add `render` (Mali-G610), RGA or NPU device access only when a workflow step
needs it and that access has been verified (#13–#16). Then also add the matching
label, `mali-g610`, `rga` or `rknpu`, as described below.

## 3. Docker runner (supported; in use)

This is the deployment running on the project's Orange Pi 5 Plus. Verified
properties:

| Item | Value |
| --- | --- |
| Container | `github-runner-hyperhdr-opi5plus` |
| Image | `local/hyperhdr-runner:debian13-arm64` (Debian 13, GitHub Actions runner 2.338.2) |
| Runner name | `opi5plus-hyperhdr-01`, repository-scoped to `Hans5849/HyperHDR` |
| Labels | `self-hosted`, `Linux`, `ARM64`, `rk3588`, `orange-pi-5-plus`, `hyperhdr-hil` (must match `runs-on` exactly) |
| User | UID 1001, GID 121, supplementary group `video` (GID 44); never root |
| Workspace | `/_work` |
| Privileges | not `--privileged`; Docker socket **not** mounted |

Device and mount mappings, each with its purpose:

| Source (host) | Destination (container) | Mode | Why |
| --- | --- | --- | --- |
| `/dev/video3` (the HDMI receiver, currently) | same path | device, rw | receiver probes. The number can change, so pass the node the preflight reports as `RECEIVER` |
| `/sys/firmware/devicetree/base` | `/proc/device-tree` | read-only bind mount | board identity (`model`, `compatible`); without it the preflight fails rather than guessing |
| group `video` | `--group-add 44` | | non-root access to `/dev/video3`. Use the host's `video` GID: `getent group video` |

Illustration of the equivalent `docker run` options (compare with your own
container; registration material is not shown and must never be committed):

```sh
docker run -d --name github-runner-hyperhdr-opi5plus --restart unless-stopped \
  --user 1001:121 --group-add 44 \
  --device /dev/video3 \
  -v /sys/firmware/devicetree/base:/proc/device-tree:ro \
  local/hyperhdr-runner:debian13-arm64
# registered once with: ./config.sh --url https://github.com/Hans5849/HyperHDR \
#   --name opi5plus-hyperhdr-01 --labels rk3588,orange-pi-5-plus,hyperhdr-hil \
#   --work /_work --unattended      (token from Settings -> Actions -> Runners; never store it)
```

No `--privileged`, no `-v /var/run/docker.sock`, no `--pid=host`.

### What differs inside a container

The preflight supports both and doesn't weaken any check for containers:

- **CPUs.** `/proc/cpuinfo` lists every host CPU, so the Cortex-A55/A76 topology
  (CPU part `0xd05` / `0xd0b`) is still identified. What `taskset` may use is the
  process's `Cpus_allowed_list`; the preflight pins to the first A55 and A76 CPU
  that list contains, and fails if a cluster has none (e.g. `--cpuset-cpus`
  excludes it) or the list can't be read.
- **Load and contention.** `/proc/loadavg` and CPU frequency are host-wide. The
  LizzardMOTD runner shares this board, so every benchmark file records the
  load average before the run and warns above 1.0.
- **Processes.** The container can't see the host's `hyperhdr` process. Runs that
  need exclusive receiver access (`expbuf`) therefore fail unless you set
  `host_idle_confirmed` after stopping the host service yourself. A visible
  `hyperhdr` process always blocks.
- **Device nodes.** `/sys/class/video4linux` is visible, but only passed-through
  `/dev` nodes exist. The receiver is found in sysfs by name or driver
  (`stream_hdmirx` / `snps_hdmirx`, never by number); a receiver in `/sys` without
  its node in `/dev` fails the probe stage with the `--device` hint.
- **Privileges.** `CapBnd` with `CAP_SYS_ADMIN` (what `--privileged` leaves) fails;
  root, passwordless `sudo` and a writable Docker socket fail as before.
- **Device tree.** Read from `/proc/device-tree`, falling back to
  `/sys/firmware/devicetree/base`. A hidden tree is a failure.

### Restart behaviour

`--restart unless-stopped` brings the runner back after a reboot or Docker
restart. After a reboot the video node number may change: check
`docker inspect` for the device mapping and the `RECEIVER` line in `preflight.txt`.
Stop it with `docker stop github-runner-hyperhdr-opi5plus`; the workflow itself
never stops or starts containers or services.

### Isolation from the LizzardMOTD runner

The LizzardMOTD runner on the same board is a separate container and
registration with its own labels. Nothing here uses its labels, workspace or
devices, and `hyperhdr-hil` is only on this repository's runner. Expect CPU
contention between them: for clean numbers, run when LizzardMOTD is idle and
check the recorded load average.

### Testing and troubleshooting

```sh
docker logs --tail 20 github-runner-hyperhdr-opi5plus      # "Listening for Jobs"
docker exec github-runner-hyperhdr-opi5plus id             # uid=1001, group video present
docker exec github-runner-hyperhdr-opi5plus sh -c 'tr -d "\0" </proc/device-tree/model; echo; grep -c "CPU part" /proc/cpuinfo; grep -E "Cpus_allowed_list|CapBnd" /proc/self/status'
docker exec github-runner-hyperhdr-opi5plus sh -c 'ls -l /dev/video*; for n in /sys/class/video4linux/*; do echo "$n: $(cat $n/name)"; done'
docker exec github-runner-hyperhdr-opi5plus bash /_work/HyperHDR/HyperHDR/scripts/rk3588/board-preflight.sh --require-receiver   # after one checkout
docker inspect -f '{{.HostConfig.Privileged}} {{.HostConfig.Devices}} {{.Mounts}}' github-runner-hyperhdr-opi5plus
```

| Symptom | Likely cause |
| --- | --- |
| `no device-tree model` | `/sys/firmware/devicetree/base` isn't mounted at `/proc/device-tree` |
| `receiver ... device node is missing` | add `--device /dev/videoN` for the node the preflight names |
| `not readable/writable ... add to group video` | `--group-add 44` (host `video` GID) missing |
| `none of the Cortex-A55/A76 CPUs ... allowed` | `--cpuset-cpus` excludes a cluster |
| `CAP_SYS_ADMIN in its bounding set` | container was started with `--privileged`; recreate it without |
| `can't tell whether HyperHDR is capturing` | `expbuf` in a container; stop the host service, then set `host_idle_confirmed` |

## 4. Native service (alternative)

Use this when you'd rather not run a container. A native runner has the same
trust boundaries and needs no device mappings.

### Dedicated account

Run once, as an administrator on the board:

```sh
sudo adduser --system --group --home /srv/github-runner github-runner
sudo usermod -aG video github-runner          # HDMI receiver; nothing else yet
# Build and probe dependencies the workflow checks for (it never installs anything):
sudo apt install build-essential cmake pkg-config qt6-base-dev libturbojpeg0-dev \
  v4l-utils iproute2 procps psmisc curl python3
```

Don't add the account to `sudo`, `docker`, `render` or any accelerator group.
Add `render` (Mali-G610), RGA or NPU device access only when a workflow step
needs it and that access has been verified (#13–#16). Then also add the matching
label, `mali-g610`, `rga` or `rknpu`, as described below.

### Install and register

As `github-runner` (`sudo -u github-runner -s`), in `/srv/github-runner`, use the
download commands from the settings page, then:

```sh
./config.sh --url https://github.com/Hans5849/HyperHDR --token <REGISTRATION TOKEN> \
  --name opi5plus-hyperhdr-01 --labels rk3588,orange-pi-5-plus,hyperhdr-hil \
  --work _work --unattended
exit
cd /srv/github-runner && sudo ./svc.sh install github-runner && sudo ./svc.sh start
```

GitHub adds `self-hosted`, `Linux` and `ARM64` automatically. Check **Settings →
Actions → Runners**: `opi5plus-hyperhdr-01` should show as *Idle* with all six
labels. Repository-scoped runners aren't shared between repositories. Don't use
the same runner name for both deployments.

## 5. First runs

Run these from **Actions → RK3588 board checks → Run workflow** on `master`, and
approve the environment when prompted:

1. **`stage: tests`.** Preflight, sanitizer suites natively on ARM64, the A76 and
   A55 BGR24 benchmark, and the preflight test. Attach the artifact's
   `bgr24-bench-a76.txt`, `bgr24-bench-a55.txt`, `metadata.json` and
   `cmake-flags.txt` to #12 and #9. The benchmark runs
   `build-codec/bin/DecoderTest` (where CMake writes it); a missing or
   non-executable binary or a non-zero benchmark exit fails the job.
2. **`stage: probe`.** Adds the receiver requirement, the diagnostics doctor (#27)
   and the read-only evidence collector (#23). Both are safe while HyperHDR runs.
   Attach to #8, #10 and #13.
3. **`stage: probe`, `expbuf: true`.** Only in an idle window: stop HyperHDR
   yourself first (`sudo systemctl stop hyperhdr@<user>`), run it, then start it
   again. The preflight fails the run while HyperHDR is running. It never stops it
   for you. With the Docker runner also set `host_idle_confirmed: true`, because
   the container can't see the host's processes.

Every artifact includes `preflight.txt` and `metadata.json`: the board, A55/A76
CPU ids and the CPUs actually pinned, container or native, receiver node and
driver, kernel, OS, Armbian, U-Boot, toolchain, Qt, commit, ref, run id and
inputs. The benchmark files add the commit, governor, frequency and thermal
readings before and after, the load average, the exit status and the raw timings;
`cmake-flags.txt` has the build configuration. Detected hardware is not proof of
use; keep GPU, RGA and NPU claims to measured results.

### Reading the numbers and the HDMI signal

- The benchmark times the decoder on a **synthetic 1920x1080 BGR24 frame**
  (stride 5760) generated in memory. It isn't the receiver's current signal and
  isn't HDMI-to-LED latency.
- Don't treat a single run as a baseline. The board is shared with the
  LizzardMOTD runner and may be thermally or frequency limited; compare the
  recorded load average, governor and temperatures.
- In the probe stage `receiver-signal.txt` keeps two things apart: the **source
  timing** the receiver reports (`VIDIOC_QUERY_DV_TIMINGS`, e.g. 3840x2160 at
  30 Hz on the loopback cable) and the **configured capture format**
  (`VIDIOC_G_FMT`, e.g. 1920x1080 `BGR3`, stride 5760, 6220800-byte buffer,
  limited range). They can differ; don't assume 1080p60 or `/dev/video3`.

## Disruption policy

- Read-only stages (`tests`, `probe` without `expbuf`) may run while the
  production HyperHDR service is capturing.
- Anything that opens the receiver (EXPBUF, `--stream`, future capture tests)
  needs exclusive access. The operator stops the service in an idle window; the
  workflow refuses to run while it's up.
- The workflow never reboots the board, changes the kernel, changes WLED or
  HDMI-source settings, or edits HyperHDR's configuration.
- Long stress or soak runs (#11) should use a physically separate board where
  possible, or a scheduled idle window on this one.

## Workspace, network and data

- **Workspace:** `/srv/github-runner/_work` (native) or `/_work` (container). Each run checks out fresh, and the
  last step removes its build trees, so only the runner's own logs and the
  checkout remain. Results live in the run's artifact for 30 days.
- **Network:** the runner only makes outbound HTTPS connections to GitHub; it
  needs no inbound ports. The doctor contacts another host only with
  `--led-host`, which the workflow doesn't pass.
- **Data:** the evidence collector and the doctor redact addresses, the hostname
  and the user name, and never read HyperHDR's database or credentials.
  Artifacts are visible to anyone who can see the repository's Actions runs.
  Review them before linking them publicly.

## Removing the runner

Container: `docker stop` and `docker rm github-runner-hyperhdr-opi5plus`, then
remove the runner under **Settings → Actions → Runners** (or run `config.sh
remove` with a removal token inside the container). Native:

```sh
cd /srv/github-runner
sudo ./svc.sh stop && sudo ./svc.sh uninstall
sudo -u github-runner ./config.sh remove --token <REMOVAL TOKEN>   # token from the runner's settings page
sudo deluser --remove-home github-runner                           # optional
```

If the board is gone or reinstalled, delete the runner under **Settings → Actions
→ Runners** instead. Removing it doesn't affect GitHub-hosted CI: `push-pull.yml`
never targets self-hosted runners.
