@echo off
REM ===========================================================================
REM  Boot BoltOS in QEMU on Windows. run.sh / run-uefi.sh do the same job under
REM  msys2 bash; this is the version you can double-click.
REM
REM    run.bat              legacy BIOS, boots iso\os.img off the IDE disk
REM    run.bat uefi         OVMF firmware, runs esp\EFI\BOOT\BOOTX64.EFI
REM    run.bat usb          legacy BIOS, boots iso\boltusb.img as a USB stick
REM    run.bat usb-uefi     OVMF, same image, in through its FAT32 ESP
REM
REM  Anything after the mode goes straight to QEMU, so "run.bat uefi -display
REM  none" and "run.bat -s -S" both work.
REM
REM  Serial lands in this console window. The e1000 sits on slirp NAT, which
REM  hands out 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 -- the addresses
REM  the kernel already expects, so the browser gets real internet.
REM
REM  8 GiB on purpose: the UEFI loader only ever allocated below 4 GiB by
REM  accident of a small VM, and a 4G run cannot see that class of bug.
REM ===========================================================================
setlocal enabledelayedexpansion
set "ROOT=%~dp0"

REM -- Find QEMU. BOLTOS_QEMU wins; otherwise the two places it installs to.
set "QEMU=%BOLTOS_QEMU%"
if not defined QEMU if exist "C:\msys64\ucrt64\bin\qemu-system-x86_64.exe" set "QEMU=C:\msys64\ucrt64\bin\qemu-system-x86_64.exe"
if not defined QEMU if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not defined QEMU for %%Q in (qemu-system-x86_64.exe) do set "QEMU=%%~$PATH:Q"
if not defined QEMU (
    echo ERROR: QEMU not found. Install it, or point BOLTOS_QEMU at the exe.
    exit /b 1
)

REM -- Mode, then whatever is left over for QEMU.
set "MODE=bios"
set "ARG1=%~1"
if /i "%ARG1%"=="bios"     ( set "MODE=bios"     & shift )
if /i "%ARG1%"=="uefi"     ( set "MODE=uefi"     & shift )
if /i "%ARG1%"=="usb"      ( set "MODE=usb"      & shift )
if /i "%ARG1%"=="usb-uefi" ( set "MODE=usb-uefi" & shift )
set "EXTRA="
:collect
if "%~1"=="" goto collected
set "EXTRA=!EXTRA! %1"
shift
goto collect
:collected

REM -- The UEFI modes need firmware plus a writable copy of its variable store.
set "FW="
if /i "%MODE:~-4%"=="uefi" (
    set "CODE=%BOLTOS_OVMF_CODE%"
    if not defined CODE if exist "C:\msys64\ucrt64\share\qemu\edk2-x86_64-code.fd" set "CODE=C:\msys64\ucrt64\share\qemu\edk2-x86_64-code.fd"
    if not defined CODE if exist "C:\Program Files\qemu\share\edk2-x86_64-code.fd" set "CODE=C:\Program Files\qemu\share\edk2-x86_64-code.fd"
    if not defined CODE (
        echo ERROR: no UEFI firmware beside QEMU. Set BOLTOS_OVMF_CODE.
        exit /b 1
    )
    set "VARS=%BOLTOS_OVMF_VARS%"
    if not defined VARS if exist "C:\msys64\ucrt64\share\qemu\edk2-i386-vars.fd" set "VARS=C:\msys64\ucrt64\share\qemu\edk2-i386-vars.fd"
    if not defined VARS if exist "C:\Program Files\qemu\share\edk2-i386-vars.fd" set "VARS=C:\Program Files\qemu\share\edk2-i386-vars.fd"
    if not exist "%ROOT%iso\OVMF_VARS.fd" if defined VARS copy /y "!VARS!" "%ROOT%iso\OVMF_VARS.fd" >nul
    set FW=-drive if=pflash,format=raw,unit=0,readonly=on,file="!CODE!"
    if exist "%ROOT%iso\OVMF_VARS.fd" set FW=!FW! -drive if=pflash,format=raw,unit=1,file="%ROOT%iso\OVMF_VARS.fd"
)

REM -- What we boot off, and how it is plugged in. The xHCI has to be created
REM -- before anything says bus=xhci.0, so it lives here rather than below.
set XHCI=-device qemu-xhci,id=xhci
set BOOT=
if "%MODE%"=="bios" (
    if not exist "%ROOT%iso\os.img" ( echo ERROR: iso\os.img missing -- run build.sh first. & exit /b 1 )
    set BOOT=-drive id=boot,file="%ROOT%iso\os.img",format=raw,if=none -device ide-hd,drive=boot,bus=ide.0,unit=0 -boot order=c
)
if "%MODE%"=="uefi" (
    if not exist "%ROOT%esp\EFI\BOOT\BOOTX64.EFI" ( echo ERROR: esp\ not staged -- run build.sh first. & exit /b 1 )
    REM QEMU serves esp\ as a FAT volume, so there is no ESP image to keep in sync.
    set BOOT=-drive id=esp,file=fat:rw:"%ROOT%esp",format=raw,if=none -device ide-hd,drive=esp,bus=ide.0,unit=0
)
if "%MODE:~0,3%"=="usb" (
    if not exist "%ROOT%iso\boltusb.img" ( echo ERROR: iso\boltusb.img missing -- run build.sh first. & exit /b 1 )
    set BOOT=-drive id=usbimg,file="%ROOT%iso\boltusb.img",format=raw,if=none -device usb-storage,drive=usbimg,bus=xhci.0,bootindex=0
)

echo [*] %MODE% boot with "%QEMU%"
echo [*] Serial output shows up here. Ctrl+C, or close the QEMU window, to stop.
echo.

"%QEMU%" !FW! !XHCI! !BOOT! ^
    -accel whpx,kernel-irqchip=off -accel tcg,thread=multi ^
    -drive id=hdd,file="%ROOT%iso\disk-hdd.img",format=raw,if=none ^
    -device ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200 ^
    -drive id=ssd,file="%ROOT%iso\disk-ssd.img",format=raw,if=none ^
    -device ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1 ^
    -drive id=nvm,file="%ROOT%iso\disk-nvme.img",format=raw,if=none ^
    -device nvme,drive=nvm,serial=BOLTNVME01 ^
    -device usb-kbd,bus=xhci.0 ^
    -device usb-mouse,bus=xhci.0 ^
    -cpu max ^
    -m 8G ^
    -smp 4 ^
    -rtc base=utc ^
    -vga std -global VGA.vgamem_mb=128 ^
    -audiodev dsound,id=snd0 ^
    -machine pcspk-audiodev=snd0 ^
    -device AC97,audiodev=snd0 ^
    -device intel-hda,id=hda ^
    -device hda-duplex,bus=hda.0,audiodev=snd0 ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0 ^
    -serial stdio ^
    -no-reboot -no-shutdown !EXTRA!

echo.
echo [*] QEMU exited (%ERRORLEVEL%).
endlocal
