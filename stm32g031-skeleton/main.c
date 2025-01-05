// Copyright (c) 2023 Cesanta Software Limited
// All rights reserved

#include "hal.h"
#include "str.h"

#define BLINK_PERIOD_MS 500  // LED blinking period in millis
static struct spi spi_pins = {
    .miso = PIN('B', 4),
    .mosi = PIN('A', 0),
    .clk = PIN('A', 1),
    .cs = PIN('A', 4),
};
uint32_t SystemCoreClock = CPU_FREQUENCY;

void SystemInit(void) {  // Called automatically by startup code
  rng_init();
  SysTick_Config(CPU_FREQUENCY / 1000);  // Sys tick every 1ms
}

static volatile uint64_t s_ticks;  // Milliseconds since boot
void SysTick_Handler(void) {       // SyStick IRQ handler, triggered every 1ms
  s_ticks++;
}

static void pc(char ch, void *arg) {
  uart_write_byte(arg, ch);
}
#define log(fmt, ...) xprintf(pc, UART_DEBUG, fmt, __VA_ARGS__);

static void blink(void) {
  gpio_toggle(LED);
  // log("LED: %s, tick: %llu\n", gpio_read(LED) ? "on" : "off", s_ticks);
}

int main(void) {
  gpio_output(LED);               // Setup green LED
  uart_init(UART_DEBUG, 115200);  // Initialise debug printf

  log("Starting, CPU freq %lu MHz\n", SystemCoreClock / 1000000);
  spi_init(&spi_pins);

  uint64_t timer = 0;
  for (;;) {
    if (timer_expired(&timer, BLINK_PERIOD_MS, s_ticks)) {
      blink();
    }
  }

  return 0;
}
