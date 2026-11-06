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
- Pins, address and height (128x32 for 0.91" modules): `idf.py menuconfig` → Board OLED.
- No display found at boot: logged as a warning, the board runs without it, `oled status` shows `not found`.

## Hardware

| Function | GPIO | Header |
|---|---|---|
| RGB LED (WS2812) | 27 | J3 pin 7 |
| OLED SDA / SCL | 8 / 9 | J1 pin 9 / 10 |
| BOOT button | 28 | J3 pin 11 |
| USB D- / D+ | 13 / 14 | J3 pin 14 / 13 |
| UART0 TX / RX | 11 / 12 | J3 pin 2 / 3 |

Avoid for new wiring: strapping pins 2, 3, 7, 25, 26, 27, 28; flash/PSRAM 16–22 (and 15 on PSRAM modules); 0/1 if a 32 kHz crystal is fitted.

## RGB LED

Off by default. `led color|blink|breathe|rainbow|off` switch to manual mode; `led auto` returns to the default pattern. Pin: `idf.py menuconfig` → Board LED. Settings are RAM-only.

## OLED

Shows a status screen by default, refreshed every second:

```
ESP32-C5
up 0d 01:02:03
led off
heap 312 KB free
```

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
| `oled status` | display found or not, pins, address, mode, power, contrast |
| `oled print <row> <text>` | write text to a row (0-based), longer text is cut at 21 chars |
| `oled clear` | blank the screen |
| `oled auto` | back to the status screen |
| `oled on` / `oled off` | display on / sleep |
| `oled contrast <0-255>` | contrast, default 127 |

## Layout

```
main/main.c              led_init → oled_init → shell_init → register command groups → shell_start
main/status_led.c        default LED pattern (auto mode)
main/status_oled.c       status screen (auto mode)
components/led/          RGB LED driver (espressif/led_strip over RMT), effects, color presets
components/cmd_led/      led
components/oled/         SSD1306 over I2C (esp_lcd), text framebuffer, 5x7 font, refresh task
components/cmd_oled/     oled
components/shell/        REPL setup + helpers: shell_args_ok, shell_subcmd_run, shell_getc
components/cmd_system/   sysinfo, version, uptime, reboot, free, ps
```

## Adding commands

Create `components/cmd_<feature>/` exposing `esp_err_t cmd_<feature>_register(void)`, register handlers with `esp_console_cmd_register()`, and call it from `main.c` before `shell_start()`.

- Flags: an argtable struct + `shell_args_ok()` (see `reboot` in `cmd_sys.c`).
- Subcommands (`foo bar ...`): a `shell_subcmd_t` table + `shell_subcmd_run()` (see `cmd_oled.c`).
- Long-running loops: poll `shell_getc()` and stop on `SHELL_CTRL_C`.
