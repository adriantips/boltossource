/* BoltOS Mini - mini/sdio.h: the H618's MMC1 as an SDIO host (mini/sdio.c). */
#pragma once
#include <stdint.h>

int     sdio_init(void);                 /* power the module up and select it; 0 if it answered */
void    sdio_module_reset(int hold);     /* PG18: 1 holds the module in reset           */
int     sdio_enable_fn(int fn);
int     sdio_enable_irq(int fn);
int     sdio_set_blksz(int fn, uint16_t bs);
int     sdio_cmd52(int write, int fn, uint32_t addr, uint8_t in, uint8_t *out);
uint8_t sdio_readb(int fn, uint32_t addr, int *err);
int     sdio_writeb(int fn, uint32_t addr, uint8_t v);
/* CMD53. buf must be 4-byte aligned, and a read may write up to 3 bytes past len. */
int     sdio_rw(int write, int fn, uint32_t addr, int incr, void *buf, uint32_t len);
int     sdio_irq_pending(void);          /* DAT1 interrupt seen since the last call     */
int     sdio_debug(char *out, int cap);
