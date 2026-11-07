#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "digi.h"
#include "shell.h"
#include "cmd_digi.h"

#define TOLERANCE_PPM 2000
#define SPDIF_IDLE_RISING_PER_FRAME 32
#define SPDIF_AUDIO_MIN_EXTRA_PER_FRAME 4
#define SPDIF_TONE_SETTLE_MS 50
#define SPDIF_TONE_HZ 440
#define SPDIF_TONE_LEVEL 10
#define TRIM_MAX_PPM 1000
#define TRIM_MEASURE_MS 1000
#define TRIM_SETTLE_MS 50
#define TRIM_CAPTURES 10
#define TRIM_WOBBLE_MS 100
#define TRIM_TOLERANCE_PPB 5000
#define TRIM_TOLERANCE_SHARE 100

static const char *TAG = "cmd_digi";

static struct {
    struct arg_int *hz;
    struct arg_end *end;
} s_rate_args;

static struct {
    struct arg_int *reg;
    struct arg_int *val;
    struct arg_end *end;
} s_reg_args;

static struct {
    struct arg_int *hz;
    struct arg_int *level;
    struct arg_end *end;
} s_tone_args;

static struct {
    struct arg_dbl *ppm;
    struct arg_end *end;
} s_trim_args;

static struct {
    struct arg_dbl *ppm;
    struct arg_int *captures;
    struct arg_int *wobble;
    struct arg_end *end;
} s_trimtest_args;

static struct {
    struct arg_lit *dump;
    struct arg_end *end;
} s_decode_args;

static bool ready(void)
{
    digi_state_t st;
    digi_get(&st);
    if (!st.ready) {
        fprintf(stderr, "digi: no WM8804, run 'digi probe'\n");
    }
    return st.ready;
}

static bool audio_ready(void)
{
    digi_state_t st;
    digi_get(&st);
    if (!st.ready && !st.blind) {
        fprintf(stderr, "digi: no WM8804, run 'digi probe'\n");
    }
    return st.ready || st.blind;
}

static const char *ppb_str(char buf[16], int32_t ppb)
{
    unsigned long mag = labs((long)ppb);
    snprintf(buf, 16, "%c%lu.%03lu", ppb < 0 ? '-' : '+', mag / 1000, mag % 1000);
    return buf;
}

static int cmd_status(int argc, char **argv)
{
    digi_state_t st;
    digi_get(&st);
    if (st.ready) {
        printf("chip     WM8804 rev %u at 0x%02x\n", st.rev, CONFIG_APP_DIGI_ADDR);
    } else if (st.blind) {
        printf("chip     no I2C answer at 0x%02x, but the WM8804 is clocking from an earlier setup: audio runs, 'digi probe' retries I2C\n", CONFIG_APP_DIGI_ADDR);
    } else {
        printf("chip     not found at 0x%02x, run 'digi probe'\n", CONFIG_APP_DIGI_ADDR);
    }
    printf("crystal  %d Hz (config)\n", CONFIG_APP_DIGI_XTAL_HZ);
    printf("format   %lu Hz, I2S 24-bit, WM8804 is clock master\n", (unsigned long)st.rate);
    printf("pins     BCLK GPIO%d, WS GPIO%d, DOUT GPIO%d, SDA GPIO%d, SCL GPIO%d\n", CONFIG_APP_DIGI_BCLK, CONFIG_APP_DIGI_WS, CONFIG_APP_DIGI_DOUT, CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
    printf("sent     %lu frames\n", (unsigned long)st.frames_sent);
    if (st.ready) {
        char trim[16];
        printf("trim     %s ppm, PLL K 0x%06lx\n", ppb_str(trim, st.trim_ppb), (unsigned long)st.pll_k);
    }
    if (st.tone_hz) {
        printf("tone     %lu Hz at %u%%\n", (unsigned long)st.tone_hz, st.tone_level);
    } else {
        printf("tone     off\n");
    }
    return 0;
}

static bool check(const char *name, const char *what, uint32_t measured, uint32_t expected)
{
    long ppm = expected ? (long)(((int64_t)measured - expected) * 1000000 / expected) : 0;
    bool ok = expected && labs(ppm) <= TOLERANCE_PPM;
    printf("%-8s %-28s %10lu Hz, expected %lu (%+ld ppm)  %s\n", name, what, (unsigned long)measured, (unsigned long)expected, ppm, ok ? "ok" : "FAIL");
    return ok;
}

static void print_lines(const char *label, const digi_lines_t *l)
{
    printf("%-8s BCLK %lu Hz, LRCLK %lu Hz (unfiltered %lu), SDA %lu edges/s, SCL %lu edges/s, DOUT driven %lu / released %lu edges/s\n", label, (unsigned long)l->bclk_hz, (unsigned long)l->lrclk_hz, (unsigned long)l->lrclk_raw_hz, (unsigned long)l->sda_edges, (unsigned long)l->scl_edges, (unsigned long)l->dout_driven_edges, (unsigned long)l->dout_edges);
    bool follows = l->dout_pulled_up && !l->dout_pulled_down;
    printf("%-8s GPIO%d released: pull-up reads %d, pull-down reads %d  %s\n", "", CONFIG_APP_DIGI_DOUT, l->dout_pulled_up, l->dout_pulled_down, follows ? "(input or open wire)" : "(DRIVEN by the HAT: pin 40 is an output, e.g. WM8804 DOUT)");
    if (l->dout_edges) {
        printf("         GPIO%d released with a pull-down should be silent (WM8804 DIN is an input): its wire is on a driven pin, e.g. swapped with LRCLK\n", CONFIG_APP_DIGI_DOUT);
    }
}

static int cmd_probe(int argc, char **argv)
{
    digi_probe_t p;
    esp_err_t err = digi_probe(&p);
    print_lines("lines", &p.before);
    if (!p.acked) {
        printf("i2c      no ACK at 0x%02x on SDA GPIO%d / SCL GPIO%d  FAIL\n", CONFIG_APP_DIGI_ADDR, CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
        printf("         idle bus: SDA %s, SCL %s\n", p.sda_level ? "high" : "LOW (held by a device)", p.scl_level ? "high" : "LOW (held by a device)");
        printf("         check 5V/GND, SDA to HAT pin 3, SCL to HAT pin 5\n");
        uint64_t bclk_want = (uint64_t)p.before.lrclk_hz * DIGI_BCLK_PER_FRAME;
        bool clocking = p.before.lrclk_hz >= 32000 * 99 / 100 && llabs((int64_t)p.before.bclk_hz - (int64_t)bclk_want) * 1000000 <= (int64_t)bclk_want * TOLERANCE_PPM;
        if (clocking) {
            printf("         BCLK is running, so the WM8804 is powered and was configured earlier\n");
        } else if (p.before.bclk_hz || p.before.lrclk_raw_hz) {
            printf("         BCLK/LRCLK show only noise, not clocks: the WM8804 is not clocking (unpowered, or never configured)\n");
        }
        if (!p.sda_level) {
            printf("         SDA stuck low: power cycle the HAT (SCL edges are the driver's bus-clear pulses)\n");
        }
        return 1;
    }
    if (err != ESP_OK && !p.id) {
        printf("i2c      ACK at 0x%02x but register read failed: %s  FAIL\n", CONFIG_APP_DIGI_ADDR, esp_err_to_name(err));
        return 1;
    }
    bool id_ok = p.id == DIGI_CHIP_ID;
    printf("i2c      0x%02x: id 0x%04x rev %u  %s\n", CONFIG_APP_DIGI_ADDR, p.id, p.rev, id_ok ? "ok" : "FAIL (not a WM8804)");
    if (!id_ok) {
        return 1;
    }
    if (err != ESP_OK) {
        printf("clocks   measurement failed: %s  FAIL\n", esp_err_to_name(err));
        return 1;
    }
    bool quiet = p.clocked_errors == 0;
    printf("noise    %d/%d I2C reads failed while the WM8804 drove BCLK/LRCLK  %s\n", p.clocked_errors, p.clocked_reads, quiet ? "ok" : "WARN");
    if (!quiet) {
        printf("         add 2.2k pull-ups from SDA and SCL to 3V3 (a Pi has 1.8k; the ESP32 internal ones are about 45k), check WS/DOUT are not swapped (see the lines row), keep I2S wires apart from SDA/SCL\n");
        printf("         the driver stops the clocks before configuring, so setup works; I2C access during playback is unreliable\n");
    }

    digi_state_t st;
    digi_get(&st);
    char what[40];
    uint32_t bclk_expected = st.rate * DIGI_BCLK_PER_FRAME;
    snprintf(what, sizeof(what), "BCLK on GPIO%d", CONFIG_APP_DIGI_BCLK);
    bool b_ok = check("bclk", what, p.after.bclk_hz, bclk_expected);
    snprintf(what, sizeof(what), "LRCLK on GPIO%d, filtered", CONFIG_APP_DIGI_WS);
    bool lr_ok = check("lrclk", what, p.after.lrclk_hz, st.rate);
    uint32_t xtal = (uint64_t)CONFIG_APP_DIGI_XTAL_HZ * p.after.bclk_hz / bclk_expected;
    bool xtal_ok = check("crystal", "config x BCLK / expected", xtal, CONFIG_APP_DIGI_XTAL_HZ);
    if (!xtal_ok && p.after.bclk_hz) {
        printf("         set menuconfig -> Board Digi+ -> crystal to the measured value\n");
    }
    uint32_t glitches = p.after.lrclk_raw_hz > p.after.lrclk_hz ? p.after.lrclk_raw_hz - p.after.lrclk_hz : 0;
    bool glitchy = glitches > st.rate / 1000;
    printf("glitches %lu/s extra edges on LRCLK (unfiltered %lu Hz)  %s\n", (unsigned long)glitches, (unsigned long)p.after.lrclk_raw_hz, glitchy ? "WARN" : "ok");
    if (glitchy) {
        printf("         BCLK couples into the LRCLK wire; the ESP32 I2S may mis-frame: route the two apart or add a GND wire between\n");
    }
    bool ok = xtal_ok;
    if (!lr_ok && !p.after.lrclk_hz) {
        printf("         no LRCLK: check HAT pin 35 -> GPIO%d\n", CONFIG_APP_DIGI_WS);
    }
    if (!b_ok && !p.after.bclk_hz) {
        printf("         no BCLK: check HAT pin 12 -> GPIO%d\n", CONFIG_APP_DIGI_BCLK);
    }
    ok = ok && lr_ok && b_ok;
    printf("dout     GPIO%d -> HAT pin 40 has no readback: 'digi tone 440' and listen, or 'digi spdif' with the monitor wire on the S/PDIF output\n", CONFIG_APP_DIGI_DOUT);
    printf("result   %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_reg(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_reg_args)) {
        return 1;
    }
    int reg = s_reg_args.reg->ival[0];
    if (reg < 0 || reg > 0x1E) {
        fprintf(stderr, "digi: register must be 0x00-0x1e\n");
        return 1;
    }
    esp_err_t err;
    if (s_reg_args.val->count) {
        err = digi_reg_write(reg, s_reg_args.val->ival[0] & 0xFF);
    } else {
        uint8_t val;
        err = digi_reg_read(reg, &val);
        if (err == ESP_OK) {
            printf("R%d (0x%02x) = 0x%02x\n", reg, reg, val);
        }
    }
    if (err != ESP_OK) {
        fprintf(stderr, "digi: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rate(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_rate_args) || !ready()) {
        return 1;
    }
    esp_err_t err = digi_set_rate(s_rate_args.hz->ival[0]);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        fprintf(stderr, "digi: rate must be 32000, 44100, 48000, 88200, 96000, 176400 or 192000\n");
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_tone(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_tone_args) || !audio_ready()) {
        return 1;
    }
    int hz = s_tone_args.hz->ival[0];
    int level = s_tone_args.level->count ? s_tone_args.level->ival[0] : 10;
    if (hz <= 0 || level < 0 || level > 100) {
        fprintf(stderr, "digi: hz must be > 0, level 0-100\n");
        return 1;
    }
    return digi_tone(hz, level) == ESP_OK ? 0 : 1;
}

static const char *cs_rate_name(uint8_t code)
{
    switch (code) {
    case 0x0: return "44.1 kHz";
    case 0x2: return "48 kHz";
    case 0x3: return "32 kHz";
    case 0x8: return "88.2 kHz";
    case 0xA: return "96 kHz";
    case 0xC: return "176.4 kHz";
    case 0xE: return "192 kHz";
    case 0x1: return "not indicated";
    default: return "reserved";
    }
}

static bool print_capture(const digi_state_t *st)
{
    digi_spdif_report_t r;
    if (digi_spdif_capture(&r, NULL, NULL) != ESP_OK) {
        printf("decode   capture failed  FAIL\n");
        return false;
    }
    bool decoded = r.subframes > 100 && r.sync_errors <= 2;
    printf("decode   %d RMT blocks, %lu symbols, %lu subframes, %lu frames, %lu block starts, %lu sync errors  %s\n", r.mem_blocks, (unsigned long)r.symbols, (unsigned long)r.subframes, (unsigned long)r.frames, (unsigned long)r.blocks, (unsigned long)r.sync_errors, decoded ? "ok" : "FAIL");
    if (!decoded) {
        return false;
    }
    bool rate_ok = labs((long)r.rate_hz - (long)st->rate) * 1000 <= (long)st->rate * 5;
    printf("rate     %lu Hz from bit timing, expected %lu  %s\n", (unsigned long)r.rate_hz, (unsigned long)st->rate, rate_ok ? "ok" : "FAIL");
    bool clean = !r.parity_errors && !r.validity_set && !r.lr_mismatch;
    printf("frames   parity errors %lu, validity set %lu, left != right %lu  %s\n", (unsigned long)r.parity_errors, (unsigned long)r.validity_set, (unsigned long)r.lr_mismatch, clean ? "ok" : "FAIL");
    bool cs_ok = false;
    if (r.cs_valid) {
        bool consumer = !(r.cs[0] & 0x01);
        bool pcm = !(r.cs[0] & 0x02);
        uint8_t freq = r.cs[3] & 0x0F;
        cs_ok = consumer && pcm;
        printf("status   %02x %02x %02x %02x %02x: %s, %s, copy %s, rate %s  %s\n", r.cs[0], r.cs[1], r.cs[2], r.cs[3], r.cs[4], consumer ? "consumer" : "professional", pcm ? "PCM audio" : "NON-AUDIO", r.cs[0] & 0x04 ? "permitted" : "inhibited", cs_rate_name(freq), cs_ok ? "ok" : "FAIL");
    } else {
        printf("status   no full channel status block captured  FAIL\n");
    }
    uint32_t tone_x10 = st->tone_hz * 10;
    bool tone_ok = r.tone_hz_x10[0] && labs((long)r.tone_hz_x10[0] - (long)tone_x10) * 100 <= (long)tone_x10 * 2;
    long peak_pct_x10 = (long)((int64_t)r.peak[0] * 1000 / (1 << 23));
    long want_pct_x10 = st->tone_level * 10;
    bool level_ok = labs(peak_pct_x10 - want_pct_x10) <= 5;
    printf("audio    %lu.%lu Hz sine, peak %ld.%ld %% of full scale, expected %lu Hz at %u %%  %s\n", (unsigned long)r.tone_hz_x10[0] / 10, (unsigned long)r.tone_hz_x10[0] % 10, peak_pct_x10 / 10, peak_pct_x10 % 10, (unsigned long)st->tone_hz, st->tone_level, tone_ok && level_ok ? "ok" : "FAIL");
    return rate_ok && clean && cs_ok && tone_ok && level_ok;
}

static int cmd_spdif(int argc, char **argv)
{
    if (!ready()) {
        return 1;
    }
    digi_state_t saved;
    digi_get(&saved);
    uint32_t silent, tone;
    if (digi_tone(0, 0) != ESP_OK || digi_spdif_edges(0, &silent) != ESP_OK) {
        return 1;
    }
    esp_err_t err = digi_tone(SPDIF_TONE_HZ, SPDIF_TONE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(SPDIF_TONE_SETTLE_MS));
    err = err == ESP_OK ? digi_spdif_edges(0, &tone) : err;
    if (err != ESP_OK) {
        digi_tone(saved.tone_hz, saved.tone_level);
        return 1;
    }
    digi_state_t st;
    digi_get(&st);
    uint32_t idle_expected = st.rate * SPDIF_IDLE_RISING_PER_FRAME;
    bool running = silent > idle_expected * 8 / 10 && silent < idle_expected * 12 / 10;
    bool carries_audio = tone > silent + st.rate * SPDIF_AUDIO_MIN_EXTRA_PER_FRAME;
    printf("silence  %lu rising edges/s on GPIO%d, expected about %lu  %s\n", (unsigned long)silent, CONFIG_APP_DIGI_SPDIF_MON, (unsigned long)idle_expected, running ? "ok" : "FAIL");
    printf("tone     %lu rising edges/s with a %d Hz tone at %d%%, expected clearly more  %s\n", (unsigned long)tone, SPDIF_TONE_HZ, SPDIF_TONE_LEVEL, carries_audio ? "ok" : "FAIL");
    if (!silent) {
        printf("         no S/PDIF: transmitter off, or GPIO%d not wired to WM8804 pin 17 / optical transmitter input\n", CONFIG_APP_DIGI_SPDIF_MON);
    }
    bool decoded = silent && print_capture(&st);
    digi_tone(saved.tone_hz, saved.tone_level);
    bool ok = running && carries_audio && decoded;
    printf("result   %s\n", ok ? "ok: the S/PDIF stream carries the tone" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_decode(int argc, char **argv)
{
    static const uint32_t glitch_ns[] = { 0, 25, 50, 100, 200, 1000 };
    printf("edges    rising edges/s on GPIO%d, pulses shorter than the filter ignored:", CONFIG_APP_DIGI_SPDIF_MON);
    for (size_t i = 0; i < sizeof(glitch_ns) / sizeof(glitch_ns[0]); i++) {
        uint32_t edges;
        if (digi_spdif_edges(glitch_ns[i], &edges) != ESP_OK) {
            return 1;
        }
        printf(" %lu ns: %lu,", (unsigned long)glitch_ns[i], (unsigned long)edges);
    }
    static const struct {
        gpio_pull_mode_t pull;
        const char *name;
    } pulls[] = { { GPIO_FLOATING, "no pull" }, { GPIO_PULLUP_ONLY, "pull-up" }, { GPIO_PULLDOWN_ONLY, "pull-down" } };
    printf("\nlevel    high in %% of 100000 reads:");
    for (size_t i = 0; i < sizeof(pulls) / sizeof(pulls[0]); i++) {
        uint32_t high = digi_spdif_high_permille(pulls[i].pull);
        printf(" %s %lu.%lu,", pulls[i].name, (unsigned long)high / 10, (unsigned long)high % 10);
    }
    printf("\n");
    if (!shell_args_ok(argc, argv, &s_decode_args)) {
        return 1;
    }
    bool dump = s_decode_args.dump->count;
    int32_t (*frames)[2] = dump ? calloc(DIGI_SPDIF_MAX_FRAMES, sizeof(*frames)) : NULL;
    digi_spdif_report_t r;
    esp_err_t err = dump && !frames ? ESP_ERR_NO_MEM : dump ? digi_spdif_capture_frames(&r, frames) : digi_spdif_capture(&r, NULL, NULL);
    if (err != ESP_OK) {
        free(frames);
        printf("decode   capture failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (dump) {
        uint32_t n = r.frames < DIGI_SPDIF_MAX_FRAMES ? r.frames : DIGI_SPDIF_MAX_FRAMES;
        printf("dump     %lu frames, %lu Hz, %lu sync errors, %lu parity errors\n", (unsigned long)n, (unsigned long)r.rate_hz, (unsigned long)r.sync_errors, (unsigned long)r.parity_errors);
        for (uint32_t i = 0; i < n; i++) {
            if (frames[i][0] == DIGI_SPDIF_BREAK) {
                printf("break\n");
            } else {
                printf("%06lx %06lx\n", (unsigned long)(frames[i][0] & 0xFFFFFF), (unsigned long)(frames[i][1] & 0xFFFFFF));
            }
        }
        printf("end\n");
        free(frames);
        return 0;
    }
    printf("decode   %d RMT blocks, %lu symbols, %lu subframes, %lu frames, %lu block starts, %lu sync errors\n", r.mem_blocks, (unsigned long)r.symbols, (unsigned long)r.subframes, (unsigned long)r.frames, (unsigned long)r.blocks, (unsigned long)r.sync_errors);
    printf("raw      %lu nonzero symbols, first:", (unsigned long)r.nonzero_symbols);
    for (int i = 0; i < DIGI_SPDIF_RAW_SYMBOLS; i++) {
        printf(" %u:%u %u:%u", (unsigned)(r.raw[i] >> 15 & 1), (unsigned)(r.raw[i] & 0x7FFF), (unsigned)(r.raw[i] >> 31), (unsigned)(r.raw[i] >> 16 & 0x7FFF));
    }
    printf("\n");
    printf("cells    UI %lu.%02lu ticks at 80 MHz; runs: <1 cell %lu, 1 cell %lu, 2 cells %lu, 3 cells %lu, longer %lu\n", (unsigned long)r.ui_x16 / 16, (unsigned long)(r.ui_x16 % 16) * 100 / 16, (unsigned long)r.runs[0], (unsigned long)r.runs[1], (unsigned long)r.runs[2], (unsigned long)r.runs[3], (unsigned long)r.runs[4]);
    if (!r.subframes) {
        return 1;
    }
    printf("rate     %lu Hz from bit timing\n", (unsigned long)r.rate_hz);
    printf("frames   parity errors %lu, validity set %lu, left != right %lu\n", (unsigned long)r.parity_errors, (unsigned long)r.validity_set, (unsigned long)r.lr_mismatch);
    if (r.cs_valid) {
        printf("status   %02x %02x %02x %02x %02x: %s, %s, copy %s, rate %s\n", r.cs[0], r.cs[1], r.cs[2], r.cs[3], r.cs[4], r.cs[0] & 0x01 ? "professional" : "consumer", r.cs[0] & 0x02 ? "NON-AUDIO" : "PCM audio", r.cs[0] & 0x04 ? "permitted" : "inhibited", cs_rate_name(r.cs[3] & 0x0F));
    } else {
        printf("status   no full channel status block captured\n");
    }
    int bits = r.sample_or ? 24 - __builtin_ctz(r.sample_or) : 0;
    for (int ch = 0; ch < 2; ch++) {
        long peak_pct_x10 = (long)((int64_t)r.peak[ch] * 1000 / (1 << 23));
        printf("%s    %lu.%lu Hz, peak %ld.%ld %% of full scale\n", ch ? "right" : "left ", (unsigned long)r.tone_hz_x10[ch] / 10, (unsigned long)r.tone_hz_x10[ch] % 10, peak_pct_x10 / 10, peak_pct_x10 % 10);
    }
    printf("bits     %d significant (OR of samples 0x%06lx)\n", bits, (unsigned long)r.sample_or);
    printf("samples  first frames, left/right, 24-bit:");
    for (int i = 0; i < DIGI_SPDIF_DUMP_FRAMES; i++) {
        printf("%s%7ld/%-7ld", i % 6 ? " " : "\n        ", (long)(r.first[i][0] >> 8), (long)(r.first[i][1] >> 8));
    }
    printf("\n");
    return 0;
}

static bool ppm_arg(double ppm, int32_t *ppb)
{
    if (ppm < -TRIM_MAX_PPM || ppm > TRIM_MAX_PPM) {
        fprintf(stderr, "digi: trim must be within +-%d ppm\n", TRIM_MAX_PPM);
        return false;
    }
    *ppb = (int32_t)lround(ppm * 1000);
    return true;
}

static bool measure_trim(int32_t ppb, int32_t *measured, uint32_t *k)
{
    if (digi_trim(ppb) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(TRIM_SETTLE_MS));
    digi_state_t st;
    digi_get(&st);
    *k = st.pll_k;
    return digi_bclk_offset(TRIM_MEASURE_MS, measured) == ESP_OK;
}

static bool negative_number(int argc, char **argv, double *val)
{
    char *end;
    if (argc != 2 || argv[1][0] != '-') {
        return false;
    }
    *val = strtod(argv[1], &end);
    return end != argv[1] && !*end;
}

static int cmd_trim(int argc, char **argv)
{
    double ppm = 0;
    bool negative = negative_number(argc, argv, &ppm);
    if ((!negative && !shell_args_ok(argc, argv, &s_trim_args)) || !ready()) {
        return 1;
    }
    if (!negative && s_trim_args.ppm->count) {
        ppm = s_trim_args.ppm->dval[0];
    }
    digi_state_t st;
    digi_get(&st);
    int32_t ppb = st.trim_ppb;
    if ((negative || s_trim_args.ppm->count) && !ppm_arg(ppm, &ppb)) {
        return 1;
    }
    int32_t measured;
    uint32_t k;
    if (!measure_trim(ppb, &measured, &k)) {
        return 1;
    }
    char a[16], b[16];
    printf("trim     %s ppm, PLL K 0x%06lx\n", ppb_str(a, ppb), (unsigned long)k);
    printf("bclk     %s ppm vs %lu Hz over %d ms (absolute value includes the ESP32 crystal error)\n", ppb_str(b, measured), (unsigned long)st.rate * DIGI_BCLK_PER_FRAME, TRIM_MEASURE_MS);
    return 0;
}

typedef struct {
    const char *name;
    uint32_t captures;
    uint32_t failed;
    uint32_t write_errors;
    uint32_t frames;
    uint32_t sync_errors;
    uint32_t parity_errors;
    uint32_t lr_mismatch;
} glitch_row_t;

static void glitch_add(glitch_row_t *row, esp_err_t err, esp_err_t write_err, const digi_spdif_report_t *r)
{
    row->captures++;
    row->write_errors += write_err != ESP_OK;
    if (err != ESP_OK) {
        row->failed++;
        return;
    }
    row->frames += r->frames;
    row->sync_errors += r->sync_errors;
    row->parity_errors += r->parity_errors;
    row->lr_mismatch += r->lr_mismatch;
}

static uint32_t glitch_total(const glitch_row_t *row)
{
    return row->failed + row->sync_errors + row->parity_errors + row->lr_mismatch;
}

static bool trim_accuracy(int32_t ppb)
{
    const int32_t steps[] = { 0, ppb, -ppb, 0 };
    int32_t base = 0;
    uint32_t k[4];
    bool ok = true;
    printf("accuracy BCLK over %d ms per step; delta is relative to the first trim 0 reading\n", TRIM_MEASURE_MS);
    for (int i = 0; i < 4; i++) {
        int32_t measured;
        if (!measure_trim(steps[i], &measured, &k[i])) {
            printf("         trim step failed  FAIL\n");
            return false;
        }
        if (i == 0) {
            base = measured;
        }
        int32_t delta = measured - base;
        int32_t error = delta - steps[i];
        int32_t tolerance = TRIM_TOLERANCE_PPB + labs((long)steps[i]) / TRIM_TOLERANCE_SHARE;
        bool step_ok = labs((long)error) <= tolerance;
        ok = ok && step_ok;
        char a[16], b[16], c[16], d[16];
        printf("         trim %s ppm  K 0x%06lx  measured %s ppm  delta %s  error %s  %s\n", ppb_str(a, steps[i]), (unsigned long)k[i], ppb_str(b, measured), ppb_str(c, delta), ppb_str(d, error), step_ok ? "ok" : "FAIL");
    }
    if ((k[1] ^ k[2]) >> 16) {
        printf("         +-%ld ppm changes R5 (K bits 21-16): byte-wise writes pass through a K far from both ends for one I2C write\n", (long)ppb / 1000);
    }
    return ok;
}

static void print_glitch_row(const glitch_row_t *row)
{
    printf("         %-22s %8lu %6lu %8lu %6lu %7lu %6lu %7lu\n", row->name, (unsigned long)row->captures, (unsigned long)row->failed, (unsigned long)row->frames, (unsigned long)row->sync_errors, (unsigned long)row->parity_errors, (unsigned long)row->lr_mismatch, (unsigned long)row->write_errors);
}

static bool trim_glitches(int32_t ppb, int captures)
{
    glitch_row_t rows[] = { { .name = "no write" }, { .name = "rewrite same K" }, { .name = "step" } };
    digi_spdif_report_t r;
    if (digi_trim(ppb) != ESP_OK) {
        return false;
    }
    digi_spdif_capture(&r, NULL, NULL);
    for (int i = 0; i < captures; i++) {
        esp_err_t err = digi_spdif_capture(&r, NULL, NULL);
        glitch_add(&rows[0], err, ESP_OK, &r);
        digi_state_t st;
        digi_get(&st);
        esp_err_t write_err;
        err = digi_trim_capture(st.trim_ppb, true, &r, &write_err);
        glitch_add(&rows[1], err, write_err, &r);
        err = digi_trim_capture(st.trim_ppb == ppb ? -ppb : ppb, false, &r, &write_err);
        glitch_add(&rows[2], err, write_err, &r);
    }
    char a[16];
    printf("glitches S/PDIF captures on GPIO%d starting right after the write (I2C is quiet during a capture); step toggles %s and back\n", CONFIG_APP_DIGI_SPDIF_MON, ppb_str(a, ppb));
    printf("         %-22s %8s %6s %8s %6s %7s %6s %7s\n", "condition", "captures", "failed", "frames", "sync", "parity", "L!=R", "wr err");
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        print_glitch_row(&rows[i]);
    }
    if (!rows[0].frames) {
        printf("         nothing decoded: wire WM8804 TX0 (pin 17) to GPIO%d  FAIL\n", CONFIG_APP_DIGI_SPDIF_MON);
        return false;
    }
    uint32_t controls = glitch_total(&rows[0]) + glitch_total(&rows[1]);
    uint32_t step = glitch_total(&rows[2]);
    bool clean = step * 2 <= controls + 2;
    printf("         steps: %lu errors in %d captures, controls: %lu in %d  %s\n", (unsigned long)step, captures, (unsigned long)controls, captures * 2, clean ? "ok" : "FAIL");
    return clean;
}

static void trim_wobble(int32_t ppb, int seconds)
{
    char a[16];
    printf("wobble   %s ppm every %d ms for %d s, listen for clicks or dropouts (Ctrl-C stops)\n", ppb_str(a, ppb), TRIM_WOBBLE_MS, seconds);
    int32_t at = ppb;
    uint32_t writes = 0, errors = 0;
    for (int ms = 0; ms < seconds * 1000; ms += TRIM_WOBBLE_MS) {
        errors += digi_trim(at) != ESP_OK;
        writes++;
        at = -at;
        if (shell_getc(TRIM_WOBBLE_MS) == SHELL_CTRL_C) {
            break;
        }
    }
    printf("         %lu writes, %lu failed\n", (unsigned long)writes, (unsigned long)errors);
}

static int cmd_trimtest(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_trimtest_args) || !ready()) {
        return 1;
    }
    int32_t ppb;
    if (!ppm_arg(s_trimtest_args.ppm->dval[0], &ppb)) {
        return 1;
    }
    int captures = s_trimtest_args.captures->count ? s_trimtest_args.captures->ival[0] : TRIM_CAPTURES;
    int wobble = s_trimtest_args.wobble->count ? s_trimtest_args.wobble->ival[0] : 0;
    if (!ppb || captures < 1 || wobble < 0) {
        fprintf(stderr, "digi: ppm must be nonzero, captures >= 1, wobble >= 0\n");
        return 1;
    }
    digi_state_t saved;
    digi_get(&saved);
    if (digi_tone(SPDIF_TONE_HZ, SPDIF_TONE_LEVEL) != ESP_OK) {
        return 1;
    }
    printf("rate     %lu Hz, tone %d Hz at %d%%\n", (unsigned long)saved.rate, SPDIF_TONE_HZ, SPDIF_TONE_LEVEL);
    bool ok = trim_accuracy(ppb);
    ok = trim_glitches(ppb, captures) && ok;
    if (wobble) {
        trim_wobble(ppb, wobble);
    }
    digi_trim(saved.trim_ppb);
    digi_tone(saved.tone_hz, saved.tone_level);
    printf("result   %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_stop(int argc, char **argv)
{
    return audio_ready() && digi_tone(0, 0) == ESP_OK ? 0 : 1;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Chip, rate, pins and tone", .func = cmd_status },
    { .name = "probe", .help = "Check I2C, chip id, crystal and the I2S clocks from the HAT", .func = cmd_probe },
    { .name = "reg", .help = "Read a register, or write it when a value is given (only R0-R2 and R11-R17 read back)", .func = cmd_reg, .argtable = &s_reg_args },
    { .name = "rate", .help = "Sample rate: 32000, 44100, 48000, 88200, 96000, 176400, 192000", .func = cmd_rate, .argtable = &s_rate_args },
    { .name = "tone", .help = "Sine test tone on both channels, level in % of full scale (default 10)", .func = cmd_tone, .argtable = &s_tone_args },
    { .name = "stop", .help = "Stop the test tone", .func = cmd_stop },
    { .name = "trim", .help = "Trim the WM8804 PLL by ppm (fractional K), then measure BCLK", .func = cmd_trim, .argtable = &s_trim_args },
    { .name = "trimtest", .help = "Check PLL trim accuracy and whether trim writes glitch the S/PDIF stream (needs the monitor wire)", .func = cmd_trimtest, .argtable = &s_trimtest_args },
    { .name = "spdif", .help = "Count S/PDIF edges on the monitor GPIO with silence and with a tone", .func = cmd_spdif },
    { .name = "decode", .help = "Decode whatever S/PDIF stream is on the monitor GPIO, no WM8804 needed", .func = cmd_decode, .argtable = &s_decode_args },
};

static int cmd_digi(int argc, char **argv)
{
    return shell_subcmd_run("digi", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_digi_register(void)
{
    s_rate_args.hz = arg_int1(NULL, NULL, "<hz>", "sample rate");
    s_rate_args.end = arg_end(2);
    s_reg_args.reg = arg_int1(NULL, NULL, "<reg>", "register, e.g. 0x1e");
    s_reg_args.val = arg_int0(NULL, NULL, "<value>", "value to write");
    s_reg_args.end = arg_end(2);
    s_tone_args.hz = arg_int1(NULL, NULL, "<hz>", "tone frequency");
    s_tone_args.level = arg_int0("l", "level", "<0-100>", "level in % of full scale, default 10");
    s_tone_args.end = arg_end(2);
    s_trim_args.ppm = arg_dbl0(NULL, NULL, "<ppm>", "PLL trim in ppm, +-1000");
    s_trim_args.end = arg_end(2);
    s_trimtest_args.ppm = arg_dbl1(NULL, NULL, "<ppm>", "trim step in ppm, e.g. 100");
    s_trimtest_args.captures = arg_int0("n", "captures", "<n>", "captures per condition, default 10");
    s_trimtest_args.wobble = arg_int0("w", "wobble", "<s>", "then toggle +-ppm for this many seconds to listen");
    s_trimtest_args.end = arg_end(4);
    s_decode_args.dump = arg_lit0("d", "dump", "print the captured frames as 24-bit hex, left right");
    s_decode_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "digi",
        .help = "Digi+ S/PDIF HAT (WM8804): status | probe | reg | rate | tone | stop | trim | trimtest | spdif | decode",
        .hint = "<command> [args]",
        .func = cmd_digi,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register digi");
    return ESP_OK;
}
