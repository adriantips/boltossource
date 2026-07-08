#include <stdint.h>
#include "pit.h"
#include "interrupts.h"
#include "io.h"

static volatile uint64_t ticks = 0;
static uint32_t hz_ = 1000;

uint64_t pit_ticks(void) { return ticks; }
uint32_t pit_hz(void) { return hz_; }

static void on_tick(struct registers *r) { (void)r; ticks++; }

void pit_init(uint32_t hz) {
    hz_ = hz;
    uint32_t divisor = 1193182u / hz;
    outb(0x43, 0x36);                       /* ch0, lo/hi, mode 3, binary */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    irq_install(0, on_tick);
}
