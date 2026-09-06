/* FlexNode first-light test: WS2812B on PF0, warm -> blue fade ("chime").
 * Bare-metal STM32G474, runs on the 16 MHz HSI straight out of reset.
 * Touches ONLY: RCC (GPIOF clock), GPIOF pin 0, SysTick.  Nothing else
 * is configured: DRV8353S EN/INHx/HIZ stay in their reset state (inputs). */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define RCC_AHB2ENR   REG(0x40021000 + 0x4C)
#define GPIOF_MODER   REG(0x48001400 + 0x00)
#define GPIOF_OTYPER  REG(0x48001400 + 0x04)
#define GPIOF_OSPEEDR REG(0x48001400 + 0x08)
#define GPIOF_PUPDR   REG(0x48001400 + 0x0C)
#define GPIOF_BSRR    REG(0x48001400 + 0x18)
#define SYST_CSR      REG(0xE000E010)
#define SYST_RVR      REG(0xE000E014)
#define SYST_CVR      REG(0xE000E018)

#define PIN 0u
#define SET_MASK   (1u << PIN)
#define RESET_MASK (1u << (PIN + 16))

#define NOP1 __asm volatile("nop")
#define NOP4 NOP1; NOP1; NOP1; NOP1

/* 16 MHz -> 62.5 ns / cycle.  '1': ~810 ns high / ~650 ns low.  '0': ~310 ns high / ~1.1 us low (measured from disassembly). */
static inline __attribute__((always_inline)) void bit1(void) {
  GPIOF_BSRR = SET_MASK;  NOP4; NOP4; NOP1;
  GPIOF_BSRR = RESET_MASK; NOP4; NOP1;
}
static inline __attribute__((always_inline)) void bit0(void) {
  GPIOF_BSRR = SET_MASK;  NOP1; NOP1; NOP1;
  GPIOF_BSRR = RESET_MASK; NOP4; NOP4; NOP4;
}

static void send_byte(uint8_t b) {
  for (int i = 7; i >= 0; --i) { if (b & (1u << i)) bit1(); else bit0(); }
}

static void show(uint8_t r, uint8_t g, uint8_t b, int n) {
  __asm volatile("cpsid i");
  for (int i = 0; i < n; ++i) { send_byte(g); send_byte(r); send_byte(b); }  /* WS2812B is GRB */
  __asm volatile("cpsie i");
  GPIOF_BSRR = RESET_MASK;
}

static void delay_ticks(uint32_t ticks10ms) {   /* SysTick reload = 10 ms @16 MHz */
  for (uint32_t i = 0; i < ticks10ms; ++i) { while (!(SYST_CSR & (1u << 16))) {} }
}

/* simple gamma: (v/255)^2 * 255 */
static uint8_t gam(uint32_t v) { return (uint8_t)((v * v) / 255); }

static uint8_t lerp(uint8_t a, uint8_t b, uint32_t t, uint32_t T) {  /* smoothstep */
  uint32_t x = (t * 1024) / T;                 /* 0..1024 */
  uint32_t s = (x * x * (3 * 1024 - 2 * x)) / (1024u * 1024u);  /* 0..1024 */
  return (uint8_t)((a * (1024 - s) + b * s) / 1024);
}

int main(void) {
  RCC_AHB2ENR |= (1u << 5);                    /* GPIOFEN */
  (void)RCC_AHB2ENR;
  GPIOF_BSRR = RESET_MASK;
  GPIOF_OTYPER &= ~(1u << PIN);                /* push-pull */
  GPIOF_OSPEEDR |= (3u << (PIN * 2));          /* very high speed */
  GPIOF_PUPDR &= ~(3u << (PIN * 2));
  GPIOF_MODER = (GPIOF_MODER & ~(3u << (PIN * 2))) | (1u << (PIN * 2));  /* output */

  SYST_RVR = 160000u - 1u;                     /* 10 ms */
  SYST_CVR = 0;
  SYST_CSR = 5;                                /* enable, core clock, no irq */

  const uint8_t WARM_R = 255, WARM_G = 120, WARM_B = 10;   /* candle-warm */
  const uint8_t BLUE_R = 0,   BLUE_G = 40,  BLUE_B = 255;
  const uint32_t BRIGHT = 96;                  /* 0..255 overall brightness cap (~38 %) */

  delay_ticks(50);                             /* 0.5 s dark after boot */
  for (;;) {
    /* 0 -> warm, 0.8 s */
    for (uint32_t t = 0; t <= 80; ++t) {
      uint8_t r = lerp(0, WARM_R, t, 80), g = lerp(0, WARM_G, t, 80), b = lerp(0, WARM_B, t, 80);
      show(gam(r * BRIGHT / 255), gam(g * BRIGHT / 255), gam(b * BRIGHT / 255), 4); delay_ticks(1);
    }
    delay_ticks(40);
    /* warm -> blue, 2.0 s */
    for (uint32_t t = 0; t <= 200; ++t) {
      uint8_t r = lerp(WARM_R, BLUE_R, t, 200), g = lerp(WARM_G, BLUE_G, t, 200), b = lerp(WARM_B, BLUE_B, t, 200);
      show(gam(r * BRIGHT / 255), gam(g * BRIGHT / 255), gam(b * BRIGHT / 255), 4); delay_ticks(1);
    }
    delay_ticks(100);                          /* hold blue 1 s */
    /* blue -> off, 1.2 s */
    for (uint32_t t = 0; t <= 120; ++t) {
      uint8_t r = lerp(BLUE_R, 0, t, 120), g = lerp(BLUE_G, 0, t, 120), b = lerp(BLUE_B, 0, t, 120);
      show(gam(r * BRIGHT / 255), gam(g * BRIGHT / 255), gam(b * BRIGHT / 255), 4); delay_ticks(1);
    }
    delay_ticks(150);                          /* dark 1.5 s, repeat */
  }
}

/* ---- startup ---- */
extern uint32_t _estack, _sbss, _ebss;
void Reset_Handler(void) {
  for (uint32_t *p = &_sbss; p < &_ebss; ++p) *p = 0;
  main();
  for (;;) {}
}
void Default_Handler(void) { for (;;) {} }
__attribute__((section(".isr_vector"), used))
const void *vectors[16 + 8] = {
  &_estack, Reset_Handler,
  Default_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler,
  0, 0, 0, 0, Default_Handler, Default_Handler, 0, Default_Handler, Default_Handler,
};
