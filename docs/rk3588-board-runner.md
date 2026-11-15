# Orange Pi 5 Plus hardware-in-the-loop runner

How to provision, use and remove the self-hosted GitHub Actions runner that
`.github/workflows/rk3588-board.yml` targets (issue #28). The runner is a real
board, possibly the one driving your LEDs, so the setup favours isolation and
never touches the production HyperHDR service.

## Trust boundaries

| Boundary | How it is enforced | Checked by |
| --- | --- | --- |
| Only trusted code runs on the board | `workflow_dispatch` is the only trigger (no `pull_request`, `push` or `schedule`), so only users with write access can start a run | `tests/ci/board-workflow-policy-test.py` |
| Only reviewed code runs | The job runs on `master` only, in the `rk3588-hil` environment; add yourself as a required reviewer | policy test |
| Only this board | `runs-on: [self-hosted, Linux, ARM64, rk3588, orange-pi-5-plus, hyperhdr-hil]`, and the preflight refuses any other board model or CPU topology | policy test, `tests/rk3588/board-preflight-test.sh` |
| The runner can't change the OS | Dedicated unprivileged account; the preflight fails on root, passwordless sudo or a writable Docker socket; workflow steps contain no `sudo`, service control or package commands | preflight, policy test |
| No production disruption | Read-only stages are safe while HyperHDR runs. EXPBUF and streaming need the receiver, and the preflight refuses them while a `hyperhdr` process exists. The workflow never stops, restarts or reconfigures HyperHDR, WLED, HDMI, the kernel or the board | preflight |
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

## 3. Install and register the runner (native service)

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
labels.

**A native service is preferred here.** The runner needs real `/dev/video*`
access and no container indirection. A Docker or Portainer runner is possible,
but it needs:
- the real devices passed through: `--device /dev/videoN`, `-v /dev/v4l:/dev/v4l:ro`, `--group-add video`, plus `/proc/device-tree`, which the preflight reads
- no `--privileged` and no Docker socket
- a separate registration for this repository

Repository-scoped runners aren't shared between repositories.

## 4. First runs

Run these from **Actions → RK3588 board checks → Run workflow** on `master`, and
approve the environment when prompted:

1. **`stage: tests`.** Preflight, sanitizer suites natively on ARM64, the A76 and
   A55 BGR24 benchmark, and the preflight test. Attach the artifact's
   `bgr24-bench-a76.txt`, `bgr24-bench-a55.txt`, `metadata.json` and
   `cmake-flags.txt` to #12 and #9.
2. **`stage: probe`.** Adds the receiver requirement, the diagnostics doctor (#27)
   and the read-only evidence collector (#23). Both are safe while HyperHDR runs.
   Attach to #8, #10 and #13.
3. **`stage: probe`, `expbuf: true`.** Only in an idle window: stop HyperHDR
   yourself first (`sudo systemctl stop hyperhdr@<user>`), run it, then start it
   again. The preflight fails the run while HyperHDR is running. It never stops it
   for you.

Every artifact includes `preflight.txt` and `metadata.json`: the board, A55/A76
CPU ids, kernel, OS, Armbian, U-Boot, toolchain, Qt, commit, ref, run id and
inputs. Detected hardware is not proof of use; keep GPU, RGA and NPU claims to
measured results.

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

- **Workspace:** `/srv/github-runner/_work`. Each run checks out fresh, and the
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

```sh
cd /srv/github-runner
sudo ./svc.sh stop && sudo ./svc.sh uninstall
sudo -u github-runner ./config.sh remove --token <REMOVAL TOKEN>   # token from the runner's settings page
sudo deluser --remove-home github-runner                           # optional
```

If the board is gone or reinstalled, delete the runner under **Settings → Actions
→ Runners** instead. Removing it doesn't affect GitHub-hosted CI: `push-pull.yml`
never targets self-hosted runners.
