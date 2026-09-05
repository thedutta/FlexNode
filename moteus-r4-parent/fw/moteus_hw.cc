// Copyright 2023 mjbots Robotic Systems, LLC.  info@mjbots.com
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

#include "fw/moteus_hw.h"

#include "mjlib/base/assert.h"

namespace moteus {

FamilyAndVersion DetectMoteusFamily(MillisecondTimer*) {
  // === FlexNode board-identity hardcode ===
  // FlexNode reuses the moteus hardware-detection strap pins for other
  // functions (PB11 -> 5V current-sense ADC; the AS5047 CS moves to
  // PC6; PB10 -> ToF interrupt), so moteus's runtime autodetection
  // would misread the board -- and would briefly drive those repurposed
  // pins during boot.  It has been removed entirely (see upstream
  // mjbots/moteus fw/moteus_hw.cc for the original).
  //
  // FlexNode is electrically an r4.11-class board, so we report
  // {family = 0, hw_version = 8}.  hw_version 8 is load-bearing:
  //   * drv8323.cc keys the DRV8353 register tables off hw_rev >= 7,
  //     and FlexNode's gate driver is the DRV8353S;
  //   * FindHardwarePins() selects the r4.11 analog map (vsense = PB12,
  //     msense = PA8, vsense_adc_scale = 0.017947) for hv >= 5.
  // See ../docs/firmware.md.
  FamilyAndVersion result;
  result.family = 0;
  result.hw_version = 8;
  result.hw_pins = 4;  // r4.11's version-strap encoding
  return result;
}

namespace {
PinName unsupported() {
  mbed_die();
  return NC;
}
}

MoteusHwPins FindHardwarePins(FamilyAndVersion fv) {
  MoteusHwPins result;

  const auto hv = fv.hw_version;

  if (fv.family == 0) {
    // === FlexNode phase-order fix (netlist-verified 2026-07-21) ===
    // The FlexNode v1.0 PCB swaps the A/C motor PWM outputs in copper
    // relative to moteus r4.11: PA0 -> INHC (winding C) and
    // PA2 -> INHA (winding A).  Current-sense connectivity is stock
    // (PB0<-SOA, PB1<-SOB, PB2<-SOC; only the schematic net *labels*
    // CUR1/CUR3 were renamed).  Unfixed, the firmware would drive
    // winding C while regulating the current sensed on winding A.
    //
    // Fix on the drive side only: swap which pins logical PWM channels
    // 1 and 3 map to.  ConfigurePwmTimer()/FindCcr() resolve CCR
    // registers from these pins generically, so bldc_servo.cc stays
    // 100% stock and phase_invert / calibration semantics are
    // preserved (a sense-side swap would silently break if
    // motor.phase_invert were ever set).  See ../docs/firmware.md.
    result.pwm1 = PA_2_ALT0;  // logical phase 1 drives PA2 -> INHA (winding A)
    result.pwm3 = PA_0_ALT0;  // logical phase 3 drives PA0 -> INHC (winding C)

    result.vsense =
        (hv <= 4 ? PA_8 :
         hv >= 5 ? PB_12_ALT0 :
         unsupported());

    result.msense =
        // Note, the hv <=3 versions don't actually have a motor sense
        // ADC at all.  So we just pick it the same as the other
        // temperature sense so that things don't get broken.
        (hv <= 3 ? PA_9 :
         hv == 4 ? PB_12 :
         hv >= 5 ? PA_8 :
         unsupported());

    // === FlexNode vsense rescale (as-built divider deviation) ===
    // The bus-voltage divider was intended as R29 100k / R30 4.7k
    // (73.5 V full-scale, the stock r4.11 0.017947 scale), but v1.0
    // consolidated every nominal-1k resistor onto the 1.2 k 1% BOM
    // line (C138040) -- R30 included (value confirmed by the
    // designer).  Rather than rework every board, the firmware
    // constant encodes the as-built ratio:
    //   3.3 V * (100k + 1.2k) / 1.2k / 4096 = 0.067944 V/count
    //   (278.3 V full-scale, 68 mV/LSB -- negligible for control).
    // A 0.01 uF filter cap parallels R30; it does not affect the DC
    // ratio.  MANDATORY bench check at first boot: compare bus_V
    // telemetry against a DMM and trim this constant if they disagree
    // (1% resistors => expect agreement within ~1.5%).
    result.vsense_adc_scale =
        (hv <= 5 ? 0.00884f : 0.067944f);  // FlexNode as-built (stock r4.11: 0.017947f)

    result.drv8323_enable = PA_3;
    result.drv8323_hiz = PB_7;
    result.drv8323_cs = PC_4;

    result.drv8323_mosi = PA_7;
    result.drv8323_miso = PA_6;
    result.drv8323_sck = PA_5;
    result.drv8323_fault = PB_6;

    // FlexNode: PF0 carries the WS2812B data line (see fw/ws2812_led.h),
    // so the stock active-low debug LED is disconnected (NC) rather than
    // left toggling the pixel data pin.  PF1 (stock power LED) is
    // unconnected on FlexNode; driving it is harmless.
    result.debug_led1 = NC;
    result.ws2812 = PF_0;
    result.power_led = PF_1;

    // We've picked these particular pins so that all 3 channels are
    // one of the "slow" channels so they will have similar analog
    // performance characteristics.
    result.current1 = PB_0_ALT0;
    result.current2 = PB_1;
    result.current3 = PB_2;

    result.as5047_cs = PC_6;   // FlexNode: AS5047 CS relocated from PB11 (PB11 now 5V current-sense ADC)

    result.can_td = PA_12;
    result.can_rd = PA_11;

    result.debug1 = PC_14;
    result.debug2 = PC_15;

    result.power_P_l_W = 900.0f;
    result.power_V_l = 30.0f;

    result.power_P_h_W = 400.0f;
    result.power_V_h = 38.0f;
  } else {
    // FlexNode is always family 0; the moteus n1/c1/x1 (family 1/2/3)
    // pin maps have been removed.
    MJ_ASSERT(false);
  }

  return result;
}

// NOTE: This must remain safe to call from a HardFault context.  That
// means: no dynamic allocation, no locks, no recursion, no reliance on
// C++ static constructors that may not yet have run, and no calls into
// mbed helpers that assert on `NC` pins.  If `g_hw_pins` has not been
// populated yet, all DRV8323 control pins are `NC` -- but in that
// window the GPIOs are still in MCU reset state and the on-board
// pull-downs hold the driver off, so there is nothing to disable.
void MoteusEnsureOff() {
  if (moteus::g_hw_pins.drv8323_enable != NC) {
    gpio_t power;
    gpio_init_out(&power, moteus::g_hw_pins.drv8323_hiz);
    gpio_write(&power, 0);

    // Also, disable the DRV8323 entirely, because, hey, why not.
    gpio_t enable;
    gpio_init_out(&enable, moteus::g_hw_pins.drv8323_enable);
    gpio_write(&enable, 0);
  }

  // We want to ensure that our primary interrupt is not running.
  // Which one it is could vary, so just turn them all off.
  NVIC_DisableIRQ(TIM1_UP_TIM16_IRQn);
  NVIC_DisableIRQ(TIM2_IRQn);
  NVIC_DisableIRQ(TIM3_IRQn);
  NVIC_DisableIRQ(TIM4_IRQn);
  NVIC_DisableIRQ(TIM5_IRQn);
}

}
