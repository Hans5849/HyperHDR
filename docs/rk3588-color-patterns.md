# Colour reference patterns (checklist C1, C2, C5)

Patterns for the board session's user-visible checks: orientation and channel
order (C1), full and limited range (C2), and LED zone colours (C5). Issues #8 and
#10.

| File (`docs/color-patterns/`) | Use |
|---|---|
| `c2-layout-1920x1080.png` | **C1/C2.** Labelled corners, 50 numbered edge segments, primaries and secondaries, black, white, gray 128, full-range 0 and 255 next to limited nominal 16 and 235, near-black (4, 8, 24, 32) and near-white (224, 240, 248) steps, a 17-step gray ramp and per-channel ramps |
| `c5-edges-1920x1080.png` | **C5.** Only the corners and the edge segments; black centre. For LED zone mapping |
| `solid-{red,green,blue,white,black,gray128}-1920x1080.png` | **C5.** Full-frame solids for LED colour and channel order |
| `*.json` | Manifest for each PNG: every region's position, its pixel values in the file, and the decoded values to expect under each range interpretation |

To regenerate them, or to make another size (for a 4K source, use the source's
own output size):

```sh
python3 scripts/rk3588/color-patterns.py generate --out docs/color-patterns
python3 scripts/rk3588/color-patterns.py generate --size 3840x2160 --out /tmp/patterns
```

Generation is deterministic: the same size always gives byte-identical files.
CI checks that the committed files match the script.

## What the files are, and what they are not

- **Values in the file.** 8-bit RGB PNG, lossless, with an sRGB chunk. Each
  pixel is an sRGB code value, full range 0–255. The transfer function is sRGB.
  Nothing in the file is "limited range". A patch at 16 is simply a very dark
  gray.
- **What the HDMI source transmits.** This is decided by the source, not by the
  file. A source configured for full RGB sends the codes as they are. One
  configured for limited RGB maps 0–255 to 16–235, and signals the choice in
  the AVI InfoFrame. **Showing an image cannot force a quantization.** A viewer,
  a display server, the GPU driver or the source's "video enhancement" settings
  can also change values on the way: scaling, sharpening, dithering, HDR tone
  mapping, colour management, night light, dynamic contrast.
- **What HyperHDR should decode.** In a correct chain, the decoded image equals
  the file's values whichever range the source used, within 1 code for limited
  range. The two wrong chains have distinct signatures, which the patches at
  0/16 and 235/255 separate:

| Interpretation | What happened | file 0 | file 16 | file 235 | file 255 |
|---|---|---:|---:|---:|---:|
| correct | full range, or limited range expanded once | 0 | 16 | 235 | 255 |
| not expanded | limited signal decoded as full | **16** | 30 | 218 | **235** |
| expanded twice | full signal expanded as if limited | 0 | **0** | 255 | 255 |

Each manifest region holds all three expectations (`expect.correct`,
`expect.not_expanded`, `expect.expanded_twice`).

The patterns are SDR. They say nothing about HDR (PQ, HLG) signalling or
precision, and an 8-bit BGR24 capture can't show more than 8 bits.

## Board session procedure

These are user-visible actions (checklist class C): HyperHDR keeps running, the
TV picture changes. Record the HyperHDR commit, the source device and its
output settings with every result.

### 1. Prepare the source

1. Set the source to output the pattern's resolution. Use 1920x1080 for the
   committed files; for a 4K source, generate 3840x2160. Choose a refresh rate
   the receiver accepts.
2. Turn off HDR output, night light or blue-light filters, dynamic contrast,
   "vivid" or "enhanced" picture modes, and any video sharpening on the source.
3. Show the image full-screen at **100 % scale, with no scaling or
   letterboxing**, and colour management off or set to sRGB. For example:
   - Linux: `mpv --fs --video-unscaled=yes --image-display-duration=inf c2-layout-1920x1080.png`
     or `feh --fullscreen --zoom 100 c2-layout-1920x1080.png`
   - Windows and macOS: a viewer that can show the image 1:1 full-screen. Check
     that the corner segments touch the screen edges.

### 2. Find and record the source's RGB range setting

Use the setting's own name, and record "unknown" when the source has no such
setting. Don't assume one.

| Source | Where |
|---|---|
| Linux, Intel (i915) or Raspberry Pi (vc4), X11 | `xrandr --prop` → `Broadcast RGB` (`Automatic`, `Full`, `Limited 16:235`); set with `xrandr --output HDMI-1 --set "Broadcast RGB" "Full"` |
| Linux, other drivers or Wayland | the compositor's or driver's own setting, if any; many have none |
| Windows, NVIDIA | NVIDIA Control Panel → Change resolution → Output dynamic range |
| Windows, AMD | AMD Software → Display → Pixel Format (RGB 4:4:4 Full RGB / Limited RGB) |
| Windows, Intel | Intel Graphics Command Center → Display → Quantization Range |
| Game consoles, streaming boxes | video output settings → RGB range (Automatic / Full / Limited) |
| macOS | no user setting; record "unknown (macOS)" |

"Automatic" usually follows the EDID that the source reads. Behind a splitter
or switch, that's the splitter's EDID, not the receiver's.

### 3. Record what the receiver reports (read-only)

These are class A commands: they're safe while HyperHDR runs.

```sh
RX=$(v4l2-ctl --list-devices | awk '/snps_hdmirx|rk_hdmirx/ {getline; print $1; exit}')
v4l2-ctl -d "$RX" --get-fmt-video | grep -i -E "pixel format|quantization"   # format quantization
v4l2-ctl -d "$RX" -l | grep -i range                                       # receiver RGB range control, if exposed
journalctl -u hyperhdr@ryan -b | grep -E "RGB range (evidence|mismatch)" | tail -3
```

Do this for every source setting you test (step 4).

### 4. C2: check the decoded picture, for each source setting

For **full**, then **limited** (and **automatic** if the source has it):

1. In HyperHDR, turn off tone mapping or LUT processing for this check. The
   preview should show the decoded image itself.
2. Open HyperHDR's live video preview and save a screenshot as **PNG**. Crop
   it to exactly the picture area, with no browser or window borders. The
   preview may be smaller than the source; that's fine.
3. Run:

   ```sh
   python3 scripts/rk3588/color-patterns.py check \
     --manifest docs/color-patterns/c2-layout-1920x1080.json preview-full.png
   ```

   It samples the centre of every region and prints:
   - the range interpretation the picture matches
   - orientation (mirrored, flipped or rotated pictures are named)
   - regions with red and blue swapped
   - every region that matches no interpretation, with measured and expected values

   Exit status 0 means `PASS`.
4. Attach the screenshot, the `check` output, the source setting and the
   step-3 output to #10.

The default tolerance is ±3 codes. That allows for display-path dithering and a
lightly compressed preview. If many regions miss by 4 or 5 and nothing else is
wrong, the preview is probably compressed. Rerun with `--tolerance 5` and say so
in the result. Don't raise the tolerance to hide a range or channel problem:
those miss by 14 or more codes at the discriminating patches.

**Pass:** `range interpretation: correct` for **every** source setting. Also
check by eye that the 0 and 16 patches differ, and that the 235 and 255 patches
differ.

### 5. C1: orientation and channel order

C1 is covered by the same `check` run on `c2-layout`: the corner colours and
the red/blue test. Also check by eye that the corner labels read correctly in
the preview.

### 6. C5: LED output (a correct preview doesn't validate the LEDs)

1. Show `c5-edges-1920x1080.png`. Each LED zone along the frame should take the
   colour of the edge segment it maps to. The `c5-edges` manifest lists every
   segment's position and colour, and the segment number is drawn in each
   segment.
2. Show each `solid-*` image in turn. Every LED should show that colour:
   - red, green and blue show the channel order
   - white shows the white balance
   - black should be off
   - gray 128 shows brightness linearity
3. Photograph the strip for each pattern, with manual exposure if possible, and
   attach the photos with the LED device type (`ddp` or `wled`) and its settings.
   Pair them with the `LED output stats` lines from the same minutes (row A9).

Photos of LEDs are qualitative. A camera doesn't measure colour accuracy. This
check finds wrong channel order, wrong zone mapping and dead or stuck LEDs. It
doesn't produce a colour-error figure.

## Tests

`tests/ci/color-patterns-test.py` runs in hosted CI and checks:

- **Generation:** byte-identical between runs; the committed files are
  current; every manifest region's centre holds exactly its stated values.
- **Range models:** they match independent formulas for all 256 values.
- **`check` on simulated pictures:**
  - a correct decode passes
  - *not expanded* and *expanded twice* are each identified
  - swapped red and blue is detected
  - mirrored, flipped and rotated pictures are named
  - a half-size preview and ±2 noise still pass, while 6-code errors fail
  - RGBA screenshots and all five PNG filter types are read; 16-bit and
    corrupt files are refused
