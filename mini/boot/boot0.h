/* BoltBoot boot0: what its parts give each other. */
#pragma once
#include <stdint.h>

int  clock_init(void);                 /* 0 if every PLL locked                  */
int  clock_cpu_mhz(uint32_t mhz);

int  pmic_init(void);                  /* AXP313A: VDD-CPU 1.0 V, VDD-DRAM 1.1 V */
int  pmic_read(uint8_t reg, uint8_t *v);
int  pmic_write(uint8_t reg, uint8_t v);
void pmic_power_off(void);

uint64_t dram_init(void);              /* bytes of DRAM at 0x40000000, 0 if none */

void wdt_start(void);                  /* resets the board unless fed for 16 s   */
void wdt_stop(void);
