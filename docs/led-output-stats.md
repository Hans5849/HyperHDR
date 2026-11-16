# LED output statistics (issue #9)

Every LED device writes one `LED output stats` line per minute while it is
enabled, and a final line when it is disabled. It answers: how many LED updates
did HyperHDR hand to this device, how many did it write, how many packets did
that take, and did the local socket accept them?

It does **not** measure:

- **capture-to-LED latency.** No frame identifier links an update to a captured
  frame. With smoothing, an update blends several captures.
- **delivery.** UDP has no acknowledgement. An accepted packet can still be
  dropped on the network or by the controller.
- **light.** Whether, and when, the controller displayed the update. WLED's own
  frame rate isn't visible to HyperHDR.

## Example

```
LED output stats: 60.0 s, presented 3600 (60.00/s); updates 3600 (60.00/s) (new colours 3600, repeats 0), failed 0; packets 10800 (3.00/update, max 3), rejected by socket 0, 15606000 bytes accepted
```

Healthy and idle intervals are logged at debug level, so set the LED device's
log level to debug to see them. An interval with a failed update is logged at
info level. A disabled device's last line is labelled
`LED output stats (device disabled): ...`.

An interval in which nothing happened is still reported:
`LED output stats: 60.0 s, no LED output (0 presented, 0 updates)`.

## Fields

| Field | Meaning |
|---|---|
| presented | Final LED colours handed to the device by the processing or smoothing stage. |
| while inactive | Presented while the device was disabled, off, not ready or in error, so discarded. |
| superseded | Presented, then replaced by newer colours before being written. Happens without a refresh timer, where updates are queued and coalesced, or when the device is disabled with colours pending. |
| updates | Writes started by the device. **Each update is one logical output group**, however many packets the driver splits it into. The `/s` rate is the LED output rate. |
| new colours / repeats | Updates carrying colours not written before, versus resends of the last colours. Resends come from a refresh timer without smoothing, or from the identify blink. |
| failed | Updates where the driver returned an error, or any packet of the update was rejected by the socket. The DDP driver itself reports success even when a packet fails, so the packet result is what counts. |
| packets | UDP datagrams handed to the socket during updates, with the average and maximum per update. DDP splits an update into 480-LED (RGB) or 360-LED (RGBW) packets. The `wled` driver splits at 490 LEDs. |
| rejected by socket | Datagrams the local socket refused or wrote short. |
| bytes accepted | Datagram bytes, headers included, the socket accepted during updates. |
| outside updates | Datagrams sent outside an update, such as black frames when the device is switched off. These aren't LED updates. |
| packets n/a | The driver doesn't send through HyperHDR's UDP provider (serial, SPI, HID, REST), so its packets aren't counted. This is shown as "n/a", never as 0. |

Accounting identity, which the tests check:
`presented = while inactive + superseded + new-colour updates (+1 if colours are pending)`.

## Where the updates come from

| Configuration | Who triggers an update | Relation to capture |
|---|---|---|
| Smoothing on (default) | The LED refresh timer ticks the smoothing stage, which presents interpolated colours that are written immediately | None. Updates follow the LED clock, for example 60/s with a 30 fps source |
| Refresh rate set, smoothing off | The refresh timer resends the last colours | New colours only when processing presents them, the rest are repeats |
| No refresh rate | Each presented colour set queues one write. Several presented before it runs are coalesced (superseded) | At most one update per presented set |

## WLED: DDP or the `wled` driver?

HyperHDR has two drivers that can feed WLED:

- **`ddp`**: DDP on UDP port 4048.
- **`wled`**: WLED's realtime UDP protocol (DRGB, or DNRGB above 490 LEDs) on
  WLED's realtime port, 21324 by default. It doesn't use DDP.

Both go through the UDP provider, so both report packets. Check the device type
at the start of the line's log context (`LEDDEVICE_DDP` or `LEDDEVICE_WLED`)
before reading packet counts as DDP. Also use the matching port in checklist row
A8's `tcpdump` filter.

## Cost

Each update adds a few relaxed atomic increments: no allocation, no lock, and
no per-frame logging. The minute timer only formats one line. It doesn't change
the refresh timer, smoothing or the order of writes. The DDP packet loop moved
into `include/led-drivers/net/DdpPacketizer.h`. The tests check that it produces
byte-identical packets to the previous loop.

## Tests

`tests/led_output` (hosted CI, x86_64 and arm64, ASan/UBSan) covers:

- DDP packet layout for single and multiple packets, RGB and RGBW, and the
  packet-size boundaries
- byte equality with the upstream loop, over 36 sizes
- single-packet and multi-packet output: N packets are one update
- write failures, including a rejected packet on a driver that returns success,
  and a driver without packet counting
- zero-output intervals, and unknown rates shown as n/a
- smoothing-generated, repeated and coalesced updates
- interval reset, flush on disable and re-enable
- a 40,000-step random sequence that checks the accounting identity
- `presented` called from another thread

Mutation checks were run locally: 10 deliberate defects in the counters and the
packetizer, each caught by the suite.

`LedDevice.cpp`, `ProviderUdp.cpp` and `DriverNetDDP.cpp` need Qt, so they're
compiled by the hosted package builds, not by this suite.

## On the board

No board measurement has been made with these counters. To collect one (operator
checklist row A9):

```sh
journalctl -u hyperhdr@ryan --since "-15 min" | grep "LED output stats"
```

Record the updates/s, packets per update and failures next to the
`capture stats` line from the same minutes. Don't compare their rates as a
pipeline throughput: they are clocked independently.
