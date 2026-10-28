/* BoltOS Mini - mini/bt.h: Bluetooth on the UWE5622 (mini/bt.c). */
#pragma once
#include <stdint.h>

enum { BT_OFF, BT_STARTING, BT_FAILED, BT_READY };
enum { BTD_FOUND, BTD_PAIRING, BTD_PAIRED, BTD_CONNECTED };
#define BT_MAXDEV 40

typedef struct {
    uint8_t  addr[6];                    /* little-endian, as HCI has it          */
    uint8_t  le, addr_type;              /* Bluetooth LE, and its address type    */
    char     name[48];
    char     addr_str[18];
    const char *kind;                    /* "Keyboard", "Mouse", "Headphones", ... */
    int8_t   rssi;
    int      state;                      /* BTD_*                                 */
    uint32_t cod;                        /* classic: class of device              */
    uint16_t appearance;                 /* LE                                    */
} bt_device_t;

int  bt_start(void);                     /* the chip (if Wi-Fi hasn't), then HCI  */
int  bt_state(void);
const char *bt_why(void);
int  bt_scan(void);                      /* about 10 s of looking                 */
int  bt_scanning(void);
int  bt_device_count(void);
const bt_device_t *bt_device(int i);
int  bt_pair(int i);
int  bt_forget(int i);
int  bt_cancel(int i);                   /* stop pairing with it                  */
int  bt_disconnect(int i);
void bt_stop(void);                      /* off, and off after a restart too      */
int  bt_changes(void);                   /* bumps when something worth redrawing happens */
void bt_poll(void);                      /* from the main loop                    */
int  bt_debug(char *out, int cap);
