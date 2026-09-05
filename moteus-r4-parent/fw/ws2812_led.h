// Copyright 2026 FlexNode contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <optional>

#include "PinNames.h"

#include "mjlib/base/visitor.h"
#include "mjlib/micro/persistent_config.h"
#include "mjlib/micro/telemetry_manager.h"

#include "fw/aux_common.h"
#include "fw/bldc_servo.h"
#include "fw/stm32_digital_output.h"

namespace moteus {

/// FlexNode: a chain of WS2812B addressable LEDs hung off one GPIO
/// (PF0).  Pixel 0 is the node's own status LED.  Pixels 1..count are
/// external "master control" lighting.
///
/// Normal operation: every pixel shows the master colour
/// (led.master_r/g/b) unless it has been given an individual colour
/// through SetPixel() (the hook for the future CAN register block).
///
/// Fault override: while the servo is in kFault and led.fault_override
/// is set, pixel 0 stops following the master colour and blinks the
/// fault code instead - tens digit in amber, units digit in red, long
/// pause, repeat.  e.g. fault 35 (encoder) = 3 amber, gap, 5 red.
///
/// Timing: the WS2812 stream is bit-banged from the main loop using the
/// DWT cycle counter.  Interrupts are masked only for the HIGH part of
/// each bit (<= ~0.85 us), never across a whole frame, so the 30 kHz
/// control ISR sees sub-microsecond added jitter and can still preempt
/// during the LOW part of any bit (the WS2812B tolerates long lows up
/// to its >50 us reset window; the ISR is far shorter than that).
/// Frames are only sent when the picture changes (plus a 1 Hz refresh),
/// so a static colour costs the main loop nothing.
class Ws2812Led {
 public:
  static constexpr int kMaxPixels = 32;

  struct Config {
    // Number of EXTERNAL pixels chained after the onboard status LED.
    // 0 = onboard LED only.  Clamped to kMaxPixels - 1.
    int32_t count = 0;

    // Global brightness scale, 0..255, applied on output.
    int32_t brightness = 64;

    // Master colour (0..255 each), applied to every pixel without an
    // individual override.
    int32_t master_r = 0;
    int32_t master_g = 80;
    int32_t master_b = 255;

    // If nonzero, pixel 0 blinks the fault code while the servo is faulted.
    int32_t fault_override = 1;

    // Bench aid: if nonzero, pixel 0 shows the IMU instead of the master
    // colour - hue from the tilt direction, saturation from tilt
    // magnitude, brightness rising with rotation rate.  Slow red blink if
    // the IMU is not answering.  Fault override still wins.
    int32_t imu_demo = 0;

    template <typename Archive>
    void Serialize(Archive* a) {
      a->Visit(MJ_NVP(count));
      a->Visit(MJ_NVP(brightness));
      a->Visit(MJ_NVP(master_r));
      a->Visit(MJ_NVP(master_g));
      a->Visit(MJ_NVP(master_b));
      a->Visit(MJ_NVP(fault_override));
      a->Visit(MJ_NVP(imu_demo));
    }
  };

  struct Status {
    uint32_t frames = 0;        // frames transmitted since boot
    int32_t mode = 1;           // 0 off, 1 solid
    int32_t pixels = 1;         // pixels per frame (1 + count)
    int32_t fault_shown = 0;    // fault code currently blinked on pixel 0 (0 = none)
    int32_t p0_r = 0;           // what pixel 0 is showing (before brightness)
    int32_t p0_g = 0;
    int32_t p0_b = 0;

    template <typename Archive>
    void Serialize(Archive* a) {
      a->Visit(MJ_NVP(frames));
      a->Visit(MJ_NVP(mode));
      a->Visit(MJ_NVP(pixels));
      a->Visit(MJ_NVP(fault_shown));
      a->Visit(MJ_NVP(p0_r));
      a->Visit(MJ_NVP(p0_g));
      a->Visit(MJ_NVP(p0_b));
    }
  };

  Ws2812Led(mjlib::micro::PersistentConfig* persistent_config,
            mjlib::micro::TelemetryManager* telemetry_manager,
            PinName pin,
            const BldcServo* servo,
            const aux::I2C::ImuStatus* imu);

  /// Call once per millisecond from the main loop.
  void PollMillisecond();

  /// Give one pixel its own colour, overriding the master colour until
  /// ClearOverrides().  Index 0 is the onboard LED (still subject to the
  /// fault override).  Out-of-range indices are ignored.
  void SetPixel(int index, uint8_t r, uint8_t g, uint8_t b);
  void ClearOverrides();

  /// Live (non-persisted) control, used by the FlexNode CAN registers
  /// 0x0b0-0x0b4.  These shadow the led.* config until the config is
  /// next changed through conf set, which drops the live values.
  void SetMode(int32_t mode);                       // 0 off, 1 solid
  void SetMasterChannel(int channel, int32_t v);    // 0=r 1=g 2=b, 0..255
  void SetBrightness(int32_t v);                    // 0..255
  int32_t mode() const { return mode_; }
  int32_t master_channel(int channel) const;
  int32_t brightness() const;

  const Config& config() const { return config_; }
  const Status& status() const { return status_; }

 private:
  struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
  };

  void UpdateTiming();
  void ConfigUpdated();
  Rgb EffectiveMaster() const;
  Rgb ImuDemoPixel() const;
  void RebuildFaultSchedule(int32_t code);
  Rgb FaultPixel();
  void Transmit();
  void SendByte(uint8_t value);
  void SendBit(bool one);

  Config config_;
  Status status_;
  const BldcServo* const servo_;
  const aux::I2C::ImuStatus* const imu_;
  std::optional<Stm32DigitalOutput> pin_;

  Rgb override_[kMaxPixels] = {};
  bool has_override_[kMaxPixels] = {};
  Rgb shown_[kMaxPixels] = {};       // last transmitted picture (pre-brightness)
  int32_t shown_count_ = -1;
  int32_t shown_brightness_ = -1;

  // Live overrides from the CAN registers (-1 = follow config).
  int32_t mode_ = 1;
  int32_t live_channel_[3] = {-1, -1, -1};
  int32_t live_brightness_ = -1;

  uint32_t ms_ = 0;
  uint32_t last_tx_ms_ = 0;
  bool ever_sent_ = false;

  // Fault blink schedule: a list of (colour, duration) steps.
  struct Step {
    Rgb color;
    uint16_t ms;
  };
  static constexpr int kMaxSteps = 2 * 9 + 2 * 9 + 2;
  Step schedule_[kMaxSteps] = {};
  int schedule_len_ = 0;
  int schedule_index_ = 0;
  uint32_t schedule_step_start_ms_ = 0;
  int32_t schedule_code_ = 0;

  // Cycle counts for the WS2812B waveform at the current core clock.
  uint32_t t1h_cycles_ = 0;
  uint32_t t0h_cycles_ = 0;
  uint32_t tbit_cycles_ = 0;
};

}  // namespace moteus
