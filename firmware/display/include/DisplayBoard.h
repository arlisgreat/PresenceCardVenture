#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/touch/Touch_GT911.hpp>

// ESP32-8048S043C direct-RGB wiring with GT911 capacitive touch.
class PresenceDisplay : public lgfx::LGFX_Device {
 public:
  PresenceDisplay() {
    {
      auto cfg = panel_.config();
      cfg.memory_width = 800;
      cfg.memory_height = 480;
      cfg.panel_width = 800;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      panel_.config(cfg);
    }

    {
      auto cfg = panel_.config_detail();
      cfg.use_psram = 1;
      panel_.config_detail(cfg);
    }

    {
      auto cfg = bus_.config();
      cfg.panel = &panel_;

      cfg.pin_d0 = GPIO_NUM_8;    // B0
      cfg.pin_d1 = GPIO_NUM_3;    // B1
      cfg.pin_d2 = GPIO_NUM_46;   // B2
      cfg.pin_d3 = GPIO_NUM_9;    // B3
      cfg.pin_d4 = GPIO_NUM_1;    // B4
      cfg.pin_d5 = GPIO_NUM_5;    // G0
      cfg.pin_d6 = GPIO_NUM_6;    // G1
      cfg.pin_d7 = GPIO_NUM_7;    // G2
      cfg.pin_d8 = GPIO_NUM_15;   // G3
      cfg.pin_d9 = GPIO_NUM_16;   // G4
      cfg.pin_d10 = GPIO_NUM_4;   // G5
      cfg.pin_d11 = GPIO_NUM_45;  // R0
      cfg.pin_d12 = GPIO_NUM_48;  // R1
      cfg.pin_d13 = GPIO_NUM_47;  // R2
      cfg.pin_d14 = GPIO_NUM_21;  // R3
      cfg.pin_d15 = GPIO_NUM_14;  // R4

      cfg.pin_henable = GPIO_NUM_40;
      cfg.pin_vsync = GPIO_NUM_41;
      cfg.pin_hsync = GPIO_NUM_39;
      cfg.pin_pclk = GPIO_NUM_42;
      // Conservative first-bring-up clock: PSRAM framebuffer and Wi-Fi share
      // memory bandwidth on ESP32-S3.
      cfg.freq_write = 12500000;

      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch = 8;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch = 8;
      cfg.pclk_idle_high = 1;
      bus_.config(cfg);
    }

    panel_.setBus(&bus_);

    {
      auto cfg = backlight_.config();
      cfg.pin_bl = GPIO_NUM_2;
      backlight_.config(cfg);
    }
    panel_.light(&backlight_);

    // GT911 capacitive touch: SDA=19, SCL=20, RST=38. INT (GPIO18) is NC on
    // this board revision (needs the R17 0-ohm bridge), so pin_int stays -1
    // and the driver polls over I2C. GT911 picks 0x5D or 0x14 at boot
    // depending on the floating INT line; Touch_GT911::init() retries both.
    {
      auto cfg = touch_.config();
      cfg.x_min = 0;
      cfg.x_max = 799;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.pin_int = -1;
      cfg.pin_rst = GPIO_NUM_38;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 1;
      cfg.i2c_addr = 0x5D;
      cfg.pin_sda = GPIO_NUM_19;
      cfg.pin_scl = GPIO_NUM_20;
      cfg.freq = 400000;  // GT911 reliable max; the ITouch 1MHz default is too fast
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }

    setPanel(&panel_);
  }

 private:
  lgfx::Bus_RGB bus_;
  lgfx::Panel_RGB panel_;
  lgfx::Light_PWM backlight_;
  lgfx::Touch_GT911 touch_;
};
