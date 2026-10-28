/* ===========================================================================
 *  BoltOS Mini  -  mini/bt.c
 *
 *  Bluetooth on the UWE5622. (Being written: for now it says so.)
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "bt.h"
#include "stdio.h"

int  bt_start(void) { return -1; }
int  bt_state(void) { return BT_OFF; }
const char *bt_why(void) { return "Off."; }
int  bt_scan(void) { return -1; }
int  bt_scanning(void) { return 0; }
int  bt_device_count(void) { return 0; }
const bt_device_t *bt_device(int i) { (void)i; return 0; }
int  bt_pair(int i) { (void)i; return -1; }
int  bt_forget(int i) { (void)i; return -1; }
int  bt_changes(void) { return 0; }
void bt_poll(void) { }
int  bt_debug(char *out, int cap) { return snprintf(out, (size_t)cap, "bt: off\n"); }
