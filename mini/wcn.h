/* BoltOS Mini - mini/wcn.h: the UWE5622 Wi-Fi/Bluetooth chip (mini/wcn.c). */
#pragma once
#include <stdint.h>

enum { WCN_OFF, WCN_STARTING, WCN_READY, WCN_FAILED };

int  wcn_start(void);                    /* load the firmware and start it; blocks ~1 s */
int  wcn_state(void);
const char *wcn_why(void);               /* why it failed                              */
uint32_t wcn_chipid(void);
int  wcn_dt_read(uint32_t addr, void *buf, uint32_t len);
int  wcn_dt_write(uint32_t addr, const void *buf, uint32_t len);
int  wcn_reg_read(uint32_t addr, uint32_t *v);
int  wcn_reg_write(uint32_t addr, uint32_t v);
int  wcn_debug(char *out, int cap);

/* The chip's firmware and settings: in this kernel, or -- in a kernel that
 * came as an update from boltos.dev, which doesn't hand Unisoc's code out --
 * the copy an earlier kernel left on the card, in /System/Firmware. */
enum { WCN_FW_MODEM, WCN_FW_WIFI_INI, WCN_FW_BT_PSKEY, WCN_FW_BT_RF, WCN_NFW };
const uint8_t *wcn_blob(int which, uint32_t *len);   /* NULL, len 0, if neither has it */
void wcn_keep_firmware(void);            /* at boot: this kernel's copy onto the card     */
int  wcn_firmware_safe(void);            /* 1 if a kernel without it would still have it  */

/* Packets on the chip's channels (wifi.c: 8 out, 22 and 24 in). */
typedef void (*wcn_rx_fn)(int chn, const uint8_t *data, int len);
void wcn_set_rx(int chn, wcn_rx_fn fn);
int  wcn_send(int chn, const void *data, int len);
void wcn_poll(void);                     /* read what the chip has; in mini_pump */
