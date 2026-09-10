#!/usr/bin/env python3
"""Software: installing something without opening a terminal.

`pkg install` is a reasonable thing to ask of a developer and an unreasonable
thing to ask of anybody else, and installing software is the one administrative
task a person using a machine cannot avoid.

The assertion worth making is not that the window draws -- it is that a click
in it drives the *real* package manager: the same index, the same install path,
the same database the command reads. So this clicks the button, then goes to a
Terminal and asks `pkg` what happened.

The window is maximised first so its geometry is a fact rather than a guess:
the compositor cascades new windows, so their position depends on how many were
registered before them.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"

# Client geometry once maximised, from kernel/app_software.c and the
# compositor's TITLE_H: the first row's action button.
TITLE_H, HEAD_H, ROW_H, BTN_W, BTN_H = 32, 76, 54, 78, 26
SCREEN_W = 1024


def row_button(row):
    x = SCREEN_W - BTN_W - 16 + BTN_W // 2
    ry = TITLE_H + HEAD_H + row * ROW_H
    return x, ry + (ROW_H - BTN_H) // 2 + BTN_H // 2


def sign_in(vm):
    vm.expect("BoltOS is up. Launching desktop.", timeout=150)
    time.sleep(4.0)
    vm.type(LOGIN_USER); vm.key("tab"); vm.type(LOGIN_PASS); vm.key("ret")
    time.sleep(3.0)


def open_app(vm, name):
    vm.key("meta_l")
    vm.type(name)
    vm.key("ret")
    time.sleep(2.0)


def main():
    with BoltVM(name="software", net=False) as vm:
        sign_in(vm)

        open_app(vm, "Software")
        vm.key("up", meta=True)              # maximise: known geometry
        time.sleep(1.5)
        shot = vm.screenshot("software.ppm")
        assert os.path.getsize(shot) > 1000, "the Software window drew nothing"
        print("  ok: the Software window opens and draws")

        # Click the first row's button and let the deferred job run. Whether it
        # says Install or Remove depends on what the catalogue happens to list
        # first, and the test should not care -- what matters is that a click
        # reaches the package manager and the package manager reports back.
        x, y = row_button(0)
        vm.clear()
        vm.click(x, y)
        assert vm.wait_for("[notify:", timeout=180), \
            "clicking the button did nothing:\n" + vm.serial()[-1200:]
        log = vm.serial()
        assert "Software:" in log, "no notification from the app:\n" + log[-1200:]
        first_installed = "Installed" in log
        assert first_installed or "Removed" in log, \
            "the app neither installed nor removed:\n" + log[-1200:]
        print("  ok: a click runs the package manager and reports what it did")

        # And back again, which proves both verbs and that the row's button
        # followed the change in state rather than staying on one label.
        time.sleep(1.5)
        vm.clear()
        vm.click(x, y)
        assert vm.wait_for("[notify:", timeout=180), \
            "the second click did nothing:\n" + vm.serial()[-1200:]
        log = vm.serial()
        want = "Removed" if first_installed else "Installed"
        assert want in log, \
            "the button did not flip to %s:\n%s" % (want, log[-1200:])
        print("  ok: the button follows the package's state, both ways")

        # Which package was it? The notification names it, and that is the name
        # to interrogate the package manager about.
        line = [l for l in log.splitlines() if "Software: " + want in l][-1]
        name = line.split(want, 1)[1].strip()
        assert name, "the notification did not name the package:\n" + line

        # The real check: the package database itself, read in a Terminal by
        # the command. If the window and the command disagree about what is
        # installed, one of them is lying about having done the work.
        open_app(vm, "Terminal")
        vm.clear()
        vm.type("pkg info %s\n" % name)
        vm.expect("status:", timeout=60)
        out = vm.serial()
        expect = "not installed" if want == "Removed" else "installed"
        status = out.split("status:", 1)[1].split("\n")[0].strip()
        assert status == expect, \
            "the window said %s but pkg says '%s'" % (want, status)
        print("  ok: `pkg info %s` agrees with what the window did" % name)

        vm.expect_absent("KERNEL PANIC")

    print("PASS software: the package manager has a window, and it is the real one")
    return 0


if __name__ == "__main__":
    sys.exit(main())
