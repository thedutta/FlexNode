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

#include "fw/ws2812_led.h"

#include <cmath>

#include "mbed.h"

namespace moteus {

namespace {
// WS2812B waveform (datasheet: T0H ~0.40 us, T1H ~0.80 us, period ~1.25 us,
// each +/- 150 ns; 0/1 decode threshold ~0.55 us).  Pulses are timed with
// the DWT cycle counter, which counts real core cycles and is therefore
// immune to flash wait-state stalls.  Open-loop delays were tried and
// failed both ways: a counted subs/bne loop ran fast (T0H ~230 ns, below
// the detection floor, so an all-'0' OFF frame registered nothing and the
// LED held its last colour), and unrolled NOPs ran slow (stalls pushed
// T0H past the threshold, so '0's read as '1' and OFF decoded turquoise).
constexpr uint32_t kT0HNs = 350;
constexpr uint32_t kT1HNs = 800;
constexpr uint32_t kBitNs = 1300;

// Upper bound on spin iterations per pulse phase, so a stopped cycle
// counter can only produce a wrong pulse, never a hung main loop.
constexpr uint32_t kSpinGuard = 4096;

constexpr uint32_t kMinFrameIntervalMs = 10;   // cap change updates at 100 Hz

// Pixel-0 "OK" breath: fade in, fade out, then stay dark.
constexpr uint32_t kOkFadeInMs = 900;
constexpr uint32_t kOkFadeOutMs = 1600;

// 0..256 smoothstep brightness envelope for the OK breath.
uint32_t OkEnvelope(uint32_t t_ms) {
  const auto ss = [](float u) -> float {         // smoothstep, 0..1
    if (u <= 0.0f) { return 0.0f; }
    if (u >= 1.0f) { return 1.0f; }
    return u * u * (3.0f - 2.0f * u);
  };
  if (t_ms < kOkFadeInMs) {
    return static_cast<uint32_t>(
        ss(static_cast<float>(t_ms) / kOkFadeInMs) * 256.0f);
  }
  const uint32_t t2 = t_ms - kOkFadeInMs;
  if (t2 < kOkFadeOutMs) {
    return static_cast<uint32_t>(
        (1.0f - ss(static_cast<float>(t2) / kOkFadeOutMs)) * 256.0f);
  }
  return 0;  // faded out; stay dark
}

// Fault blink cadence.
constexpr uint16_t kBlinkOnMs = 150;
constexpr uint16_t kBlinkOffMs = 150;
constexpr uint16_t kDigitGapMs = 500;
constexpr uint16_t kCycleGapMs = 1200;

uint8_t Clamp8(int32_t v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

uint8_t Scale(uint8_t v, uint8_t brightness) {
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(v) * brightness + 127u) / 255u);
}
}  // namespace

Ws2812Led::Ws2812Led(mjlib::micro::PersistentConfig* persistent_config,
                     mjlib::micro::TelemetryManager* telemetry_manager,
                     PinName pin,
                     const BldcServo* servo,
                     const aux::I2C::ImuStatus* imu)
    : servo_(servo),
      imu_(imu) {
  if (pin != NC) {
    pin_.emplace(pin, 0);
  }
  persistent_config->Register("led", &config_,
                              [this]() { this->ConfigUpdated(); });
  telemetry_manager->Register("led", &status_);

  // Enable the DWT cycle counter (moteus.cc does this too; idempotent)
  // and confirm it is advancing.  If it ever were not, transmit is
  // skipped rather than emitting garbage or spinning.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  {
    const uint32_t a = DWT->CYCCNT;
    for (volatile int i = 0; i < 64; i++) {}
    dwt_ok_ = (DWT->CYCCNT != a);
  }

  UpdateTiming();
}

void Ws2812Led::UpdateTiming() {
  const uint64_t clk = SystemCoreClock;  // 170 MHz on moteus G4 builds
  t0h_cycles_ = static_cast<uint32_t>(clk * kT0HNs / 1000000000ull);
  t1h_cycles_ = static_cast<uint32_t>(clk * kT1HNs / 1000000000ull);
  tbit_cycles_ = static_cast<uint32_t>(clk * kBitNs / 1000000000ull);
}

void Ws2812Led::ConfigUpdated() {
  // A conf set led.* wins over stale live values from the bus.
  mode_ = 1;
  live_channel_[0] = live_channel_[1] = live_channel_[2] = -1;
  live_brightness_ = -1;
  UpdateTiming();
}

void Ws2812Led::SetMode(int32_t mode) {
  mode_ = (mode <= 0) ? 0 : 1;   // 2 breathe / 3 chase: reserved, treated as solid
}

void Ws2812Led::SetMasterChannel(int channel, int32_t v) {
  if (channel < 0 || channel > 2) { return; }
  live_channel_[channel] = Clamp8(v);
}

void Ws2812Led::SetBrightness(int32_t v) {
  live_brightness_ = Clamp8(v);
}

int32_t Ws2812Led::master_channel(int channel) const {
  if (channel < 0 || channel > 2) { return 0; }
  if (live_channel_[channel] >= 0) { return live_channel_[channel]; }
  const int32_t cfg[3] = {config_.master_r, config_.master_g, config_.master_b};
  return Clamp8(cfg[channel]);
}

int32_t Ws2812Led::brightness() const {
  return live_brightness_ >= 0 ? live_brightness_ : Clamp8(config_.brightness);
}

Ws2812Led::Rgb Ws2812Led::ImuDemoPixel() const {
  if (!imu_ || !imu_->active) {
    // IMU missing: slow red blink.
    return ((ms_ / 500) % 2) ? Rgb{120, 0, 0} : Rgb{0, 0, 0};
  }
  constexpr float kAccelG = 0.000122f;   // +/-4 g
  constexpr float kGyroDps = 0.0175f;    // +/-500 dps
  constexpr float kPi = 3.14159265f;

  const float ax = imu_->ax * kAccelG;
  const float ay = imu_->ay * kAccelG;
  const float tilt = std::sqrt(ax * ax + ay * ay);        // 0 flat .. ~1 on edge
  float hue = std::atan2(ay, ax) / (2.0f * kPi);          // -0.5 .. 0.5
  if (hue < 0.0f) { hue += 1.0f; }
  const float sat = std::min(1.0f, tilt * 1.5f);

  const float gx = imu_->gx * kGyroDps;
  const float gy = imu_->gy * kGyroDps;
  const float gz = imu_->gz * kGyroDps;
  const float rate = std::sqrt(gx * gx + gy * gy + gz * gz);
  const float val = std::min(1.0f, 0.45f + rate / 400.0f);

  // HSV -> RGB
  const float h6 = hue * 6.0f;
  const int sector = static_cast<int>(h6) % 6;
  const float f = h6 - static_cast<float>(static_cast<int>(h6));
  const float p = val * (1.0f - sat);
  const float q = val * (1.0f - sat * f);
  const float t = val * (1.0f - sat * (1.0f - f));
  float r = 0.0f, g = 0.0f, b = 0.0f;
  switch (sector) {
    case 0: r = val; g = t; b = p; break;
    case 1: r = q; g = val; b = p; break;
    case 2: r = p; g = val; b = t; break;
    case 3: r = p; g = q; b = val; break;
    case 4: r = t; g = p; b = val; break;
    default: r = val; g = p; b = q; break;
  }
  return Rgb{static_cast<uint8_t>(r * 255.0f),
             static_cast<uint8_t>(g * 255.0f),
             static_cast<uint8_t>(b * 255.0f)};
}

Ws2812Led::Rgb Ws2812Led::EffectiveMaster() const {
  return Rgb{static_cast<uint8_t>(master_channel(0)),
             static_cast<uint8_t>(master_channel(1)),
             static_cast<uint8_t>(master_channel(2))};
}

void Ws2812Led::SetPixel(int index, uint8_t r, uint8_t g, uint8_t b) {
  if (index < 0 || index >= kMaxPixels) { return; }
  override_[index] = Rgb{r, g, b};
  has_override_[index] = true;
}

void Ws2812Led::ClearOverrides() {
  for (int i = 0; i < kMaxPixels; i++) { has_override_[i] = false; }
}

void Ws2812Led::RebuildFaultSchedule(int32_t code) {
  schedule_code_ = code;
  schedule_len_ = 0;
  schedule_index_ = 0;
  schedule_step_start_ms_ = ms_;
  if (code <= 0) { return; }

  const Rgb amber{255, 60, 0};
  const Rgb red{255, 0, 0};
  const Rgb off{0, 0, 0};

  const int tens = (code / 10) % 10;
  const int units = code % 10;

  auto push = [&](Rgb c, uint16_t ms) {
    if (schedule_len_ < kMaxSteps) {
      schedule_[schedule_len_++] = Step{c, ms};
    }
  };

  for (int i = 0; i < tens; i++) {
    push(amber, kBlinkOnMs);
    push(off, kBlinkOffMs);
  }
  if (tens > 0) { push(off, kDigitGapMs); }
  for (int i = 0; i < units; i++) {
    push(red, kBlinkOnMs);
    push(off, kBlinkOffMs);
  }
  if (units == 0) {
    // "x0": one long red so the units digit is still visible.
    push(red, kBlinkOnMs * 3);
    push(off, kBlinkOffMs);
  }
  push(off, kCycleGapMs);
}

Ws2812Led::Rgb Ws2812Led::FaultPixel() {
  if (schedule_len_ == 0) { return Rgb{}; }
  while (ms_ - schedule_step_start_ms_ >= schedule_[schedule_index_].ms) {
    schedule_step_start_ms_ += schedule_[schedule_index_].ms;
    schedule_index_ = (schedule_index_ + 1) % schedule_len_;
  }
  return schedule_[schedule_index_].color;
}

void Ws2812Led::PollMillisecond() {
  ms_++;
  if (!pin_) { return; }

  // --- Decide what the picture should be. ---
  const int32_t count =
      (config_.count < 0) ? 0 :
      (config_.count > kMaxPixels - 1) ? (kMaxPixels - 1) : config_.count;
  const int32_t pixels = 1 + count;
  const Rgb master = EffectiveMaster();
  const uint8_t brightness = static_cast<uint8_t>(this->brightness());
  const Rgb off{0, 0, 0};

  int32_t fault_code = 0;
  if (config_.fault_override && servo_ &&
      servo_->status().mode == BldcServoMode::kFault) {
    fault_code = static_cast<int32_t>(servo_->status().fault);
    if (fault_code == 0) { fault_code = 1; }  // faulted with no code: still show something
  }
  if (fault_code != schedule_code_) { RebuildFaultSchedule(fault_code); }

  // "OK" = running, no fault, not in the IMU demo.  Pixel 0 gives one blue
  // fade-in / fade-out on entering OK, then stays dark; re-armed each time
  // the board returns to OK (e.g. after a fault clears).
  const bool ok_state = (mode_ != 0) && (fault_code == 0) && !config_.imu_demo;
  if (ok_state && !ok_was_active_) { ok_anim_start_ms_ = ms_; }
  ok_was_active_ = ok_state;

  Rgb want[kMaxPixels];
  for (int32_t i = 0; i < pixels; i++) {
    want[i] = (mode_ == 0) ? off : (has_override_[i] ? override_[i] : master);
  }
  // Pixel 0 status, highest precedence first.
  if (fault_code != 0) {
    want[0] = FaultPixel();
  } else if (config_.imu_demo) {
    want[0] = ImuDemoPixel();
  } else if (mode_ != 0) {
    const uint32_t env = OkEnvelope(ms_ - ok_anim_start_ms_);   // 0..256
    const Rgb base = has_override_[0] ? override_[0] : master;
    want[0] = Rgb{static_cast<uint8_t>((base.r * env) >> 8),
                  static_cast<uint8_t>((base.g * env) >> 8),
                  static_cast<uint8_t>((base.b * env) >> 8)};
  }

  status_.mode = mode_;
  status_.pixels = pixels;
  status_.fault_shown = fault_code;
  status_.p0_r = want[0].r;
  status_.p0_g = want[0].g;
  status_.p0_b = want[0].b;

  // --- Send only if it changed. ---
  bool changed = !ever_sent_ ||
      pixels != shown_count_ ||
      brightness != shown_brightness_;
  for (int32_t i = 0; !changed && i < pixels; i++) {
    if (want[i] != shown_[i]) { changed = true; }
  }
  // WS2812B latches and holds its last value, so a static picture is sent
  // exactly once -- no periodic refresh (a resend landing during an
  // interrupt burst is what caused the occasional one-frame dimming).
  if (!changed) { return; }
  if ((ms_ - last_tx_ms_) < kMinFrameIntervalMs) { return; }  // rate-limit changes

  for (int32_t i = 0; i < pixels; i++) { shown_[i] = want[i]; }
  shown_count_ = pixels;
  shown_brightness_ = brightness;
  ever_sent_ = true;
  last_tx_ms_ = ms_;
  Transmit();
}

void Ws2812Led::SendBit(bool one) {
  // Interrupts off only for the high pulse (<1 us), so the 30 kHz control
  // ISR gains sub-microsecond jitter at most.  The low period is left
  // interruptible; if an ISR stretches it, the WS2812B tolerates a long
  // low (it only latches after >50 us), so a stretched low never corrupts
  // the frame.
  const uint32_t high = one ? t1h_cycles_ : t0h_cycles_;
  // Interrupts off only for the high pulse.  The control ISR zeroes
  // CYCCNT, which is why the pulse is measured as a difference and why the
  // low wait may end early: if an ISR ran during it, that already lasted
  // longer than the minimum low time.
  __disable_irq();
  const uint32_t start = DWT->CYCCNT;
  pin_->set();
  for (uint32_t g = 0; (DWT->CYCCNT - start) < high && g < kSpinGuard; g++) {}
  pin_->clear();
  __enable_irq();
  for (uint32_t g = 0; (DWT->CYCCNT - start) < tbit_cycles_ && g < kSpinGuard; g++) {}
}

void Ws2812Led::SendByte(uint8_t value) {
  for (int i = 7; i >= 0; i--) {
    SendBit((value >> i) & 1);
  }
}

void Ws2812Led::Transmit() {
  if (!dwt_ok_) { return; }
  const uint8_t brightness = static_cast<uint8_t>(shown_brightness_);
  for (int32_t i = 0; i < shown_count_; i++) {
    // WS2812B byte order is G, R, B.
    SendByte(Scale(shown_[i].g, brightness));
    SendByte(Scale(shown_[i].r, brightness));
    SendByte(Scale(shown_[i].b, brightness));
  }
  // Line stays low from here; the next frame is >= 10 ms away, well
  // past the WS2812B latch/reset time.
  pin_->clear();
  status_.frames++;
}

}  // namespace moteus
