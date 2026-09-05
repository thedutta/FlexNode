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

#include "mbed.h"

namespace moteus {

namespace {
// WS2812B waveform (datasheet: T0H 0.40 us, T1H 0.80 us, period 1.25 us,
// each +/- 150 ns).  We aim slightly under on T0H for margin against the
// ~550 ns decode threshold, and give every bit a full 1.30 us period.
constexpr uint32_t kT0HNs = 350;
constexpr uint32_t kT1HNs = 800;
constexpr uint32_t kBitNs = 1300;

constexpr uint32_t kMinFrameIntervalMs = 10;   // never faster than 100 Hz
constexpr uint32_t kRefreshIntervalMs = 1000;  // resend a static picture at 1 Hz

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
                     const BldcServo* servo)
    : servo_(servo) {
  if (pin != NC) {
    pin_.emplace(pin, 0);
  }
  persistent_config->Register("led", &config_,
                              [this]() { this->UpdateTiming(); });
  telemetry_manager->Register("led", &status_);

  // Make sure the DWT cycle counter is running (moteus.cc enables it
  // too; this is idempotent).
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  UpdateTiming();
}

void Ws2812Led::UpdateTiming() {
  const uint64_t clk = SystemCoreClock;  // 170 MHz on moteus G4 builds
  t0h_cycles_ = static_cast<uint32_t>(clk * kT0HNs / 1000000000ull);
  t1h_cycles_ = static_cast<uint32_t>(clk * kT1HNs / 1000000000ull);
  tbit_cycles_ = static_cast<uint32_t>(clk * kBitNs / 1000000000ull);
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
  const Rgb master{Clamp8(config_.master_r),
                   Clamp8(config_.master_g),
                   Clamp8(config_.master_b)};
  const uint8_t brightness = Clamp8(config_.brightness);

  Rgb want[kMaxPixels];
  for (int32_t i = 0; i < pixels; i++) {
    want[i] = has_override_[i] ? override_[i] : master;
  }

  int32_t fault_code = 0;
  if (config_.fault_override && servo_ &&
      servo_->status().mode == BldcServoMode::kFault) {
    fault_code = static_cast<int32_t>(servo_->status().fault);
    if (fault_code == 0) { fault_code = 1; }  // faulted with no code: still show something
  }
  if (fault_code != schedule_code_) { RebuildFaultSchedule(fault_code); }
  if (fault_code != 0) { want[0] = FaultPixel(); }

  status_.pixels = pixels;
  status_.fault_shown = fault_code;
  status_.p0_r = want[0].r;
  status_.p0_g = want[0].g;
  status_.p0_b = want[0].b;

  // --- Send only if it changed (or for the periodic refresh). ---
  bool changed = !ever_sent_ ||
      pixels != shown_count_ ||
      brightness != shown_brightness_;
  for (int32_t i = 0; !changed && i < pixels; i++) {
    if (want[i] != shown_[i]) { changed = true; }
  }
  const uint32_t since_tx = ms_ - last_tx_ms_;
  if (!(changed && since_tx >= kMinFrameIntervalMs) &&
      since_tx < kRefreshIntervalMs) {
    return;
  }

  for (int32_t i = 0; i < pixels; i++) { shown_[i] = want[i]; }
  shown_count_ = pixels;
  shown_brightness_ = brightness;
  ever_sent_ = true;
  last_tx_ms_ = ms_;
  Transmit();
}

void Ws2812Led::SendBit(bool one) {
  const uint32_t high = one ? t1h_cycles_ : t0h_cycles_;
  // Interrupts off only for the high pulse.  Nothing else can touch
  // DWT->CYCCNT in this window (the control ISR zeroes it, which is why
  // the pulse is measured as a difference, and why the low wait below
  // is allowed to end early: if an ISR ran, it lasted far longer than
  // the minimum low time anyway).
  __disable_irq();
  const uint32_t start = DWT->CYCCNT;
  pin_->set();
  while ((DWT->CYCCNT - start) < high) {}
  pin_->clear();
  __enable_irq();
  while ((DWT->CYCCNT - start) < tbit_cycles_) {}
}

void Ws2812Led::SendByte(uint8_t value) {
  for (int i = 7; i >= 0; i--) {
    SendBit((value >> i) & 1);
  }
}

void Ws2812Led::Transmit() {
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
