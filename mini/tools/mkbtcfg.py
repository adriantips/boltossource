#!/usr/bin/env python3
"""The UWE5622's Bluetooth settings, from Orange Pi's two .ini files to HCI payloads.

    mkbtcfg.py bt_configure_pskey.ini bt_configure_rf.ini bt_pskey.bin bt_rf.bin

Before its Bluetooth controller will start, the chip wants two vendor commands:
0xFCA0 with the "pskey" (device class, features, address, timings) and 0xFCA2
with the RF power tables. Unisoc's hciattach (hciattach_sprd.c, carried by
CoreELEC's BlueZ) reads the .ini files into structs and sends their fields in
order, little-endian. This writes those two payloads once, at build time; the
kernel puts its own device address into the pskey's (bytes 20-25).
"""
import struct, sys

# (name, how many, bytes each), in the order hciattach_sprd.c sends them
PSKEY = [("device_class", 1, 4), ("feature_set", 16, 1), ("device_addr", 6, 1), ("comp_id", 1, 2),
         ("g_sys_uart0_communication_supported", 1, 1), ("cp2_log_mode", 1, 1), ("LogLevel", 1, 1),
         ("g_central_or_perpheral", 1, 1), ("Log_BitMask", 1, 2), ("super_ssp_enable", 1, 1),
         ("common_rfu_b3", 1, 1), ("common_rfu_w", 2, 4), ("le_rfu_w", 2, 4), ("lmp_rfu_w", 2, 4),
         ("lc_rfu_w", 2, 4), ("g_wbs_nv_117", 1, 2), ("g_wbs_nv_118", 1, 2), ("g_nbv_nv_117", 1, 2),
         ("g_nbv_nv_118", 1, 2), ("g_sys_sco_transmit_mode", 1, 1), ("audio_rfu_b1", 1, 1),
         ("audio_rfu_b2", 1, 1), ("audio_rfu_b3", 1, 1), ("audio_rfu_w", 2, 4),
         ("g_sys_sleep_in_standby_supported", 1, 1), ("g_sys_sleep_master_supported", 1, 1),
         ("g_sys_sleep_slave_supported", 1, 1), ("power_rfu_b1", 1, 1), ("power_rfu_w", 2, 4),
         ("win_ext", 1, 4), ("edr_tx_edr_delay", 1, 1), ("edr_rx_edr_delay", 1, 1), ("tx_delay", 1, 1),
         ("rx_delay", 1, 1), ("bb_rfu_w", 2, 4), ("agc_mode", 1, 1), ("diff_or_eq", 1, 1), ("ramp_mode", 1, 1),
         ("modem_rfu_b1", 1, 1), ("modem_rfu_w", 2, 4), ("BQB_BitMask_1", 1, 4), ("BQB_BitMask_2", 1, 4),
         ("bt_coex_threshold", 8, 2), ("other_rfu_w", 6, 4)]
RF = [("g_GainValue_A", 6, 2), ("g_ClassicPowerValue_A", 10, 2), ("g_LEPowerValue_A", 16, 2),
      ("g_BRChannelpwrvalue_A", 8, 2), ("g_EDRChannelpwrvalue_A", 8, 2), ("g_LEChannelpwrvalue_A", 8, 2),
      ("g_GainValue_B", 6, 2), ("g_ClassicPowerValue_B", 10, 2), ("g_LEPowerValue_B", 16, 2),
      ("g_BRChannelpwrvalue_B", 8, 2), ("g_EDRChannelpwrvalue_B", 8, 2), ("g_LEChannelpwrvalue_B", 8, 2),
      ("LE_fix_powerword", 1, 2), ("Classic_pc_by_channel", 1, 1), ("LE_pc_by_channel", 1, 1),
      ("RF_switch_mode", 1, 1), ("Data_Capture_Mode", 1, 1), ("Analog_IQ_Debug_Mode", 1, 1),
      ("RF_common_rfu_b3", 1, 1), ("RF_common_rfu_w", 5, 4)]


def load(path):
    """name -> list of numbers, parsed as vnd_load_configure() does."""
    vals = {}
    for line in open(path, "rb").read().decode("latin-1").splitlines():
        if not line or line[0] == "#" or "=" not in line:
            continue
        name, rest = line.split("=", 1)
        name = name.strip(" \t")
        rest = rest.split("#", 1)[0]
        nums = []
        for tok in rest.replace(",", " ").replace("=", " ").split():
            nums.append(int(tok, 16) if tok[:2].lower() == "0x" else int(tok, 10))
        vals[name] = nums
    return vals


def pack(spec, vals):
    out = b""
    fmt = {1: "<B", 2: "<H", 4: "<I"}
    for name, n, size in spec:
        v = vals.get(name, [])
        for i in range(n):
            out += struct.pack(fmt[size], (v[i] if i < len(v) else 0) & ((1 << (8 * size)) - 1))
    return out


def main(pskey_ini, rf_ini, pskey_out, rf_out):
    open(pskey_out, "wb").write(pack(PSKEY, load(pskey_ini)))
    open(rf_out, "wb").write(pack(RF, load(rf_ini)))


if __name__ == "__main__":
    main(*sys.argv[1:5])
