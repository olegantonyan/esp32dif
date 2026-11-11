# Internals

Technical details moved out of the README. Bring-up history and measurements: [digi-bringup.md](digi-bringup.md).

## Digi+ startup

At boot the firmware first recovers the shared I2C bus: if SDA is held low, it clocks SCL up to 9 times and sends a STOP.

Then it looks for the WM8804 at 0x3B for 0.5 s, checks the ID (0x8805), resets the chip and starts it at 44.1 kHz:
- WM8804 PLL runs from the crystal.
- WM8804 is the I2S clock master (BCLK = 64 fs).
- ESP32 I2S0 is the slave and sends 32-bit slots; the WM8804 uses 24 bits.
- The S/PDIF transmitter takes audio from the audio interface.

If the chip is not found, the firmware logs a warning; `digi probe` tries again. If the WM8804 does not answer on I2C but still sends clocks from an earlier setup (the ESP32 was reset, the WM8804 kept power), I2S starts anyway at the measured LRCLK rate, so audio still plays.

Every WM8804 setup starts with a software reset (up to 20 tries). The reset stops BCLK/LRCLK, so the other register writes run on a quiet bus.

Output is silent at boot. menuconfig → Board Digi+ → Test tone at boot sets a tone (Hz, 0 = off).

## Digi+ wiring notes

- Power: the HAT makes its own 3.3 V from 5V. Wire only 5V and GND; leave HAT pin 1 (3V3) open. A WM8804 without power pulls SDA low through its ESD diodes, and this also blocks the OLED.
- I2C pull-ups: add 2.2 kΩ from SDA and SCL to 3V3. The HAT expects the Pi's 1.8 kΩ pull-ups. The ESP32 internal pull-ups (about 45 kΩ) are too weak: BCLK crosstalk breaks 40–90 % of transfers while the WM8804 runs, and SDA can stay low until the HAT is power cycled. SCL is driven push-pull (no device stretches the clock); this alone halves the errors.
- The HAT straps the WM8804 to 2-wire software mode (GPO0/SWIFMODE to GND), address 0x3B.
- The WM8804 must be the I2S clock master (`AIF_MS`, R28 = 0x1C bit 6). In slave mode its S/PDIF transmitter needs an MCLK input, and the header has none. The ESP32 I2S is `I2S_ROLE_SLAVE`, Philips format.
- Check the device ID first: R0 = 0x05, R1 = 0x88. An RC network holds the WM8804 in reset at power-up, so the first accesses can NAK; retry after a short delay.
- Datasheet: Cirrus Logic WM8804 Rev 4.5.
- Swapped WS and DOUT wires: the ESP32 DOUT output fights the WM8804 LRCLK output. Symptoms: I2C NACKs and bad reads while clocks run, SDA stuck low until the HAT is power cycled, LRCLK reads as noise, `DOUT released` shows 48000 edges/s. Fix the wires or swap the pins in menuconfig.
- The data line (GPIO4 → HAT pin 40) cannot be read back: play `digi tone 440` and listen. Without ACK, `digi probe` prints the idle SDA/SCL levels; a line held low means a device on the bus clamps it.

## Diagnostic commands

`digi probe` checks the connection step by step:

| Row | How | What it proves |
|---|---|---|
| lines | PCNT counts edges for 250 ms on BCLK, LRCLK (with and without a 1 µs glitch filter), SDA, SCL and DOUT (released, with a pull-down), before it touches the chip | BCLK running = WM8804 has power and a config; DOUT silent = its wire goes to an input, not swapped with LRCLK |
| i2c | ACK at 0x3B, ID 0x8805, revision | 5V, GND, SDA, SCL |
| noise | 200 reads of R1 while the WM8804 drives BCLK/LRCLK | I2C works with the I2S clocks running |
| bclk / lrclk | after a restart: rate and 64 × rate within 2000 ppm | HAT pins 12 and 35, PLL settings |
| crystal | config × measured BCLK / expected BCLK | crystal matches the config |
| glitches | unfiltered minus filtered LRCLK edges | the LRCLK wire is clean |

`digi spdif` checks the S/PDIF output. Connect WM8804 pin 17 (TX0), or the data input of the optical transmitter, to J3 pin 9 (GPIO5, menuconfig → Board Digi+ → S/PDIF monitor). It counts rising edges with silence (32 × fs expected) and with a 440 Hz tone at 10 %. Then it captures about 12 ms with RMT at 80 MHz and decodes it: biphase-mark cells, B/M/W preambles, parity, validity, the first 5 channel status bytes and the samples. It reports the rate from the bit timing, the channel status (consumer, PCM, rate code), and the tone frequency and peak level.

`digi decode` decodes any S/PDIF source on GPIO5 (for example another player on the same node) without comparing it to the WM8804 setup, so you can compare two sources field by field. The source must share GND with the ESP32.

`digi reg <reg> [value]` reads or writes a WM8804 register. Only R0–R2 and R11–R17 can be read back.

`digi trim <ppm>` changes the WM8804 PLL fraction (±1000 ppm) and measures BCLK over 1 s. `digi trimtest <ppm>` checks trim accuracy, then compares S/PDIF captures right after trim steps with no-write and same-value controls; `-w <s>` then switches ±ppm for listening. Results: [digi-bringup.md](digi-bringup.md#pll-trim-digi-trim-digi-trimtest).

## RTP receiver

- Timeline from RTP timestamps: a gap (lost packets, sender skip) becomes silence, so the buffer level stays the same. A jump past the target (PipeWire re-anchors its clock) rebases without silence. After an underrun, late audio is dropped, so the level recovers at once without rebuffering.
- Drift: a PI loop holds the buffer level at the target. The level is an EMA of each second's peak, so Wi-Fi bursts can't push it up. The loop trims the WM8804 PLL (bit-perfect), at most once per 10 s, in steps of 5 ppm or more. Changes of K bits 21–16 are limited to one per minute because they glitch (see [digi-bringup.md](digi-bringup.md#pll-trim-digi-trim-digi-trimtest)); all K bytes are rewritten every minute. Without I2C (3 failed writes, or audio started without I2C) it repeats or drops single frames instead and tries the PLL again every 5 min.
- Sender auth: the board keeps up to 4 sender IPs, each valid for 60 s after `POST /api/rtp/allow` (menuconfig → Board RTP receiver). Packets from other IPs are dropped before parsing and counted as `denied`.

## PC side

`esp32dif-sink` runs a PipeWire client with `module-rtp-sink` (unicast to UDP 5004) and `module-rtp-sap` (announces the format to the board on UDP 9875). The node must be `media.class = "Audio/Sink"`; as a plain stream, WirePlumber links the microphone into it.

rtp-sink stamps packets with its driver's clock position. When a client links the sink with an ALSA device (the Plasma applet's level meters do this), PipeWire moves the sink to that device's driver: the RTP timestamps jump, rtp-sink resyncs and drops audio (you hear a stall), and waking a suspended device can stall the graph for up to 230 ms more. `esp32dif-sink` sets `node.group = "pipewire.dummy"`, which keeps the sink on the Dummy-Driver. Because of this, the Plasma applet shows the sink only with "Show virtual devices" on.

Remote buttons: the sink opens TCP 5005 to the board and sends the password line; the board answers `ok` or `denied`, then sends one line per button press (`previous`, `play-pause`, `next`) and `ping` every 30 s. The sink reconnects after 75 s without a line. NEC repeat frames and the same key again within 300 ms are ignored. If a connected PC is the current RTP source, only it gets the action. The sink calls the MPRIS `Previous`/`PlayPause`/`Next` methods with `busctl --user`.

## Firmware layout

```
main/main.c                 settings → ota → led/oled/ir/digi → net → rtp → shell → commands → web → telnet → remote → shell_start
main/status_led.c           default LED pattern
main/status_oled.c          OLED status screen
components/settings/        hostname and password in NVS (also does NVS init)
components/net/             Wi-Fi client, saved network, reconnect, 5 GHz preference, mDNS, fallback AP
components/rtp/             RTP receiver, sender auth, SAP/SDP parser, playback buffer
components/digi/            WM8804 (PLL, clock master, S/PDIF TX), I2S slave, test tone, clock probe, S/PDIF decoder
components/i2c_bus/         shared I2C bus (OLED, WM8804), bus recovery
components/oled/            SSD1306 driver, text framebuffer, 5x7 font
components/led/             RGB LED driver and effects
components/ir/              IR receiver: GPIO edge timestamps, NEC/Samsung decoder
components/remote/          IR keymap (keymap.c) and TCP 5005 server that sends button actions to the sink
components/web/             HTTP server, Basic auth, embedded index.html, JSON API, firmware upload
components/telnet/          telnet server: login, line editor, per-task stdio (needs newlib)
components/ota/             OTA writes, image checks, 60 s mark-valid timer, rollback
components/shell/           USB console, shell_exec (one command at a time), helpers
components/cmd_*/           shell commands: system, wifi, rtp, config, ota, oled, led, ir, digi
partitions.csv              nvs, otadata, phy_init, two 3 MB app slots
pipewire/esp32dif-sink      PC side: PipeWire RTP sink + SAP announcement
pipewire/esp32dif-sink.service systemd user unit
```

## Adding commands

Create `components/cmd_<feature>/` with `esp_err_t cmd_<feature>_register(void)`. Register handlers with `esp_console_cmd_register()` and call it from `main.c` before `shell_start()`.

- Flags: an argtable struct + `shell_args_ok()` (see `reboot` in `cmd_sys.c`).
- Subcommands (`foo bar ...`): a `shell_subcmd_t` table + `shell_subcmd_run()` (see `cmd_oled.c`).
- Long loops: check `shell_getc()` and stop on `SHELL_CTRL_C`.
- Output: use `printf`/`stdout`. A command runs in the session's task, so its output goes to USB or telnet. Output from other tasks always goes to USB.
