# Using BoltOS

This is the guide for somebody who wants to *use* the machine: install it, sign
in, get on a network, keep files, and get out of trouble. It assumes nothing
about operating systems and nothing about C.

If you want to write software on BoltOS, read [DEVELOPING.md](DEVELOPING.md)
instead. If you want the syscall ABI, read [SDK.md](SDK.md).

- [Before you start](#before-you-start)
- [Installing](#installing)
- [Signing in](#signing-in)
- [The desktop](#the-desktop)
- [Files](#files)
- [Getting on a network](#getting-on-a-network)
- [The terminal](#the-terminal)
- [Accounts and passwords](#accounts-and-passwords)
- [Shutting down](#shutting-down)
- [Troubleshooting](#troubleshooting)
- [What BoltOS cannot do yet](#what-boltos-cannot-do-yet)

---

## Before you start

BoltOS runs on 64-bit x86 machines, on real hardware or in a virtual machine. It
needs roughly:

| | |
| --- | --- |
| CPU | x86-64, one core or more |
| Memory | 512 MiB to boot; 2 GiB to be comfortable |
| Disk | 512 MiB for a minimal install |
| Firmware | Legacy BIOS or UEFI — both work |
| Display | Anything the firmware can set up a linear framebuffer on |
| Network | Intel e1000, or virtio-net (most hypervisors) |

There is no Wi-Fi driver yet. On a laptop with no Ethernet port you will be able
to do everything except get online.

**Try it first.** The safest way to meet BoltOS is to boot the ISO in a virtual
machine and not install anything. Everything in this guide works from the live
image except keeping files across a reboot.

---

## Installing

Installing writes over the whole target disk. There is no dual boot and no
resizing of an existing partition. Pick a disk you are willing to lose.

### 1. Boot the image

Boot `boltos.iso` (or write `os.img` to a USB stick). Within five seconds you
will see the boot menu:

```
1. BoltOS
2. Safe mode
3. Recovery shell
4. BoltOS + fsck
```

Press **1**, or wait — it starts on its own.

### 2. Run the installer

On the desktop, open **Install BoltOS** from the Start menu. It will:

1. show you the disks it can install to, with their size and type;
2. ask which one;
3. ask for the username and password of the account you will sign in with;
4. show the exact layout it is about to write, and wait for you to confirm;
5. write it, with a progress bar.

If you would rather do it from a terminal, the same thing is:

```sh
install                       # list the disks and the layout it would write
install nvme0n1               # interactive: asks for the account, confirms
install nvme0n1 ada hunter2   # unattended, if you already know what you want
```

### 3. Reboot

Remove the install medium and restart. You should get the boot menu again, from
the disk this time, and then a sign-in screen.

### What it wrote

Worth knowing, because it is what makes the disk bootable on both kinds of
firmware:

```
LBA 0        stage1 (the master boot record)
LBA 1..32    stage2
LBA 33       a boot header saying where the kernel is and how big it is
LBA 34..     the kernel
partition 1  a FAT32 EFI system partition holding BOOTX64.EFI and the kernel
partition 2  BoltFS -- your files
```

---

## Signing in

The sign-in screen wants the username and password you chose during install.
Type the name, press **Tab**, type the password, press **Enter**.

Passwords are stored as a PBKDF2-HMAC-SHA256 hash over a random per-account
salt, never in the clear. Repeated wrong guesses back off exponentially, so a
machine left at the sign-in screen cannot be worked through with a word list at
any useful speed.

**There is no password recovery.** If you forget it, the account is gone. Boot
the recovery entry from the boot menu to read your files off the disk.

---

## The desktop

A taskbar at the bottom, a Start button in the middle of it, and your windows
above.

### Getting around

| | |
| --- | --- |
| **Start menu** | Tap the **Win** key, or click the Start button. Start typing to filter the apps. |
| **Switch windows** | **Alt+Tab** forward, **Shift+Alt+Tab** back. |
| **Close a window** | **Alt+F4**, or the × in its title bar. |
| **Show the desktop** | **Win+D**, and again to put everything back. |
| **Snap a window** | Drag it to the left or right edge for half the screen, or the top to maximise. **Win+Left/Right/Up/Down** does the same. |
| **Lock the screen** | **Ctrl+Alt+L**. It also locks itself after ten idle minutes. |
| **Take a screenshot** | **F12**. It lands in your Pictures folder and opens in Photos. |

All of these can be changed — see [Keyboard shortcuts](#keyboard-shortcuts).

### Virtual desktops

There are four, so you can keep what you are doing apart from what you were
doing. The four pips at the left end of the taskbar show which one you are on
and which ones have windows.

| | |
| --- | --- |
| **Win+1** … **Win+4** | Go to that desktop |
| **Ctrl+Alt+Left / Right** | Previous / next desktop |
| **Win+Shift+Left / Right** | Take the current window with you |
| Click a pip | Go to that desktop |

A window lives on one desktop. From the others it is not just hidden — it is out
of the taskbar and out of Alt+Tab, and nothing you click or type can reach it.

### Notifications

The bell in the taskbar keeps everything the machine has told you, with a badge
for what you have not read. Click it to open the notification centre: click a
row to mark it read, the × on a row to remove it, **Clear** to empty the list.
Toasts appear bottom-right for a few seconds; clicking one opens the centre on
it.

Some of these matter. If you see **"Disk write failed. Changes are RAM-only"**,
stop and read [Troubleshooting](#troubleshooting) before you reboot.

### Volume, network and the clock

At the right end of the taskbar. Click the speaker for a volume slider (the
scroll wheel over it works too), the network glyph to see whether you are online
and on what address, and the clock for a calendar.

### Keyboard shortcuts

Every shortcut above is a setting, not a fact. In a terminal:

```sh
keys                          # list every action and its shortcut
keys lock Ctrl+Alt+K          # rebind one
keys show-desktop none        # turn one off
keys reset                    # put all the defaults back
```

A shortcut is modifiers joined with `+` and then one key: `Ctrl`, `Alt`,
`Shift`, `Win`, then something like `Left`, `Tab`, `F9`, or a single letter. A
bare letter is refused — binding `k` on its own would take that letter away from
every program you run. Changes are saved to `/etc/keys.conf` and come back after
a reboot.

### The apps

Twenty-six of them, all in the Start menu. The ones you are most likely to want:

- Browser — a real web browser, over HTTPS.
- File Explorer — your files.
- Notes — writing, saved to disk.
- Terminal — the shell, in a window.
- Code — a programming environment; see [DEVELOPING.md](DEVELOPING.md).
- Settings — themes, resolution, wallpaper, fonts.
- Security — the lock timer, the firewall, and the account list.
- Media — plays sound files. Open a `.wav` from File Explorer, or open the
  app and pick one: the folder it came out of becomes the playlist, and it
  moves on to the next track by itself. It keeps playing while you do something
  else -- minimise it, switch desktop, lock the screen, it carries on.
- Photos, Music, Paint, Sheets, Calendar, Contacts, Calculator, Clock, and a
  shelf of games including DOOM.

---

## Files

Your files live in `/home/YOURNAME`, with `Documents`, `Downloads`, `Pictures`,
`Music` and `Desktop` inside it. Anything you put on the Desktop folder appears
on the desktop.

**Files are saved as you make them.** There is no "save the filesystem" step:
every change is committed to disk as it happens. What that means in practice is
that pulling the power out will not cost you the filesystem — at worst you lose
the very last thing you typed.

Other people's home directories are not readable by you, and yours is not
readable by them. `ls -l` shows who owns what; `chmod` and `chown` change it if
you own the file.

### If the status bar says changes will not be saved

File Explorer says so at the bottom of its window when the machine is running
without persistent storage — because you are on the live image, in recovery
mode, or because something went wrong with the disk. Nothing you do will
survive the reboot. See [Troubleshooting](#troubleshooting).

---

## Getting on a network

If you have a wired connection and a DHCP server — which is what almost every
home router and every hypervisor gives you — it should already work. Check the
network glyph in the taskbar, or:

```sh
ifconfig                      # what address we have, if any
ping 1.1.1.1                  # is anything out there
nslookup example.com          # do names resolve
```

To set an address by hand:

```sh
ifconfig eth0 10.0.2.50 255.255.255.0 10.0.2.2 10.0.2.3     # ip mask gateway dns
ifconfig save                                                # keep it across reboots
ifconfig dhcp                                                # go back to automatic
```

Network settings are saved to disk and reapplied at boot, so this is a
once-per-machine job, not a once-per-boot one.

The clock is set from the network with SNTP, so it stays right even if the
hardware clock drifts.

**Wi-Fi does not work yet.** There is no radio driver. On a laptop, use a USB
Ethernet adapter or a dock.

---

## The terminal

Open **Terminal** from the Start menu, or press **2** at the boot menu for a
machine with no desktop at all.

`help` lists every command by category. `man COMMAND` explains one properly —
what it takes, and which of the several similar commands is the one you want.
`man -k WORD` searches the manual when you cannot remember a name.

The essentials:

```sh
ls  cd  pwd  cat  cp  mv  rm  mkdir  find      # files
edit FILE                                       # a full-screen text editor
write FILE "some text"                          # or just write a line
df  free  ps  uptime  diskinfo                  # what the machine is doing
notify list                                     # what it has been telling you
```

Up and Down walk your history, **Tab** completes commands and paths, and the
arrow keys move within the line. History is kept per account and survives a
clean shutdown. A command typed with a leading space is not recorded, and
anything that takes a password is never recorded.

The shell is a real one: variables, `if`, `for`, `while`, functions, and script
files run with `sh FILE`. [SHELL.md](SHELL.md) is the reference.

---

## Accounts and passwords

```sh
whoami                        # who am I
id  groups                    # and what am I allowed to do
passwd                        # change my password
users                         # list the accounts
sudo useradd sam              # add one (admins only)
sudo userdel sam              # remove one
```

`sudo COMMAND` runs one command as root and then stops being root, so nothing
stays elevated by accident. You are asked for your password the first time and
not again for five minutes. Only members of the admin group can do it.

---

## Keeping files private

```sh
vault create                  # make ~/vault, and pick a password for it
vault status                  # locked or unlocked, and how much is in it
vault lock                    # seal it: the files become one encrypted archive
vault unlock                  # open it again
```

While the vault is **unlocked**, `~/vault` is an ordinary directory — put files
in it, edit them, open them in any app. While it is **locked**, the directory is
empty and everything that was in it is one encrypted file, `~/.vault.bv`. Without
the password there is no way back: the password is not stored anywhere, so if you
forget it the files are gone.

You do not have to remember to lock it. Locking the screen, logging out,
switching accounts and `poweroff`/`reboot` all seal an open vault on the way,
without asking for the password again.

Two things to know, because a vault you misunderstand is worse than none:

* **Unlocked means unlocked.** The files are real files on the disk while the
  vault is open. Lock it before you walk away; pulling the power out of a
  machine with the vault open leaves the files on the disk.
* **The vault covers what is in it.** If you type a secret at the prompt it is
  in your shell history, and if you `cat` it, it is in the system log. Neither
  is inside the vault. Put the file in the vault and open it there.

---

## Shutting down

```sh
poweroff        # commit everything, mark the filesystem clean, then power off
reboot          # the same, then restart
```

Use these rather than holding the power button. They flush the filesystem and
mark it clean, which is what lets the next boot skip the consistency check.

Pulling the power is *safe* — BoltOS is built to survive it — but it costs the
next boot an `fsck`, and it can lose the last thing you did.

---

## Troubleshooting

### It will not boot

Press a key at the boot menu and choose a different entry:

- **Safe mode** — no desktop, no network, no audio, just a text shell. This is
  the way in when a bad setting or a crashing app is what is stopping the boot.
- **Recovery shell** — safe mode, plus the filesystem is mounted read-only so
  nothing you do can make things worse. Use this to read files off a disk you
  are worried about.
- **BoltOS + fsck** — checks and repairs the filesystem before starting.

### It crashed, and I want to know why

```sh
crashlog        # kernel crashes, kept across reboots
dmesg           # the kernel's own log from this boot
notify list -a  # everything the machine has told you
```

If an *app* crashed rather than the machine, the crash is contained — the app
dies, everything else keeps running, and a core dump is written. `core list`
shows them, `core show` opens the newest with a backtrace.

### The disk went read-only

If you see **"Disk write failed. Changes are RAM-only"** or **"Saved filesystem
could not be loaded"**, BoltOS has deliberately stopped writing to the disk to
avoid making a bad situation worse. What is on screen is real; what is on the
disk is whatever was last successfully written.

1. **Do not reboot yet.** Anything you have done since the message is only in
   memory.
2. Copy anything you care about somewhere else — a second disk, or a machine
   over the network with `share`.
3. Run `diskinfo` to see what state the storage is in, and `fsck` to have the
   filesystem checked without writing anything.
4. Then reboot. If the disk is failing, install onto a different one.

[STORAGE_UPGRADE.md](STORAGE_UPGRADE.md) explains exactly what BoltOS does and
does not protect against here.

### The network does not work

```sh
ifconfig            # is there an address at all
netstat             # is anything connected
firewall            # is something being blocked
ping 10.0.2.2       # can we reach the gateway
nslookup bolt.os    # is it names that are broken, rather than the network
```

If `ifconfig` shows no address, DHCP did not answer: check the cable, then set
an address by hand.

### I forgot my password

There is no recovery. Boot the **Recovery shell** entry to read your files off
the disk, then reinstall.

### The screen is the wrong size

Settings → Display. If the desktop is letterboxed with black bars, the
resolution you picked is a different shape from the panel; pick one that
matches.

### Something is using all the memory

```sh
free            # how much is left
ps              # what is running
kill PID        # stop one of them
```

A ring-3 program that will not die can be killed; the kernel reclaims its memory
and hands the screen back.

---

## What BoltOS cannot do yet

Being straight about this is more useful than a feature list:

- **No Wi-Fi.** Wired Ethernet only.
- **No suspend or resume**, and no battery or brightness control. On a laptop it
  runs at full brightness until the battery is flat.
- **No USB mass storage.** USB keyboards and mice work; USB drives do not appear
  in File Explorer.
- **No printing.**
- **IPv6 works, but nothing hands out a resolver over it.** The machine
  addresses itself, finds its router, and opens TCP connections over v6 --
  `download http://[fe80::1]:8080/x` and `ssh user@fe80::1` both work. What it
  cannot yet do is find a *name server* over v6: that address still comes from
  DHCP, so on a network with no IPv4 at all, addresses you type work and names
  you type do not.
- **Sound plays; nothing records.** Intel HD Audio and AC'97 are both driven,
  and the Media player plays WAV files. There is no microphone input, no volume
  mixer beyond the one master level, and no MP3 or any other compressed format
  -- a `.wav` is the only kind of sound file the machine can open.
- **No disk encryption.** Anyone who can take the disk out can read it.
- **Nothing is upgradable in place.** A newer BoltOS means reinstalling.

If any of those matters to you, BoltOS is not the machine's only operating
system yet.
