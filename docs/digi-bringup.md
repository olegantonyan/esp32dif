# Digi+ (WM8804) bring-up notes

Breadboard bring-up of the PiFi Digi+ 1.0 HAT on the ESP32-C5-DevKitC-1 with jumper wires, November 2026. State at the end: audio plays (440 Hz test tone heard on coax), I2C works with retries but is still noisy. Next step: move to a proper PCB.

## Verified

| What | Result |
|---|---|
| I2C | WM8804 at 0x3B, ID 0x8805 (R0 = 0x05, R1 = 0x88), revision 4 |
| Crystal | 27 MHz, measured from BCLK within a few ppm |
| PLL at 44.1 kHz | N=6, K=0x2c2b25, prescale /2, freqmode 2 |
| PLL at 48 kHz | N=7, K=0x1208a5, prescale /2, freqmode 2 |
| Clocks | BCLK 64 fs and LRCLK exact (±5 ppm), no glitches on LRCLK |
| I2S | ESP32 slave, Philips, 32-bit slots, WM8804 takes 24 bits; 44100 frames/s sent |
| S/PDIF out (TX0, pin 17) | decoded on GPIO5: 44.1 kHz, 0 parity/sync errors, validity 0, L = R, 440.0 Hz at the set level |
| Channel status sent | `04 82 00 00 0b`: consumer, PCM, copy permitted, category PCM coder original, 44.1 kHz, 24-bit |
| Receiver | plays it, coax and optical |
| HAT pin 40 → WM8804 pin 13 (DIN) | continuity OK |

## Problems found, in the order they bit

1. **HAT 3V3 not powered.** This HAT has no 3.3 V regulator path from 5V alone. An unpowered WM8804 clamps SDA low through its ESD diodes and kills the OLED too. Wire 3V3.
2. **WS and DOUT wires swapped.** The ESP32 DOUT output fought the WM8804 LRCLK output. Symptoms: I2C NACKs, SDA stuck low, LRCLK as noise, `DOUT released` showing 48000 edges/s. `digi probe` now detects it.
3. **`digi probe` silenced the audio (firmware bug, fixed).** Its DOUT pull test calls `gpio_set_direction(INPUT)`, which disconnects the I2S output signal from GPIO4. Setting OUTPUT again leaves a plain GPIO stuck at 0, so the S/PDIF stream carried zeros until reboot. The probe now calls `i2s_channel_reconfig_std_gpio` afterwards. This was why no tone was heard for most of the session, even though the stream was valid.
4. **SDA stuck low after an ESP32 reset (every flash) while the WM8804 runs.** Nine recovery clocks plus a STOP don't release it; only a HAT power cycle does. The WM8804 keeps its configuration and keeps clocking. Firmware workaround: if I2C is dead at boot but LRCLK runs at a supported rate, I2S starts anyway at the measured rate, so audio plays (`digi status` says "no I2C answer ... audio runs").
5. **I2C corrupted by I2S crosstalk.** It works with the clocks stopped and fails while BCLK (2.8 MHz) runs. The driver resets the chip first (the reset stops the clocks), so configuration runs on a quiet bus, and retries every access up to 20 times.

## I2C noise measurements (reads of R1 while BCLK/LRCLK run)

| Setup | Failed reads |
|---|---|
| internal pull-ups only (~45 kΩ) + OLED module, open-drain SCL, 400 kHz | 87% |
| same, 50 / 20 / 10 kHz | 100% (bus wedged during the run) |
| push-pull SCL, internal pull-ups, 400 → 10 kHz | 47% → 28% |
| push-pull SCL, external pull-ups on SDA/SCL, tone off | 13% |
| same, tone at 100% (DOUT toggling) | 16% |

- Lower I2C speed doesn't help much: the failures come from crosstalk glitches on slow edges, not from timing.
- SCL is driven push-pull (`gpio_od_disable` after bus creation). This is safe because neither the WM8804 nor the SSD1306 stretches the clock. It halved the errors.
- External pull-ups were the biggest win. A Raspberry Pi has 1.8 kΩ on GPIO2/3, and the HAT relies on them.
- What remains is most likely ground bounce and coupling in the jumper wires: only 2 GND wires (HAT pins 6 and 39) carry the 2.8 MHz return current.

## S/PDIF reference streams

Decoded with `digi decode` on GPIO5.

| | WM8804 (ours) | Orange Pi Zero 3 `hw:SPDIF` |
|---|---|---|
| Rate | 44099 Hz | 47999 Hz / 44099 Hz |
| Channel status | `04 82 00 00 0b` | `00 00 00 00 00` (says 44.1 kHz even at 48 kHz) |
| Sample bits | 24 | 16 |
| Source strength | strong: the ESP32 pull-up doesn't move the duty cycle | weak: the pull-up moves the high time from 51% to 76% |
| Receiver | plays | plays |

- The receiver doesn't care about channel status: all zeros with the wrong rate code plays.
- **Orange Pi bug:** its right channel arrives pairwise swapped (R(n) = L(n xor 1)), so the right channel decodes as 790–1000 Hz zero crossings. The bug is in its tool's interleave or in the H616 S/PDIF driver/DMA path with S16 stereo.

## Decoder notes (`spdif_rx.c`)

- **Capture:** RMT RX at 80 MHz. Only 2 memory blocks are free on the C5 (the LED uses RMT TX); ping-pong into a 24000-symbol buffer gives about 12 ms.
- **Cell-width estimate:** the bit-cell unit interval comes from the run-length histogram, skipping the shortest 0.5% as glitches. Taking the shortest pulse broke on one glitch. Taking the first bin with ≥2% of the runs broke on near-silent streams, which have few 1-cell runs.
- **Expected edge counts:** silence gives exactly 32 × fs rising edges/s; audio adds more. Rates around 200k/s with pulses under 25 ns mean a floating or unpowered line, not S/PDIF.
- **Corrupted captures (fixed):** any I2C transfer during a capture, including OLED redraws, garbles the rest of it. Captures hold `i2c_bus_lock()`, and the OLED takes it per frame. Raising the RMT interrupt priority is not possible: the LED's RMT TX channel shares the group interrupt at the default priority.

## PLL trim (`digi trim`, `digi trimtest`)

For Wi-Fi streaming: track the sender's clock by trimming the WM8804 PLL fraction K instead of resampling, so the output stays bit-perfect. 1 K LSB is about 0.04 ppm at 44.1 kHz.

| Test | 44.1 kHz | 48 kHz |
|---|---|---|
| Accuracy, ±100 ppm and ±1 ppm (BCLK over 1 s) | within 0.7 ppm | within 0.7 ppm |
| Static trim ±100 ppm, `digi spdif` | 0 errors | 0 errors |
| ±1 ppm steps, 40 captures (R3 only) | 2 sync errors, controls 1 | 0, controls 1 |
| ±100 ppm steps, 20 captures | 0–2 sync errors, controls 0–1 | 1–12, controls 0–1 |

- **R5 crossings glitch.** At 48 kHz the nominal K (0x1208a5) is about 75 ppm above an R5 boundary. Byte-wise writes then pass through a K about 2000 ppm off. No write order avoids it. A control loop needs hysteresis so it crosses an R5 boundary at most once.
- **Breadboard I2C corrupts some writes that still ACK.** Rewriting all three K bytes with the same values gave 26–88 sync errors in 40 captures; R3-only steps stayed clean. K registers don't read back, so a corrupted R4/R5 write goes unnoticed. In a control loop: write only changed bytes, and check BCLK after an R4/R5 write.
- **First BCLK reading was about 8 ppm high:** cold code between the count and the timestamp. A warm-up read fixed it.

## PCB checklist

- 2.2 kΩ pull-ups on SDA and SCL (1.8–4.7 kΩ).
- A solid ground plane under the I2S and I2C traces. Keep BCLK/LRCLK/DOUT away from SDA/SCL, or put ground between them.
- 22–33 Ω series resistors at the drivers of BCLK and LRCLK (WM8804 side) and DOUT (ESP32 side), to slow the edges and cut crosstalk.
- A way to reset the WM8804 without power cycling everything. Its I2C can latch SDA low after an ESP32 reset. Options: a GPIO-controlled load switch on the HAT 3V3 (and 5V), or a GPIO on the WM8804 reset line if the board exposes one (on this HAT it's an RC network).
- Optionally a separate I2C bus for the WM8804, so a latched SDA doesn't take the OLED with it.
- WM8804 TX0 (pin 17) to GPIO5 through a normally closed solder jumper and a 1 kΩ series resistor at the ESP32, for `digi spdif` / `digi decode` / `digi trimtest`. The resistor protects TX0 if GPIO5 is ever driven as an output.

## Next steps on the PCB

1. `digi probe`: the noise row should show 0/200.
2. `digi decode`: 440.0 Hz on both channels, 0 errors.
3. Reset the ESP32 a few times while the WM8804 runs and check that SDA no longer latches (`digi status` shows the chip, not the no-I2C fallback).
4. Then: real audio input (`digi_write`), and the rate switching `digi rate` offers.
