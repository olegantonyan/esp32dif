# esp32dif

Wi-Fi to S/PDIF receiver. The PC sends audio over Wi-Fi, and an ESP32-C5-DevKitC-1 plays it on a PiFi Digi+ (WM8804) S/PDIF board.

- Lossless 16/24-bit PCM, 32–192 kHz, bit-perfect when PipeWire does not change the audio.
- Web UI, telnet and USB shell for setup and diagnostics.
- Firmware updates over Wi-Fi (OTA) with automatic rollback.
- Status on a small OLED screen.

Firmware: ESP-IDF 6.1. Technical details: [docs/internals.md](docs/internals.md). Digi+ bring-up notes: [docs/digi-bringup.md](docs/digi-bringup.md).

## Build and flash

```sh
make build
make flash-monitor   # first time, over USB; TTY_PORT=/dev/ttyACM0 by default
make ota             # later, over Wi-Fi; ESP_HOST=esp32dif.local, ESP_PASSWORD=esp32dif-admin by default
```

Use the board's **USB** port (`/dev/ttyACM0`), not **UART** (`/dev/ttyUSB0` has no console).

Build settings are in `sdkconfig.defaults`. To create `sdkconfig` again: `rm sdkconfig && idf.py set-target esp32c5`.

## First setup

1. Flash the board over USB.
2. With no saved Wi-Fi, the board starts its own Wi-Fi network (access point) `esp32dif`. The password is on the OLED (default `esp32dif-admin`).
3. Join it from a phone or PC and open `http://192.168.4.1`. Log in as `admin` with the same password.
4. Pick your Wi-Fi network and press **Connect**. The board saves it after it gets an IP address.
5. Open `http://esp32dif.local` from your home network. The board's access point turns off 2 minutes after the connection works.

On the USB shell you can do the same with `wifi connect <ssid> <password>`.

## Access and settings

One password protects the web UI, telnet, the board's access point and audio senders. The USB shell has no password.

| Way | How |
|---|---|
| Web UI | `http://esp32dif.local` (or `http://192.168.4.1` on the access point), user `admin`. Status, Wi-Fi, hostname, password, firmware upload, reboot. |
| Telnet | `telnet esp32dif.local`. Same commands as USB, one session at a time, logout after 10 minutes without input. |
| USB | `make monitor` |

Change the hostname and password in the web UI, or with `config hostname <name>` and `config password <pw>`. The board saves them and uses them at once. The OLED always shows the current password, so you can't lock yourself out.

The password travels as plain text over HTTP and telnet. This is fine on a home network, but not on networks you don't trust.

## Wi-Fi

- The board connects to the saved network at every boot. If the connection drops, it tries again: 3 times at once, then after 5, 10, 20 and 40 s, then every 60 s.
- Band `auto` (default) prefers 5 GHz. Use `wifi connect -b 5` or `-b 2.4` to force a band.
- Power save is off, for low latency.
- The board gets its IP address from DHCP and answers as `<hostname>.local` (mDNS).
- **Access point:** the board starts its own network (named after the hostname, WPA2, the device password, `192.168.4.1`) when no network is saved, after `wifi disconnect`, or after 60 s without an IP address. It turns off 2 minutes after the home connection works again.

## Firmware updates (OTA)

`make ota`, or upload `build/esp32dif.bin` in the web UI. The board has two firmware slots. A new firmware is confirmed 60 s after it starts. If the board resets before that, it starts the old firmware again. `ota status` shows both slots; `ota rollback` goes back to the old firmware.

The first flash with this partition layout must be over USB (`make flash`). The saved Wi-Fi is kept.

If you rename the project, flash over USB too: OTA accepts only firmware with the same project name.

## Playing audio from the PC

The PC runs `tools/esp32dif-sink`. It adds an output device "S/PDIF over Wi-Fi" to PipeWire. Select it in the sound settings, or play a file with `pw-play --target=esp32dif-rtp file.wav`.

```sh
tools/esp32dif-sink                              # esp32dif.local, 48000 Hz, S24BE
tools/esp32dif-sink esp32dif.local 44100 S24BE   # host, rate, format
```

- **Password:** every 10 s the script logs in to the board with `SPDIF_PASSWORD` (default `esp32dif-admin`). The board plays audio only from computers that logged in during the last 60 s. With a wrong password, the script prints an error and doesn't create the device.
- **Board offline:** the device disappears about 30 s after the board stops answering, and comes back about 10 s after the board returns. If the board's IP address changes, the script follows it.
- **Firewall:** allow mDNS on the PC (`firewall-cmd --permanent --zone=public --add-service=mdns`, then `firewall-cmd --reload`). Without it, `esp32dif.local` doesn't work; use the IP address instead.

### Run it as a service

Install the package (below), or link the files from this folder:

```sh
ln -s "$PWD/tools/esp32dif-sink" ~/.local/bin/esp32dif-sink
systemctl --user link "$PWD/tools/esp32dif-sink.service"
systemctl --user enable --now esp32dif-sink
journalctl --user -u esp32dif-sink -f
```

Settings go in `~/.config/esp32dif-sink.conf`, for example:

```sh
SPDIF_HOST=esp32dif.local
SPDIF_RATE=44100
SPDIF_FORMAT=S24BE
SPDIF_PASSWORD=esp32dif-admin
```

Run `chmod 600` on this file, because it has the password. Apply changes with `systemctl --user restart esp32dif-sink`. When you change the board's hostname or password, change them here too.

Other settings: `SPDIF_PORT` (5004), `SPDIF_SAP_PORT` (9875), `SPDIF_CHECK_S` (10), `SPDIF_MISSES` (3).

### Package

`omnipackage build --distros opensuse_tumbleweed` builds an RPM, DEB or pacman package with the script and the service (config in `.omnipackage/`). It needs `pipewire`, `curl` and `nss-mdns`. The service is enabled for all users and starts at the next login. To start it now: `systemctl --user daemon-reload && systemctl --user start esp32dif-sink`. To turn it off for your user: `systemctl --user mask esp32dif-sink`.

### Sound quality

For bit-perfect output:
- set the device volume to 100 %,
- play nothing else at the same time,
- use the same rate in the file, the script and the PipeWire graph. The default PipeWire graph runs only at 48 kHz. Add `default.clock.allowed-rates = [ 44100 48000 ]` to allow 44.1 kHz too, then run the script with `44100`.

Measured with `digi decode -d` (24-bit noise and −3 dBFS sines, compared with the file):

| Path | Result |
|---|---|
| 48 kHz file → 48 kHz sink, 100 % volume | bit-perfect (269/269 frames) |
| same at 90 % volume | 0 frames exact |
| 44.1 kHz file → 44.1 kHz graph and sink | bit-perfect (270/270 frames) |
| 44.1 kHz → 48 kHz (resampled), 997 Hz / 15 kHz | SINAD 138.1 / 138.8 dB, error −144 dBFS |
| 48 kHz → 44.1 kHz (resampled), 15 kHz | SINAD 139.6 dB |
| native, 997 Hz (24-bit limit) | SINAD 143 dB |

How it works:
- The board plays a stream only when it has a matching SAP announcement (L16 or L24 stereo). It switches the WM8804 to the announced rate.
- The buffer adds 250 ms of latency. Video players don't know about it, so video and sound can be out of sync. The buffer is in PSRAM (1152 KB, 4 s at 48 kHz 24-bit), or 64 KB of internal RAM without PSRAM.
- The board slowly adjusts its clock to the PC's clock, so the buffer never runs empty or full. This doesn't change the audio data.
- `rtp status` shows senders, format, lost packets, jitter, buffer level, underruns and clock adjustment.

## Wiring

### Pins

| Function | GPIO | Header |
|---|---|---|
| RGB LED (WS2812) | 27 | J3 pin 7 |
| OLED and Digi+ I2C SDA / SCL | 8 / 9 | J1 pin 9 / 10 |
| Digi+ I2S BCLK / WS / DOUT | 24 / 23 / 4 | J3 pin 4 / 5 / 8 |
| S/PDIF monitor (optional, for `digi spdif`) | 5 | J3 pin 9 |
| BOOT button | 28 | J3 pin 11 |
| USB D- / D+ | 13 / 14 | J3 pin 14 / 13 |
| UART0 TX / RX | 11 / 12 | J3 pin 2 / 3 |

Don't use for new wiring: strapping pins 2, 3, 7, 25, 26, 27, 28; flash/PSRAM pins 16–22 (and 15 on PSRAM modules); 0 and 1 if a 32 kHz crystal is fitted.

Pins and I2C addresses can be changed in `idf.py menuconfig` (Board OLED, Board I2C bus, Board Digi+, Board LED).

### OLED

0.96" 128x64 SSD1306 I2C module. All four wires go to header **J1** (the header with `3V3` at pin 1, see the [DevKitC-1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/esp32-c5-devkitc-1/user_guide.html)):

| OLED pin | DevKitC-1 pin | Label on board |
|---|---|---|
| GND | J1 pin 15 | `G` |
| VCC | J1 pin 1 | `3V3` |
| SCL | J1 pin 10 | `9` |
| SDA | J1 pin 9 | `8` |

- Use 3V3, not 5V. With 5V the module's pull-ups would pull SDA/SCL to 5 V.
- I2C address 0x3C (0x3D if the address jumper is moved). For 0.91" 128x32 modules, change the height in menuconfig.
- The board also works without the display.

### Digi+ (WM8804 S/PDIF HAT)

PiFi Digi+ 1.0, a HiFiBerry Digi+ clone. HAT pins are the physical pins of its 40-pin Raspberry Pi header.

| HAT pin | DevKitC-1 pin | Label on board | Signal |
|---|---|---|---|
| 2 | J1 pin 14 | `5V` | 5 V (the HAT makes its own 3.3 V) |
| 6 | J1 pin 15 | `G` | ground |
| 3 | J1 pin 9 | `8` | I2C SDA, shared with the OLED |
| 5 | J1 pin 10 | `9` | I2C SCL, shared with the OLED |
| 12 | J3 pin 4 | `24` | I2S BCLK, HAT → ESP |
| 35 | J3 pin 5 | `23` | I2S WS (LRCLK), HAT → ESP |
| 40 | J3 pin 8 | `4` | I2S DOUT, ESP → HAT |
| 39 | J3 pin 1 | `G` | ground, next to the I2S wires |

- **Add 2.2 kΩ pull-ups** from SDA and SCL to 3V3. Without them, I2C fails while audio plays.
- Check that HAT pin 1 has 3.3 V. If it shows about 0 V, connect J1 pin 1 (`3V3`) to HAT pin 1.
- Run `digi probe` to check the wiring step by step. More notes: [docs/internals.md](docs/internals.md#digi-wiring-notes).

## OLED screen

```
48kHz 24-bit         format
rtp 102ms buf        source: rtp, tone, idle; noI2C = no I2C to the WM8804
192.168.0.21         IP address or Wi-Fi state
esp32dif.local       hostname
AP off               board's access point: off, or on with number of clients
pw esp32dif-admin    password (first 18 characters)
up 0d 01:02:03       uptime
heap 312 KB free     free memory
```

On 128x32 displays: format, source, IP address, password.

## Shell commands

The same commands work on USB and telnet. Line editing, history, Tab completion and `help [cmd]` are available.

| Command | What it does |
|---|---|
| `config show [-p]` | hostname, password (hidden without `-p`), access point |
| `config hostname <name>` | set hostname: a-z, 0-9, `-`, up to 32 characters |
| `config password <pw>` | set password: 8–63 characters |
| `wifi scan` | list nearby networks |
| `wifi connect [ssid] [password] [-b 2.4\|5\|auto] [-n] [-t s]` | join a network; it is saved after it gets an IP (`-n`: don't save). Without ssid: the saved network |
| `wifi status` | connection, signal, IP address, access point |
| `wifi saved [-p]` / `forget` / `disconnect` | show / delete the saved network / disconnect |
| `rtp status` / `rtp reset` | audio receiver statistics / reset them |
| `ota status` / `ota rollback` | firmware slots / go back to the old firmware |
| `digi status` | WM8804 found, crystal, rate, pins, tone |
| `digi probe` | check the Digi+ wiring step by step |
| `digi rate <hz>` | 32000, 44100 (default), 48000, 88200, 96000, 176400, 192000 |
| `digi tone <hz> [-l 0-100]` / `digi stop` | play a test sine (level in %, default 10) / stop it |
| `digi reg <reg> [value]` | read or write a WM8804 register |
| `digi spdif` | check the S/PDIF output on the monitor pin |
| `digi decode [-d]` | decode any S/PDIF signal on the monitor pin (`-d`: print all frames) |
| `digi trim <ppm>` / `digi trimtest <ppm>` | adjust the WM8804 clock / test clock adjustment |
| `oled status` / `on` / `off` / `contrast <0-255>` | display state / power / contrast |
| `oled print <row> <text>` / `clear` / `auto` | write text / clear / back to the status screen |
| `led color <color>` | color: name, `#rrggbb` or `r,g,b` (`led presets` lists names) |
| `led blink\|breathe <color> [-p ms]` / `led rainbow [-p ms]` | effects |
| `led off` / `led auto` / `led brightness <0-100>` / `led status` | LED control (off by default) |
| `sysinfo` / `version` / `uptime` | chip, firmware and uptime information |
| `free [-h]` / `ps` | memory / tasks |
| `reboot [-d ms]` | restart |

LED and OLED changes made with commands are not saved; they reset at reboot.
