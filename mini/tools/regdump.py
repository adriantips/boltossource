#!/usr/bin/env python3
"""Save the H618's register state from a running (development) BoltOS Mini.

    regdump.py [OUT]       (default build/mini-fw/regs-<date>.txt)

Reads, through the kernel's /regs endpoint, the blocks the firmware sets up
before the kernel runs -- the clocks, the DRAM controller and its PHY, the
security switches, the GIC, the CPU configuration -- so one firmware's
leftovers (U-Boot + TF-A) can be compared with another's (BoltBoot):

    regdump.py build/mini-fw/uboot-state.txt     # with U-Boot on the card
    regdump.py build/mini-fw/boltboot-state.txt  # with BoltBoot
    diff build/mini-fw/uboot-state.txt build/mini-fw/boltboot-state.txt

Some of these the kernel changes on its own (HDMI's clocks, USB, Ethernet);
the DRAM controller, the CPU PLL and the security switches it never does.
BOLTOS_PI=192.168.86.44 reaches the Pi over Ethernet.
"""
import os, sys, time, urllib.request

PI = "http://" + os.environ.get("BOLTOS_PI", "10.77.0.1")

BLOCKS = [
    ("SYSCON (SRAM ctrl, version)", 0x03000000, 0x100),
    ("CCU (clocks, resets)",         0x03001000, 0x1000),
    ("SPC (secure peripherals)",     0x03008000, 0x100),
    ("PIO (pins A-I)",               0x0300B000, 0x300),
    ("GICD (first words, groups)",   0x03021000, 0x100),
    ("GICC",                         0x03022000, 0x20),
    ("DRAM COM",                     0x047FA000, 0x1000),
    ("DRAM CTL0",                    0x047FB000, 0x1000),
    ("DRAM PHY0",                    0x04800000, 0x1000),
    ("RTC (incl. general-purpose)",  0x07000000, 0x200),
    ("R_CPUCFG",                     0x07000400, 0x200),
    ("R_PRCM",                       0x07010000, 0x400),
    ("R_PIO (pins L)",               0x07022000, 0x40),
    ("CPUCFG (RVBAR, core power)",   0x09010000, 0x100),
]


def fetch(addr, nbytes):
    out = []
    for a in range(addr, addr + nbytes, 4096):
        n = min(1024, (addr + nbytes - a) // 4)
        url = "%s/regs?a=0x%x&n=%d" % (PI, a, n)
        with urllib.request.urlopen(url, timeout=10) as r:
            out.append(r.read().decode().rstrip("\n"))
    return "\n".join(out)


def main(argv):
    out = argv[0] if argv else "build/mini-fw/regs-%s.txt" % time.strftime("%Y%m%d-%H%M%S")
    lines = ["# H618 registers from %s at %s" % (PI[7:], time.strftime("%Y-%m-%d %H:%M"))]
    for name, a, n in BLOCKS:
        lines.append("\n## %s @ 0x%08x" % (name, a))
        lines.append(fetch(a, n))
        sys.stdout.write("  %-30s %5d bytes\n" % (name, n))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    open(out, "w").write("\n".join(lines) + "\n")
    print("saved %s" % out)


if __name__ == "__main__":
    main(sys.argv[1:])
