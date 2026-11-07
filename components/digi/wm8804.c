#include <stdlib.h>
#include "esp_check.h"
#include "wm8804.h"

#define WM8804_I2C_HZ (100 * 1000)
#define WM8804_TIMEOUT_MS 50
#define WM8804_ATTEMPTS 20

#define REG_RST_DEVID1 0x00
#define REG_DEVID2 0x01
#define REG_DEVREV 0x02
#define REG_PLL1 0x03
#define REG_PLL2 0x04
#define REG_PLL3 0x05
#define REG_PLL4 0x06
#define REG_PLL5 0x07
#define REG_PLL6 0x08
#define REG_SPDTX1 0x12
#define REG_SPDTX2 0x13
#define REG_SPDTX4 0x15
#define REG_AIFRX 0x1C
#define REG_PWRDN 0x1E

#define PLL4_PRESCALE_DIV2 (1 << 4)
#define PLL5_CLKOUTDIV_256FS (1 << 4)
#define PLL5_MCLKDIV_128FS (1 << 3)
#define PLL5_FRACEN (1 << 2)
#define PLL6_DEFAULT 0x18
#define SPDTX1_COPY_PERMITTED (1 << 2)
#define SPDTX2_CATEGORY_PCM_CODER_ORIGINAL 0x82
#define SPDTX4_TXSRC_AIF (1 << 6)
#define SPDTX4_CLKACU_LEVEL2 (0 << 4)
#define AIFRX_SYNC_OFF (1 << 7)
#define AIFRX_MASTER (1 << 6)
#define AIFRX_WL_24 (2 << 2)
#define AIFRX_FMT_I2S 2
#define PWRDN_PLL (1 << 0)
#define PWRDN_SPDIFRX (1 << 1)
#define PWRDN_SPDIFTX (1 << 2)

#define PLL_F2_MIN 90000000ULL
#define PLL_F2_MAX 100000000ULL
#define PLL_N_MIN 5
#define PLL_N_MAX 13
#define PLL_N_BEST 8

static const char *TAG = "wm8804";

static const struct {
    uint32_t rate;
    uint8_t status_freq;
} s_rates[] = {
    { 32000, 0x3 }, { 44100, 0x0 }, { 48000, 0x2 }, { 88200, 0x8 }, { 96000, 0xA }, { 176400, 0xC }, { 192000, 0xE },
};

typedef struct {
    bool prescale;
    bool mclkdiv;
    uint8_t n;
    uint8_t freqmode;
    uint32_t k;
} pll_t;

static i2c_master_dev_handle_t s_dev;

esp_err_t wm8804_reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), WM8804_TIMEOUT_MS);
}

esp_err_t wm8804_reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, WM8804_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < WM8804_ATTEMPTS && err != ESP_OK; i++) {
        err = wm8804_reg_write(reg, val);
    }
    return err;
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < WM8804_ATTEMPTS && err != ESP_OK; i++) {
        err = wm8804_reg_read(reg, val);
    }
    return err;
}

esp_err_t wm8804_attach(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (s_dev) {
        return ESP_OK;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = WM8804_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

esp_err_t wm8804_read_id(uint16_t *id, uint8_t *rev)
{
    uint8_t id_lo, id_hi;
    ESP_RETURN_ON_ERROR(reg_read(REG_RST_DEVID1, &id_lo), TAG, "read DEVID1");
    ESP_RETURN_ON_ERROR(reg_read(REG_DEVID2, &id_hi), TAG, "read DEVID2");
    ESP_RETURN_ON_ERROR(reg_read(REG_DEVREV, rev), TAG, "read DEVREV");
    *id = (uint16_t)(id_hi << 8 | id_lo);
    *rev &= 0x0F;
    return ESP_OK;
}

esp_err_t wm8804_reset(void)
{
    return reg_write(REG_RST_DEVID1, 0);
}

static int rate_index(uint32_t rate)
{
    for (size_t i = 0; i < sizeof(s_rates) / sizeof(s_rates[0]); i++) {
        if (s_rates[i].rate == rate) {
            return i;
        }
    }
    return -1;
}

bool wm8804_rate_supported(uint32_t rate)
{
    return rate_index(rate) >= 0;
}

static esp_err_t pll_calc(uint32_t xtal_hz, uint32_t rate, pll_t *pll)
{
    static const uint8_t f2_to_mclk[] = { 2, 4, 8, 12 };
    *pll = (pll_t){ .mclkdiv = rate > 96000 };
    uint64_t mclk = (uint64_t)rate * (pll->mclkdiv ? 128 : 256);
    uint64_t f2 = 0;
    for (int mode = 0; mode < 4 && !f2; mode++) {
        uint64_t f = mclk * f2_to_mclk[mode] * (pll->mclkdiv ? 2 : 1);
        if (f >= PLL_F2_MIN && f <= PLL_F2_MAX) {
            f2 = f;
            pll->freqmode = mode;
        }
    }
    ESP_RETURN_ON_FALSE(f2, ESP_ERR_NOT_SUPPORTED, TAG, "no PLL output divider for %lu Hz", (unsigned long)rate);

    for (int prescale = 0; prescale < 2; prescale++) {
        uint64_t ratio = f2 << prescale;
        uint32_t n = ratio / xtal_hz;
        if (n < PLL_N_MIN || n > PLL_N_MAX) {
            continue;
        }
        if (!pll->n || abs((int)n - PLL_N_BEST) < abs((int)pll->n - PLL_N_BEST)) {
            pll->n = n;
            pll->prescale = prescale;
            pll->k = ((ratio % xtal_hz) << 22) / xtal_hz;
        }
    }
    ESP_RETURN_ON_FALSE(pll->n, ESP_ERR_NOT_SUPPORTED, TAG, "no PLL ratio for %lu Hz from a %lu Hz crystal", (unsigned long)rate, (unsigned long)xtal_hz);
    return ESP_OK;
}

static esp_err_t write_seq(const uint8_t (*seq)[2], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(reg_write(seq[i][0], seq[i][1]), TAG, "write R%u", seq[i][0]);
    }
    return ESP_OK;
}

esp_err_t wm8804_start(uint32_t xtal_hz, uint32_t rate)
{
    int idx = rate_index(rate);
    ESP_RETURN_ON_FALSE(idx >= 0, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported rate %lu", (unsigned long)rate);
    pll_t pll;
    ESP_RETURN_ON_ERROR(pll_calc(xtal_hz, rate, &pll), TAG, "pll");

    ESP_RETURN_ON_ERROR(wm8804_reset(), TAG, "reset");
    const uint8_t seq[][2] = {
        { REG_PLL1, pll.k & 0xFF },
        { REG_PLL2, (pll.k >> 8) & 0xFF },
        { REG_PLL3, (pll.k >> 16) & 0x3F },
        { REG_PLL4, (pll.prescale ? PLL4_PRESCALE_DIV2 : 0) | pll.n },
        { REG_PLL5, PLL5_CLKOUTDIV_256FS | (pll.mclkdiv ? PLL5_MCLKDIV_128FS : 0) | PLL5_FRACEN | pll.freqmode },
        { REG_PLL6, PLL6_DEFAULT },
        { REG_SPDTX1, SPDTX1_COPY_PERMITTED },
        { REG_SPDTX2, SPDTX2_CATEGORY_PCM_CODER_ORIGINAL },
        { REG_SPDTX4, SPDTX4_TXSRC_AIF | SPDTX4_CLKACU_LEVEL2 | s_rates[idx].status_freq },
        { REG_PWRDN, PWRDN_SPDIFRX },
        { REG_AIFRX, AIFRX_SYNC_OFF | AIFRX_MASTER | AIFRX_WL_24 | AIFRX_FMT_I2S },
    };
    ESP_RETURN_ON_ERROR(write_seq(seq, sizeof(seq) / sizeof(seq[0])), TAG, "configure");
    ESP_LOGI(TAG, "%lu Hz: PLL N=%u K=0x%06lx prescale=%d freqmode=%u mclkdiv=%d", (unsigned long)rate, pll.n, (unsigned long)pll.k, pll.prescale, pll.freqmode, pll.mclkdiv);
    return ESP_OK;
}
