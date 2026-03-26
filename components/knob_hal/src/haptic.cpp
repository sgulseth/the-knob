#include "haptic.h"
#include "display.h"
#include "hal_pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char *TAG = "haptic";

static constexpr uint8_t REG_STATUS = 0x00;
static constexpr uint8_t REG_MODE = 0x01;
static constexpr uint8_t REG_LIBRARY = 0x03;
static constexpr uint8_t REG_WAVESEQ1 = 0x04;
static constexpr uint8_t REG_WAVESEQ2 = 0x05;
static constexpr uint8_t REG_GO = 0x0C;
static constexpr uint8_t REG_OVERDRIVE = 0x0D;
static constexpr uint8_t REG_SUSTAIN_POS = 0x0E;
static constexpr uint8_t REG_SUSTAIN_NEG = 0x0F;
static constexpr uint8_t REG_BRAKE = 0x10;
static constexpr uint8_t REG_RATED_V = 0x16;
static constexpr uint8_t REG_OD_CLAMP = 0x17;
static constexpr uint8_t REG_FEEDBACK = 0x1A;
static constexpr uint8_t REG_CTRL3 = 0x1D;

static constexpr uint8_t MODE_INTERNAL_TRIGGER = 0x00;
static constexpr uint8_t MODE_RESET = 0x80;
static constexpr uint8_t LIBRARY_A = 0x01;
static constexpr uint8_t EFFECT_STRONG_CLICK = 0x01;

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static esp_err_t write_reg(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50);
}

void haptic_init() {
  i2c_master_bus_handle_t bus = display_get_i2c_bus();
  if (!bus) {
    ESP_LOGE(TAG, "I2C bus not available");
    return;
  }

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = DRV2605_ADDR;
  dev_cfg.scl_speed_hz = TOUCH_I2C_FREQ;

  esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add DRV2605 device: %s", esp_err_to_name(err));
    return;
  }

  uint8_t status = 0;
  err = read_reg(REG_STATUS, &status);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "DRV2605 not responding: %s", esp_err_to_name(err));
    return;
  }

  write_reg(REG_MODE, MODE_RESET);
  vTaskDelay(pdMS_TO_TICKS(10));

  write_reg(REG_MODE, MODE_INTERNAL_TRIGGER);

  uint8_t fb = 0;
  read_reg(REG_FEEDBACK, &fb);
  fb &= 0x7F;
  write_reg(REG_FEEDBACK, fb);

  write_reg(REG_LIBRARY, LIBRARY_A);

  write_reg(REG_RATED_V, 0x53);
  write_reg(REG_OD_CLAMP, 0x89);

  write_reg(REG_OVERDRIVE, 0x00);
  write_reg(REG_SUSTAIN_POS, 0x00);
  write_reg(REG_SUSTAIN_NEG, 0x00);
  write_reg(REG_BRAKE, 0x00);

  s_ready = true;
  ESP_LOGI(TAG, "DRV2605 ready (status=0x%02x)", status);
}

void haptic_play(uint8_t effect) {
  if (!s_ready)
    return;

  write_reg(REG_WAVESEQ1, effect);
  write_reg(REG_WAVESEQ2, 0x00);
  write_reg(REG_GO, 0x01);
}

void haptic_buzz() { haptic_play(EFFECT_STRONG_CLICK); }
