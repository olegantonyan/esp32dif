# esp32c5

ESP32-C5-DevKitC-1 firmware (ESP-IDF 6.1): on-board RGB LED, 0.96" SSD1306 I2C OLED, interactive shell on USB-Serial-JTAG.

## Build & run

```sh
make build
make flash-monitor              # TTY_PORT=/dev/ttyACM0 by default
```

Plug into the board's **USB** port (USB-Serial-JTAG, `/dev/ttyACM0`), not **UART** (USB-UART bridge, `/dev/ttyUSB0`, no console). Settings live in `sdkconfig.defaults`; `sdkconfig` is generated from it (`rm sdkconfig && idf.py set-target esp32c5` to regenerate).

## OLED wiring

0.96" 128x64 SSD1306 I2C module (4 pins: GND, VCC, SCL, SDA). All four wires go to header **J1**, the header with `3V3` at pin 1 and `RST` at pin 2 (pin numbers per the [DevKitC-1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/esp32-c5-devkitc-1/user_guide.html)):

| OLED pin | DevKitC-1 pin | Silkscreen | Signal |
|---|---|---|---|
| GND | J1 pin 15 | `G` | ground |
| VCC | J1 pin 1 | `3V3` | 3.3 V |
| SCL | J1 pin 10 | `9` | GPIO9, I2C SCL |
| SDA | J1 pin 9 | `8` | GPIO8, I2C SDA |

```
J1 pin  silkscreen          OLED
  1     3V3  ────────────── VCC
  2     RST
  3     2
  4     3
  5     0
  6     1
  7     6
  8     7
  9     8    ────────────── SDA
 10     9    ────────────── SCL
 11     10
 12     26
 13     25
 14     5V
 15     G    ────────────── GND
 16     NC
```

- Use 3V3, not 5V: most modules accept 5 V, but their pull-ups would then pull SDA/SCL to 5 V.
- Most modules have I2C pull-ups on board; the internal ones are enabled too.
- Address 0x3C. Modules with the address jumper moved are 0x3D.
- Address and height (128x32 for 0.91" modules): `idf.py menuconfig` → Board OLED. Pins: → Board I2C bus (shared with the Digi+).
- No display found at boot: logged as a warning, the board runs without it, `oled status` shows `not found`.

## Digi+ wiring (WM8804 S/PDIF HAT)

PiFi Digi+ 1.0, a HiFiBerry Digi+ clone: one WM8804 S/PDIF transmitter, I2S audio plus I2C control. HAT pins below are the physical pins on its 40-pin Raspberry Pi header.

| HAT pin | Pi function | DevKitC-1 pin | Silkscreen | Signal |
|---|---|---|---|---|
| 2 | 5V | J1 pin 14 | `5V` | 5 V, the HAT makes its own 3.3 V |
| 6 | GND | J1 pin 15 | `G` | ground |
| 3 | GPIO2, I2C SDA | J1 pin 9 | `8` | GPIO8, I2C SDA, shared with the OLED |
| 5 | GPIO3, I2C SCL | J1 pin 10 | `9` | GPIO9, I2C SCL, shared with the OLED |
| 12 | GPIO18, PCM CLK | J3 pin 4 | `24` | GPIO24, I2S BCLK, HAT → ESP |
| 35 | GPIO19, PCM FS | J3 pin 5 | `23` | GPIO23, I2S WS (LRCLK), HAT → ESP |
| 40 | GPIO21, PCM DOUT | J3 pin 8 | `4` | GPIO4, I2S DOUT, ESP → HAT |
| 39 | GND | J3 pin 1 | `G` | ground, next to the I2S wires |

- 3.3 V: with 5V connected, measure HAT pin 1 (3V3) against GND. About 0 V means the HAT has no regulator: wire J1 pin 1 (`3V3`) to HAT pin 1. An unpowered WM8804 clamps SDA low through its ESD diodes, and that also blocks the OLED.
- I2C pull-ups: add 2.2 kΩ from SDA and SCL to 3V3. The HAT relies on the Pi's 1.8 kΩ pull-ups; the ESP32 internal ones (about 45 kΩ) leave SDA so weak that BCLK crosstalk corrupts 40–90% of transfers while the WM8804 runs, and can latch SDA low until the HAT is power cycled. SCL is driven push-pull (no device on the bus stretches the clock), which roughly halves the errors on its own.
- I2C address 0x3B, so it can share the bus with the OLED (0x3C). The HAT straps the WM8804 to 2-wire software mode (GPO0/SWIFMODE tied to GND).
- The WM8804 must be the I2S clock master: set `AIF_MS`, R28 (0x1C) bit 6. Its PLL runs from the on-board crystal. In slave mode its S/PDIF transmitter would need an MCLK input, and the header doesn't carry one. Configure the ESP32 I2S as `I2S_ROLE_SLAVE` in Philips format.
- Check the device ID first: R0 = 0x05, R1 = 0x88. An RC network holds the WM8804 in reset at power-up, so the first accesses can NAK; retry after a short delay.
- Datasheet: Cirrus Logic WM8804 Rev 4.5.
- Pins, address, crystal: `idf.py menuconfig` → Board Digi+ (WM8804).

## Digi+

Bring-up findings, I2C noise measurements and a PCB checklist: [docs/digi-bringup.md](docs/digi-bringup.md).

At boot the firmware looks for the WM8804 at 0x3B for 0.5 s, checks its id (0x8805), resets it and starts it at 44.1 kHz: WM8804 PLL from the crystal, WM8804 as I2S clock master (BCLK 64 fs), ESP32 I2S0 as slave sending 32-bit slots (the WM8804 takes 24 bits), S/PDIF TX fed from the audio interface. Not found at boot: logged as a warning, `digi probe` retries. If the WM8804 doesn't answer on I2C but still clocks from an earlier setup (the ESP32 was reset, the WM8804 kept power), I2S starts anyway at the measured LRCLK rate, so audio plays. Silent at boot; menuconfig → Board Digi+ → Test tone at boot sets a tone (Hz, 0 = off). `digi tone` works anytime.

`digi probe` checks the connection step by step:

| Row | How | What it proves |
|---|---|---|
| lines | PCNT counts edges for 250 ms on BCLK, LRCLK (with and without a 1 µs glitch filter), SDA, SCL and DOUT released with a pull-down, before touching the chip | BCLK running = WM8804 powered and configured; DOUT silent = its wire goes to an input, not swapped with LRCLK |
| i2c | ACK at 0x3B, id 0x8805, revision | 3V3, GND, SDA, SCL |
| noise | 200 reads of R1 while the WM8804 drives BCLK/LRCLK | I2C survives the I2S clocks |
| bclk / lrclk | after a restart: rate and 64 × rate within 2000 ppm | HAT pins 12 and 35, PLL settings |
| crystal | config × measured BCLK / expected BCLK | crystal matches the config |
| glitches | unfiltered minus filtered LRCLK edges | the LRCLK wire is clean |

`digi spdif` checks the S/PDIF output itself: wire WM8804 pin 17 (TX0) or the optical transmitter's data input to J3 pin 9 (GPIO5, menuconfig → Board Digi+ → S/PDIF monitor). It counts rising edges with silence (32 × fs expected) and with a 440 Hz tone at 10%, then captures about 12 ms of the stream with RMT at 80 MHz and decodes it on the chip: biphase-mark cells, B/M/W preambles, parity, validity, the first 5 channel status bytes and the left/right samples. It reports the sample rate from the bit timing, the channel status (consumer, PCM, rate code), and the decoded tone's frequency and peak level.

`digi trim <ppm>` trims the WM8804 PLL fraction (±1000 ppm) and measures BCLK over 1 s. `digi trimtest <ppm>` checks trim accuracy, then compares S/PDIF captures right after trim steps against no-write and same-value-rewrite controls; `-w <s>` then toggles ±ppm for listening. Results: [docs/digi-bringup.md](docs/digi-bringup.md#pll-trim-digi-trim-digi-trimtest).

`digi decode` decodes any S/PDIF source on GPIO5 (for example another player wired to the same node) without checking it against the WM8804 setup, so two sources can be compared field by field. A source must share GND with the ESP32.

The data line (GPIO4 → HAT pin 40) has no direct readback: play `digi tone 440` and listen on the S/PDIF output. With no ACK the probe prints the idle SDA/SCL levels; a line held LOW means a device on the bus is clamping it.

Every WM8804 configuration starts with a software reset (retried up to 20 times): the reset stops BCLK/LRCLK, so the rest of the writes run on a quiet bus.

Swapped WS and DOUT wires: the ESP32 DOUT output fights the WM8804 LRCLK output. Symptoms: I2C NACKs and corrupted reads while the clocks run, SDA stuck low until the HAT is power cycled, LRCLK reading as noise (GPIO23 then sits on the floating DIN line), `DOUT released` showing 48000 edges/s. Fix the wires or swap the pins in menuconfig.

At boot the shared I2C bus is recovered first: if SDA is held low, SCL is clocked up to 9 times and a STOP is sent.

## Wi-Fi

`wifi connect <ssid> [password]` saves the network to NVS once it gets an IP; the board joins it on every boot. Reconnects back off after 3 quick retries: 5, 10, 20, 40, then every 60 s. Band `auto` (default) prefers a 5 GHz AP within 20 dB of the best 2.4 GHz one (menuconfig → Board network); `-b 5` or `-b 2.4` forces a band. Power save is off for low latency. The board takes its IP from DHCP and answers as `spdif.local` over mDNS.

## Wi-Fi audio (RTP)

The PC sends uncompressed PCM over RTP; the board plays it on the Digi+.

```sh
tools/spdif-sink                       # spdif.local, 48000 Hz, S24BE
tools/spdif-sink spdif.local 44100 S16BE
```

The script resolves `spdif.local` and runs a PipeWire client with `module-rtp-sink` (unicast to UDP 5004) and `module-rtp-sap` (announces the stream format to the board on UDP 9875). It adds an output device "S/PDIF over Wi-Fi"; pick it in the sound settings or `pw-play --target=spdif-rtp file.wav`. The node must be `media.class = "Audio/Sink"`: as a plain stream, WirePlumber links the microphone into it.

The script runs until stopped. It waits until the board resolves and answers ping, pings it every 10 s, and removes the sink after 3 misses (`SPDIF_CHECK_S`, `SPDIF_MISSES`), so the device disappears ~30 s after the board goes offline and returns ~10 s after it's back. If the board's DHCP address changes, the sink follows it. To run it as a user service that starts with PipeWire:

```sh
ln -s "$PWD/tools/spdif-sink" ~/.local/bin/spdif-sink
systemctl --user link "$PWD/tools/spdif-sink.service"
systemctl --user enable --now spdif-sink
journalctl --user -u spdif-sink -f
```

Limits: one fixed format (everything is resampled to it, so 44.1 kHz music isn't bit-perfect at the 48 kHz default), and PipeWire doesn't know the board's 250 ms buffer, so video players can't compensate for it.

- PC firewall: allow mDNS (`firewall-cmd --permanent --zone=public --add-service=mdns`, then `--reload`). Without it `spdif.local` doesn't resolve; pass the IP instead.
- The board plays only streams with a matching SAP announcement (L16 or L24 stereo), and switches the WM8804 to the announced rate.
- Buffer: target 250 ms (playback latency), in PSRAM: 1152 KB, 4 s at 48 kHz L24. Without PSRAM 64 KB of internal RAM. menuconfig → Board RTP receiver.
- Timeline from RTP timestamps: a timestamp gap (lost packets, sender skip) becomes silence, so the level holds; a jump past the target (PipeWire re-anchoring its clock) rebases without silence. After an underrun, audio that arrives late is dropped, so the level recovers at once instead of rebuffering.
- Drift: a PI loop holds the buffer level (EMA of each second's peak, which Wi-Fi bursts can't push up) at target. It trims the WM8804 PLL (bit-perfect), at most once per 10 s in ≥ 5 ppm steps, clamps changes of K bits 21-16 to one per minute (those glitch, see [docs/digi-bringup.md](docs/digi-bringup.md#pll-trim-digi-trim-digi-trimtest)), and rewrites all K bytes every minute. Without I2C (3 failed writes, or audio started without I2C) it repeats or drops single frames instead, and retries the PLL every 5 min.
- `rtp status`: source, SAP session, format, loss, jitter, longest gap, timestamp rebases, buffer level and target, underruns, late audio dropped, PLL trim, sender clock offset, sample slip.
- PC: rtp-sink stamps packets with its driver's clock position. When a client links the sink with an ALSA device (the Plasma applet's level meters do), PipeWire moves the sink to that device's driver: the RTP timestamps jump, rtp-sink resyncs and drops audio (an audible stall), and waking a suspended device stalls the graph for up to 230 ms more. `spdif-sink` sets `node.group = "pipewire.dummy"`, which keeps the sink on the Dummy-Driver. The Plasma applet lists the sink only with "Show virtual devices" on.

## Hardware

| Function | GPIO | Header |
|---|---|---|
| RGB LED (WS2812) | 27 | J3 pin 7 |
| OLED SDA / SCL | 8 / 9 | J1 pin 9 / 10 |
| Digi+ I2S BCLK / WS / DOUT | 24 / 23 / 4 | J3 pin 4 / 5 / 8 |
| BOOT button | 28 | J3 pin 11 |
| USB D- / D+ | 13 / 14 | J3 pin 14 / 13 |
| UART0 TX / RX | 11 / 12 | J3 pin 2 / 3 |

Avoid for new wiring: strapping pins 2, 3, 7, 25, 26, 27, 28; flash/PSRAM 16–22 (and 15 on PSRAM modules); 0/1 if a 32 kHz crystal is fitted.

## RGB LED

Off by default. `led color|blink|breathe|rainbow|off` switch to manual mode; `led auto` returns to the default pattern. Pin: `idf.py menuconfig` → Board LED. Settings are RAM-only.

## OLED

Shows a status screen by default, refreshed every second:

```
48kHz 24-bit
rtp 102ms buf
192.168.0.21
up 0d 01:02:03
led off
heap 312 KB free
```

Rows: Digi+ format; source (`tone …`, `rtp <ms> buf`, `rtp buffering`, `idle`; `noI2C` when audio runs without I2C; `S/PDIF off` / `no WM8804` without the chip); IP address or Wi-Fi state; uptime. LED and heap rows only on 128x64.

Text: 5x7 font, 21 columns × 8 rows (4 on 128x32). `oled print|clear` switch to manual mode; `oled auto` returns to the status screen. Settings are RAM-only.

## Shell

Built on ESP-IDF `esp_console`: line editing, history, Tab completion, `help [cmd]`.

| Command | Description |
|---|---|
| `sysinfo` | chip, revision, cores, features, flash, MAC addresses |
| `version` | app version, ESP-IDF version, build time, ELF SHA |
| `uptime` | time since boot, last reset reason |
| `reboot [-d ms]` | restart the chip |
| `free [-h]` | heap per memory region |
| `ps` | FreeRTOS tasks: state, priority, core, stack high-water mark, CPU % since boot |
| `led status` | mode, effect, color, period, brightness |
| `led color <color>` | solid color; `<color>` = preset name, `#rrggbb` or `r,g,b` |
| `led blink\|breathe <color> [-p ms]` | blink (default 500 ms) or fade (default 3000 ms) |
| `led rainbow [-p ms]` | hue cycle, default 5000 ms |
| `led off` / `led auto` | turn off / back to the default (off) |
| `led brightness <0-100>` | global brightness, default 20 |
| `led presets` | list color names and effects |
| `wifi scan` | nearby networks: ssid, bssid, channel, band, rssi, auth |
| `wifi connect [ssid] [password] [-b 2.4\|5\|auto] [-n] [-t s]` | join; saved once it gets an IP; no ssid = saved network |
| `wifi status` / `saved [-p]` / `forget` / `disconnect` | state, signal, IP / saved network / erase it / leave |
| `rtp status` / `rtp reset` | RTP receiver stats / clear them and rebuffer |
| `oled status` | display found or not, pins, address, mode, power, contrast |
| `oled print <row> <text>` | write text to a row (0-based), longer text is cut at 21 chars |
| `oled clear` | blank the screen |
| `oled auto` | back to the status screen |
| `oled on` / `oled off` | display on / sleep |
| `oled contrast <0-255>` | contrast, default 127 |
| `digi status` | chip found or not, crystal, rate, pins, tone |
| `digi probe` | I2C, chip id, crystal and I2S clock check (see Digi+) |
| `digi reg <reg> [value]` | read a WM8804 register, or write it; only R0–R2 and R11–R17 read back |
| `digi rate <hz>` | 32000, 44100 (default), 48000, 88200, 96000, 176400, 192000 |
| `digi tone <hz> [-l 0-100]` | sine on both channels, level in % of full scale, default 10 |
| `digi stop` | stop the tone |
| `digi spdif` | capture and decode the S/PDIF stream on the monitor GPIO: rate, frames, channel status, tone frequency and level |
| `digi decode` | decode any S/PDIF stream on the monitor GPIO, no WM8804 needed: edges, levels with pulls, cell widths, rate, channel status, per-channel tone and level, sample bits |

## Layout

```
main/main.c              led_init → oled_init → digi_init → net_init → rtp_init → shell_init → register command groups → shell_start
main/status_led.c        default LED pattern (auto mode)
main/status_oled.c       status screen (auto mode)
components/led/          RGB LED driver (espressif/led_strip over RMT), effects, color presets
components/cmd_led/      led
components/oled/         SSD1306 over I2C (esp_lcd), text framebuffer, 5x7 font, refresh task
components/cmd_oled/     oled
components/i2c_bus/      shared I2C master bus (OLED, WM8804), bus recovery at start
components/digi/         WM8804 over I2C (PLL, clock master, S/PDIF TX), I2S slave, test tone, PCNT clock probe
components/cmd_digi/     digi
components/net/          Wi-Fi client: NVS-saved network, reconnect backoff, 5 GHz preference, DHCP hostname, mDNS
components/cmd_wifi/     wifi
components/rtp/          RTP receiver, SAP/SDP parser, playback ring buffer feeding the Digi+
components/cmd_rtp/      rtp
tools/spdif-sink         PC side: PipeWire RTP sink + SAP announcement to the board, follows the board coming and going
tools/spdif-sink.service systemd user unit for it
components/shell/        REPL setup + helpers: shell_args_ok, shell_subcmd_run, shell_getc
components/cmd_system/   sysinfo, version, uptime, reboot, free, ps
docs/digi-bringup.md     Digi+ breadboard bring-up notes
```

## Adding commands

Create `components/cmd_<feature>/` exposing `esp_err_t cmd_<feature>_register(void)`, register handlers with `esp_console_cmd_register()`, and call it from `main.c` before `shell_start()`.

- Flags: an argtable struct + `shell_args_ok()` (see `reboot` in `cmd_sys.c`).
- Subcommands (`foo bar ...`): a `shell_subcmd_t` table + `shell_subcmd_run()` (see `cmd_oled.c`).
- Long-running loops: poll `shell_getc()` and stop on `SHELL_CTRL_C`.
