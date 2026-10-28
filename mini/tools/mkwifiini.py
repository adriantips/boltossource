#!/usr/bin/env python3
"""The UWE5622's Wi-Fi calibration, from its .ini text to the binary the firmware takes.

    mkwifiini.py wifi_2355b001_1ant.ini wifi_ini.bin

Unisoc's Linux driver (uwe5622/unisocwifi/rf_marlin3.c) reads the .ini at every
start into `struct wifi_conf_t` and sends it to the firmware in three pieces
(WIFI_CMD_DOWNLOAD_INI). Mini does the reading here, once, at build time, and
writes the struct as it lies in memory on an ARM: section 1 (328 bytes),
section 2 (1464 bytes), then the RF data (the length, then the bytes).
The parsing follows rf_marlin3.c: a line is a name and numbers, in any of
decimal, 0x hex and 0-prefixed octal.
"""
import struct, sys

# name -> (offset in struct wifi_conf_t, element size)
T = {
    "Major": (0, 2), "Minor": (2, 2),
    "Calib_Bypass": (4, 2), "TxChain_Mask": (6, 1), "RxChain_Mask": (7, 1),
    "DPD_LUT_idx": (8, 1), "TPC_Goal_Chain0": (16, 2), "TPC_Goal_Chain1": (32, 2),
    "2G_Channel_Chain0": (112, 1), "2G_Channel_Chain1": (126, 1),
    "5G_Channel_Chain0": (140, 1), "5G_Channel_Chain1": (165, 1),
    "11b_Power": (192, 1), "11ag_Power": (196, 1), "11n_Power": (204, 1), "11ac_Power": (221, 1),
    "Green_WIFI_offset": (244, 1), "HT40_Power_offset": (245, 1), "VHT40_Power_offset": (246, 1),
    "VHT80_Power_offset": (247, 1), "SAR_Power_offset": (248, 1), "Mean_Power_offset": (249, 1),
    "TPC_mode": (250, 1), "MAGIC_word": (251, 1),
    "reg_domain1": (252, 4), "reg_domain2": (256, 4),
    "BW20M": (260, 1), "BW40M": (299, 1), "BW80M": (320, 1),
    "DFS_switch": (1576, 1), "power_save_switch": (1577, 1),
    "ex-Fem_and_ex-LNA_param_setup": (1578, 1), "rssi_report_diff": (1579, 1),
    "address": (1580, 4), "value": (1644, 4),
    "rf_config": (1796, 1),
}
for i in range(8):
    T["Chain0_LUT_%d" % i] = (48 + 4 * i, 1)
    T["Chain1_LUT_%d" % i] = (80 + 4 * i, 1)
for i in range(39):
    T["Chain0_%d" % (i + 1)] = (328 + 16 * i, 1)
    T["Chain1_%d" % (i + 1)] = (952 + 16 * i, 1)
for i, n in enumerate(["bt_performance_cfg0", "bt_performance_cfg1", "wifi_performance_cfg0",
                       "wifi_performance_cfg2", "strategy_cfg0", "strategy_cfg1", "strategy_cfg2",
                       "compatibility_cfg0", "compatibility_cfg1", "ant_cfg0", "ant_cfg1",
                       "isolation_cfg0", "isolation_cfg1"] + ["reserved_cfg%d" % k for k in range(8)]):
    T[n] = (1708 + 4 * i, 4)

SIZE = 1792 + 4 + 1500


def ctype(c):
    if ("a" <= c <= "z" or "A" <= c <= "Z") and c not in "xX" or c == "_":
        return 1
    if "0" <= c <= "9" or c == "-":
        return 2
    if c in "xX.":
        return 3
    if c in "\0\r\n#":
        return 4
    return 0


def number(tok, last):
    s, neg = tok, False
    if s.startswith("-"):
        neg, s = True, s[1:]
    try:
        if s[:2].lower() == "0x":
            v = int(s[2:], 16)
        elif len(s) > 1 and s[0] == "0":
            v = int(s, 8)
        else:
            v = int(s, 10)
    except ValueError:
        return last                       # kstrtol failed: rf_marlin3.c keeps the old value
    return -v if neg else v


def parse_line(line):
    name, vals, tok, kind, last = None, [], "", -1, 0
    for c in line + "\0":
        t = ctype(c)
        if t in (1, 2, 3):
            tok += c
            if kind == -1:
                kind = 2 if t == 2 else 1
            elif kind == 2 and t == 1:
                kind = 1
            continue
        if kind != -1:
            if kind == 1 and name is None:
                name = tok
            else:
                last = number(tok, last)
                vals.append(last)
            kind, tok = -1, ""
        if t == 4:
            break
    return name, vals


def main(src, dst):
    conf = bytearray(SIZE)
    text = open(src, "rb").read().decode("latin-1")
    for line in text.replace("\r", "\n").split("\n"):
        if len(line) < 5:
            continue
        name, vals = parse_line(line)
        if name not in T:
            continue
        off, size = T[name]
        fmt = {1: "<B", 2: "<H", 4: "<I"}[size]
        for i, v in enumerate(vals):
            struct.pack_into(fmt, conf, off + i * size, v & ((1 << (8 * size)) - 1))
        if name == "rf_config":
            struct.pack_into("<i", conf, 1792, len(vals))
    rf_len = struct.unpack_from("<i", conf, 1792)[0]
    open(dst, "wb").write(bytes(conf[:1796 + rf_len]))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
