# BGR24 benchmark reporting and the LUT slowdown (issue #12)

`DecoderTest --bgr24-bench` times the real `FrameDecoder` BGR24 dispatch at the
mainline capture layout (1920x1080, stride 5760, one plane) for full/limited
range x full/quarter output x LUT on/off. This page covers its machine-readable
output, the multi-run summarizer, and the open question of why LUT decoding is so
much slower on the board.

**Status (2026-11-15):** reporting and summarizer are tested in hosted CI. No
board run with the new output has been made yet; every board figure on this page
is either the earlier manual A76 run or labelled pending.

## Running it

Without options the output is the original table, byte for byte (CI checks the
header), so older notes and scripts keep working:

```sh
cmake -S tests/codec_test -B build-codec -DPLATFORM=linux -DCMAKE_BUILD_TYPE=Release -DUSE_CCACHE_CACHING=OFF
cmake --build build-codec -j --target DecoderTest
taskset -c 4 build-codec/bin/DecoderTest --bgr24-bench                 # Cortex-A76 (RK3588 CPU 4-7)
taskset -c 0 build-codec/bin/DecoderTest --bgr24-bench                 # Cortex-A55 (RK3588 CPU 0-3)
```

Machine-readable output, as the board workflow runs it:

```sh
taskset -c 4 build-codec/bin/DecoderTest --bgr24-bench \
  --pattern random,gradient,solid --raw \
  --json a76.json --csv a76.csv --commit "$(git rev-parse HEAD)" --label A76
taskset -c 0 build-codec/bin/DecoderTest --bgr24-bench \
  --pattern random,gradient,solid --raw \
  --json a55.json --csv a55.csv --commit "$(git rev-parse HEAD)" --label A55
python3 scripts/rk3588/bench-summarize.py a76.json a55.json > summary.md
```

| Option | Meaning |
|---|---|
| `--json FILE` | Full run: metadata, configuration, one record per case (schema `hyperhdr.bgr24-bench/1`). |
| `--csv FILE` | One row per case with the key metadata repeated, for spreadsheets. |
| `--raw` | Include every timed sample (`raw_ns`) in the JSON so statistics can be recomputed. |
| `--pattern LIST` | Inputs, comma-separated: `random` (the original, default), `gradient`, `solid`. |
| `--iterations N`, `--warmup N` | Timed and warm-up calls per case (defaults 200 and 10). |
| `--commit SHA`, `--label NAME` | Recorded as given; `--label` names the core when MIDR is unavailable. |
| `--input FILE --input-desc DESC` | Time a saved raw frame instead of the synthetic patterns (see [Captured-frame input](#captured-frame-input)). Replaces `--pattern`; giving both is an error. |

Unknown or malformed options exit with status 2 and name the first bad argument,
so a typo can't silently produce a default run.

## What is recorded

Per case: scenario (`range-output-lut-pattern`), range, output size actually
produced, LUT on/off, input pattern, warm-up count, sample count, the first
(cold) call, min / median / p95 / p99 / max / mean / sample standard deviation,
**output buffer reallocations** across the timed calls (an allocation check), and
for LUT cases the **LUT access profile** of that input: lookups, distinct entries,
distinct 64-byte lines and distinct 4 KiB pages actually touched. The access
profile is computed from the input with the decoder's own sampling and range
expansion, not from timing.

Per run: commit, label, compiler, CMake build type, optimisation and assertion
state, OS, architecture, CPU affinity, pinned CPU, MIDR and decoded core
(Cortex-A76 / A55), `model name`, governor, min/max frequency, and before/after
snapshots of current frequency, load average and every thermal zone.

**Missing is not zero.** Anything that can't be read is JSON `null` / CSV `NA`.
An observed 0 kHz is written as 0. Per-core sysfs is read only when the process
is pinned to exactly one CPU; otherwise it would describe a core the benchmark
may not have run on.

**Statistics rule.** Times are integer nanoseconds from `steady_clock`.
Percentile p is element `floor(n*p/100)` (0-based) of the ascending samples:
the same rule the original table used, so the text and JSON agree.
`bench-summarize.py` recomputes min/median/p95/p99/max from `raw_ns` and refuses
a file whose summary disagrees.

## JSON fields

`scripts/rk3588/bench-schema.json` specifies every field: name, type, unit,
whether it may be null and whether it is required, in output order. The table
below is generated from it (`scripts/rk3588/bench-validate.py --markdown`). CI
fails if this table and the specification disagree.

| Field | Type | Unit | Null allowed | Required | Meaning |
|---|---|---|---|---|---|
| `schema` | string |  | no | yes | schema and version identifier |
| `metadata` | object |  | no | yes | run conditions |
| `config` | object |  | no | yes | input layout and statistics rules |
| `cases` | array<object> |  | no | yes | one record per timed case |
| `metadata.commit` | string |  | yes | yes | as given by --commit |
| `metadata.label` | string |  | yes | yes | as given by --label |
| `metadata.compiler` | string |  | yes | yes | compiler and version |
| `metadata.build_type` | string |  | yes | yes | CMake build type |
| `metadata.optimized` | boolean |  | no | yes | built with optimisation |
| `metadata.assertions` | boolean |  | no | yes | assertions enabled |
| `metadata.os` | string |  | yes | yes | kernel name and release |
| `metadata.arch` | string |  | yes | yes | machine architecture |
| `metadata.affinity` | array<integer> | CPU index | yes | yes | CPUs the process may run on |
| `metadata.pinned_cpu` | integer | CPU index | yes | yes | the CPU when the affinity is exactly one, else null |
| `metadata.start_cpu` | integer | CPU index | yes | yes | CPU at start |
| `metadata.midr` | string |  | yes | yes | MIDR_EL1 of the pinned CPU; null unless pinned to one CPU |
| `metadata.core` | string |  | yes | yes | core decoded from MIDR (e.g. Cortex-A76); null unless pinned to one CPU |
| `metadata.model_name` | string |  | yes | yes | /proc/cpuinfo model name |
| `metadata.governor` | string |  | yes | yes | cpufreq governor of the pinned CPU; null unless pinned to one CPU |
| `metadata.min_freq_khz` | integer | kHz | yes | yes | scaling_min_freq of the pinned CPU; null unless pinned to one CPU |
| `metadata.max_freq_khz` | integer | kHz | yes | yes | scaling_max_freq of the pinned CPU; null unless pinned to one CPU |
| `metadata.before` | object |  | no | yes | readings before the first case |
| `metadata.after` | object |  | no | yes | readings after the last case |
| `metadata.before / after.cur_freq_khz` | integer | kHz | yes | yes | scaling_cur_freq of the pinned CPU; null unless pinned to one CPU |
| `metadata.before / after.loadavg` | array<number> |  | yes | yes | 1, 5 and 15 minute load averages |
| `metadata.before / after.thermal` | array<object> |  | no | yes | every readable thermal zone; empty when none |
| `metadata.*.thermal[].zone` | string |  | no | yes | thermal_zoneN |
| `metadata.*.thermal[].type` | string |  | yes | yes | zone type, e.g. bigcore0-thermal |
| `metadata.*.thermal[].milli_c` | integer | milli-degC | yes | yes | temperature |
| `config.width` | integer | px | no | yes | input width |
| `config.height` | integer | px | no | yes | input height |
| `config.stride` | integer | bytes | no | yes | bytes from one input row to the next |
| `config.lut_bytes` | integer | bytes | no | yes | LUT size |
| `config.lut_fill` | string |  | no | yes | LUT contents |
| `config.seed` | integer |  | no | yes | seed of the random pattern |
| `config.percentile_rule` | string |  | no | yes | percentile definition |
| `config.time_unit` | string |  | no | yes | unit of every *_ns field |
| `config.clock` | string |  | no | yes | clock used for timing |
| `config.input` | object |  | no | no | where the input came from; absent in files written before 2026-11-16 |
| `config.input.kind` | string |  | no | yes | synthetic pattern or saved raw frame |
| `config.input.name` | string |  | yes | yes | raw file name, without directories; null for synthetic input |
| `config.input.sha256` | string |  | yes | yes | SHA-256 of the raw file; null for synthetic input |
| `config.input.format` | string |  | yes | yes | pixel format from the description; null for synthetic input |
| `config.input.declared_range` | string |  | yes | yes | full or limited, as the description states; null for synthetic input |
| `config.input.orientation` | string |  | yes | yes | row order from the description; null for synthetic input |
| `config.input.payload_bytes` | integer | bytes | yes | yes | raw file size; null for synthetic input |
| `config.input.source` | string |  | yes | yes | free text from the description (optional there) |
| `cases[].scenario` | string |  | no | yes | range-output-lut|nolut-pattern |
| `cases[].range` | string |  | no | yes | decoder range mode |
| `cases[].output` | string |  | no | yes | output size mode |
| `cases[].lut` | boolean |  | no | yes | LUT applied |
| `cases[].pattern` | string |  | no | yes | input: synthetic pattern or file:<first 12 hex of sha256> |
| `cases[].output_width` | integer | px | no | yes | output width actually produced |
| `cases[].output_height` | integer | px | no | yes | output height actually produced |
| `cases[].warmup` | integer | calls | no | yes | untimed calls before sampling (--warmup) |
| `cases[].samples` | integer | calls | no | yes | timed calls (--iterations) |
| `cases[].first_ns` | integer | ns | no | yes | the first (cold) call, part of the warm-up |
| `cases[].output_reallocations` | integer | count | no | yes | output buffer address changes across timed calls |
| `cases[].min_ns` | integer | ns | no | yes | minimum |
| `cases[].median_ns` | integer | ns | no | yes | p50 by the percentile rule |
| `cases[].p95_ns` | integer | ns | no | yes | p95 by the percentile rule |
| `cases[].p99_ns` | integer | ns | no | yes | p99 by the percentile rule |
| `cases[].max_ns` | integer | ns | no | yes | maximum |
| `cases[].mean_ns` | number | ns | no | yes | arithmetic mean |
| `cases[].stddev_ns` | number | ns | yes | yes | sample standard deviation; null below two samples |
| `cases[].lut_access` | object |  | yes | yes | LUT access profile of the input; null without LUT |
| `cases[].raw_ns` | array<integer> | ns | no | no | every timed sample, in order (--raw only) |
| `cases[].lut_access.lookups` | integer | count | no | yes | LUT lookups per call |
| `cases[].lut_access.distinct_entries` | integer | count | no | yes | distinct LUT entries touched |
| `cases[].lut_access.distinct_lines_64` | integer | count | no | yes | distinct 64-byte lines touched |
| `cases[].lut_access.distinct_pages_4k` | integer | count | no | yes | distinct 4 KiB pages touched |

`scripts/rk3588/bench-validate.py [--iterations N] [--warmup N] [--patterns LIST] [--raw] [--input-sha256 HEX] RUN.json...`
checks a file against the specification. Use it before the summarizer, and on
every board artifact. Beyond types, it checks:

- no undocumented fields, and fields in the specified order;
- null only where allowed. Per-core fields must be null when the run wasn't
  pinned to one CPU, and the file fields of `config.input` must be null for
  synthetic input;
- `raw_ns` length equals `samples`. min/median/p95/p99/max are recomputed
  exactly, and mean and sample deviation to the written precision;
- scenario names, case order (per pattern: range x output x LUT), output sizes
  (quarter is `(w>>1) x (h>>1)`), and LUT profiles present exactly for LUT cases;
- with options: the iteration and warm-up counts, the pattern order, `--raw`,
  and the input file's SHA-256.

`config.input` was added on 2026-11-16 and is optional, so earlier files stay
valid. The schema identifier is unchanged (`hyperhdr.bgr24-bench/1`): every
change was additive.

## Captured-frame input

`--input FRAME --input-desc DESC` times the decoder on a saved raw frame instead
of a generated pattern, for example to test hypothesis H2 below with real,
spatially coherent video. The benchmark **never opens a capture device**. Saving
a frame is a separate, operator-run step, and it is never automatic.

The description is a small text file. Every property the decoder depends on is
stated in it, never guessed from the file size:

```
# hyperhdr raw frame description, version 1
format = BGR24
width = 1920
height = 1080
stride = 5760
range = limited
orientation = top-down
payload = 6220800
source = <how the frame was produced; optional>
```

| Key | Rule |
|---|---|
| `format` | `BGR24` only (packed B, G, R; V4L2 `BGR3`). Other formats are rejected, not reinterpreted. |
| `width`, `height` | Pixels, 2..7680 and 2..4320. |
| `stride` | Bytes from one row to the next. At least `width x 3`. |
| `range` | `full` or `limited`: the content's quantization, as recorded when the frame was saved (e.g. from checklist row A4/A6). It is recorded as `declared_range`. The benchmark still times both decoder range modes, which describe how the decoder interprets the bytes, not what the source sent. |
| `orientation` | `top-down` only: the first byte is the blue value of the top-left pixel. Bottom-up frames are rejected. |
| `payload` | Exact file size in bytes. It must be `height x stride` (a V4L2 buffer including the last row's padding) or `(height-1) x stride + width x 3` (no padding after the last row). |
| `source` | Optional free text. |

Unknown or duplicate keys, a missing key, a file shorter or longer than
`payload`, or a payload that matches neither layout: the benchmark exits with
status 2 and names the problem. It doesn't time anything in that case.

In the results, `config.input` names the file and its SHA-256, and the case
pattern is `file:<first 12 hex of the SHA-256>`. Two different frames are never
grouped by the summarizer, and the same bytes always get the same name.

**Privacy.** A captured frame is a picture of whatever the source showed. Only
commit or attach frames with no personal content, such as test patterns shown
by the source.
`tests/bench_report/fixtures` holds two synthetic 64x36 frames made by
`make-fixtures.py`. CI checks that they match the script byte for byte.

**Saved frames are not live capture.** A `--input` run times the decoder on
bytes already in memory: no V4L2 dequeue, DMA, driver or HDMI timing. To keep
that visible, the text output says so on its `input file:` line, and the
summarizer lists saved-frame inputs in their own section and marks their result
rows `(saved frame)`.

The fixtures and CI runs show that the parser and the timing path work. **They
are not RK3588 measurements**, and a 64x36 frame says nothing about cache
behaviour at 1080p.

## Summarizer

`scripts/rk3588/bench-summarize.py [--format markdown|csv|json] [--baseline CORE] RUN.json...`
(Python standard library only):

- validates schema and fields; recomputes statistics from raw samples;
- groups by core x pattern x range x output x LUT. Repeated runs give the median
  of their medians and the spread between runs;
- **core comparison**: each core's median / the baseline's. The default baseline
  is the fastest core, so an A55 row reads "N x slower than the A76";
- **full vs quarter** and **LUT cost** ratios;
- lists each run's conditions (not pinned, governor other than `performance`,
  1-minute load > 1.0, frequency changed during the run, a zone at or above 80 °C,
  any of these unknown, commit unknown), so a noisy run isn't read as a baseline;
- output is identical whatever order the files are given in.

Synthetic sample (generated by `bench-report-test --emit-sample`, the A55 run made
exactly 2.5x the A76 run; **not measurements**):

```
## Core comparison (core / Cortex-A76)
| Core | Pattern | Range | Output | LUT | Ratio |
|---|---|---|---|---|---:|
| Cortex-A55 | gradient | full | full | no | 2.50x |
...
## Full vs quarter output (full / quarter)
| Cortex-A76 | random | full | yes | 4.00x |
## LUT cost (LUT / no LUT)
| Cortex-A76 | random | full | full | 83.33x |
```

On the board, `RK3588 board checks` (stage `tests`) now leaves
`bgr24-bench-{a76,a55}.{txt,json,csv}` and `bgr24-bench-summary.md` in its
artifact. The summary is written when the runner has `python3`; otherwise
summarize the downloaded JSON. Its preflight, environment approval and read-only
policy are unchanged (`tests/ci/board-workflow-policy-test.py`).

## Why is LUT decoding so much slower?

Board data so far (manual A76 run, 2026-11-15, shared board, conditions not
recorded, random input): full-frame LUT 52.9 ms vs 0.62 ms without; quarter-frame
LUT 13.35 ms.

### Method

1. Compare the same decode across inputs with very different LUT locality
   (`random`, `gradient`, `solid`). This changes nothing but the addresses the
   lookups hit.
2. Record the access profile per input, so time can be related to footprint.
3. Check allocation (`output_reallocations`, first-call time) separately from
   steady-state time. The LUT vector is filled before timing, so its pages are
   resident and no page fault falls in the timed loop.
4. Only then use hardware counters (`perf stat -e cycles,instructions,cache-misses,dTLB-load-misses`)
   on one pinned core. That needs operator approval on the board, see below.

### Hypotheses, ranked by current evidence

| # | Hypothesis | Evidence for / against | How it would be falsified |
|---|---|---|---|
| H1 | **Per-pixel LUT lookups miss cache and TLB; time follows access locality, not arithmetic.** The LUT is 48 MiB (16.7 M entries x 3 B), indexed by the RGB value, so random input hits a random line per pixel. | Board: 52.9 / 2.07 M px ≈ **25.5 ns per lookup**, against 0.3 ns/px without LUT: memory latency, not compute. Quarter output (¼ of the lookups) takes 3.96x less (limited: 3.90x): time scales with lookup count. x86 (Xeon, 260 MiB L3 that holds the whole LUT, indicative only): random touches 734 k lines, 12.9 ms; gradient 70 k lines, 3.2 ms; solid 1 line, 2.4 ms. | On the board, `gradient` and `solid` LUT medians close to `random` (within ~1.5x) would reject it. The RK3588's L3 is 3 MiB, so the board gap is expected to be larger than x86's. |
| H2 | **The benchmark's random input overstates production cost.** Real video is spatially coherent, like `gradient`. | Follows from H1 if it holds. No real frame has been timed. | A captured frame (future `--input` option) timing like `random` rather than `gradient`. |
| H3 | **Limited range without LUT is slow because the per-channel table lookup blocks vectorisation.** | Board: limited no-LUT 4.93 ms vs full 0.62 ms (8x). x86 gap is small and noisy. No disassembly yet. | Inspect `Bgr24::row<false,true,1>` code for the A76 build; a vectorised loop would reject it. |
| H4 | Allocation or page faults inside the timed loop. | x86: `output_reallocations` = 0 in all 24 cases; the LUT is pre-touched. | Non-zero `output_reallocations` or a timed median close to `first_ns` on the board. |
| H5 | Board contention, frequency or thermal state inflated the manual run. | Possible: the board is shared and nothing was recorded. Now recorded per run. | Two runs under a clean summary (no conditions) agreeing within a few percent. |

What this does **not** claim: no change has been made to the decoder, and no
speed-up is claimed. If H1 holds on the board, the candidate directions (separate
work, to be measured) are a smaller or factored LUT that fits in L2/L3, a
cache-friendlier layout, and RGA or GPU offload for LUT application.

## Tests

| Suite | Where | What |
|---|---|---|
| `tests/bench_report` | hosted CI (x86_64 + arm64, ASan/UBSan, `-Werror`) | statistics and percentile rule, access profile (sampling, range expansion, line straddling), input patterns, option parsing, metadata from a fake `/sys` and `/proc` (missing vs zero vs malformed), text/JSON/CSV writers |
| `tests/ci/bench-summarize-test.py` | hosted CI | C++ JSON accepted; ratios exact; order-independent output; raw mismatch, schema, missing field and malformed JSON rejected; missing metadata shown as n/a/NA/null; run conditions; repeated runs |
| real `DecoderTest` | hosted CI | default output unchanged; `--raw --json` output passes the summarizer's recomputation; CSV row count; unknown option rejected |
| `tests/ci/board-workflow-policy-test.py` | hosted CI | board step passes `--raw --json --csv --commit --label`, keeps both cores' JSON, writes the summary, fails when a bench exits 0 without JSON |

Mutation checks were run locally: 12 mutants of the report header (percentile
rounding, line straddling, partial numeric parsing, pinning rule, range in the
profile, pattern header, raw on request, sample vs population deviation,
missing-as-zero in JSON and CSV, option typo, quarter sampling) and 7 of the
summarizer. Each one fails a suite.
