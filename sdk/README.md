# The BoltOS SDK on Windows

`bolt` makes a BoltOS program on a Windows machine and puts it on a BoltOS one.

```
sdk\bolt new hello          a project that builds
cd hello
..\sdk\bolt build           an ELF the kernel will load
..\sdk\bolt ship            serve it, and say what to type in the guest
```

[SDK.md](../SDK.md) is the specification — the memory layout, the syscall
convention, every call, what is stable. This is the tooling that saves you
retyping it.

`sdk/build.sh` is still here and still works; it is the same compiler and the
same flags, for people driving the build from msys2 bash or from a Makefile.
`bolt` is what you want if the machine you are sitting at runs Windows: it
needs no bash, it makes the project as well as building it, and it knows how
to get the result onto the other machine.

---

## Getting it

Put `sdk\` on `PATH` and `bolt` works from anywhere. Otherwise call it by
path — `C:\path\to\boltOS\sdk\bolt build` — from wherever your project lives.
It builds against the checkout it sits in, so a program built by that copy of
`bolt` matches the kernel beside it.

You need clang, ld.lld and nasm. msys2 is where they come from here:

```
pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld nasm
```

`bolt doctor` says what it found and what it could not.

```
> sdk\bolt doctor
[*] BoltOS SDK, checking out C:\...\boltOS

  clang    C:\msys64\ucrt64\bin\clang.exe
  ld.lld   C:\msys64\ucrt64\bin\ld.lld.exe
  nasm     C:\msys64\usr\bin\nasm.exe

  include\syscall.h    ok
  user\ulibc.h         ok
  user\user.ld         ok
  user\crt0.asm        ok

  builds against BoltOS ABI 1
[+] ready
```

Set `BOLTOS_MSYS` if msys2 is not on `C:\msys64`, or `CLANG` / `LLD` / `NASM`
to name one tool at a time.

---

## The commands

### `bolt new NAME [-kind console|graphical] [-dir PATH]`

Writes `NAME\` with a manifest, a program and a `.gitignore`.

`console` writes to the terminal — arguments, files, an exit status the shell
can see. `graphical` claims the whole framebuffer, which is what a BoltOS
program that wants pixels gets: there is no window and no toolkit, and the
compositor is paused until you hand the screen back. Both are short enough to
read in one sitting and are meant to be deleted down to what you wanted.

### `bolt build [DIR]`

Compiles and links. Out comes `DIR\build\NAME` — a static ELF64, loaded at
`0x400000`, entered at `_start`, which is what the kernel's loader will accept
and nothing else is.

The flags are the ones `build.sh` uses for the OS's own ring-3 programs, down
to `-fno-omit-frame-pointer` (that one is what lets `core show` walk a
backtrace out of your program's crash; keep it). The C runtime and library are
compiled once into `build\sdk\` in the checkout and reused, so the second build
is only your own files.

### `bolt pack [DIR]`

Builds, then wraps the result in a `.pkg`: the format in `include/pkg.h`, which
`pkg install` reads. Nothing about it is Windows-specific — the same file
installs from a USB stick, from a directory, or from a repository.

### `bolt ship [DIR] [-port N] [-bind ADDR] [-stage]`

Builds, packs, writes a one-package repository, and serves it over HTTP, then
prints the commands to type on the other machine:

```
[+] serving C:\...\hello\build\ship on 0.0.0.0:8420

  In BoltOS (QEMU, user-mode networking) -- as a package:
      pkg repo http://10.0.2.2:8420
      pkg update
      pkg install hello
      chmod 755 /bin/hello
      run hello

  Or straight in, no package manager:
      download http://10.0.2.2:8420/hello /bin/hello
      chmod 755 /bin/hello
      run hello
```

`10.0.2.2` is this machine as QEMU's user-mode networking presents it. For a
BoltOS on real hardware, use this machine's address on that network — `ship`
prints those too.

It binds every interface so a machine that is not this one can reach it, which
means Windows Firewall will ask the first time. `-bind 127.0.0.1` keeps it to
loopback (still enough for QEMU, and no prompt). `-stage` writes the repository
and stops, for putting on a stick or serving with something else.

### `bolt clean [DIR]` and `bolt doctor`

The first removes `DIR\build`. The second is above.

---

## The manifest

`pkg.txt` is BoltOS's own manifest — the same file `pkg build` reads on the
machine itself:

```
package hello
version 1.0
kind    program
summary a BoltOS program

sources main.c

install build/hello /bin/hello
run     /bin/hello
```

`sources` is the one key the SDK added. The kernel's parser ignores keys it
does not recognise, on purpose ("a newer repository must not be fatal"), so a
project written here stays packageable on BoltOS as well as from Windows.

`install SRC DST` can appear more than once, and carries anything — a
configuration file, a wallpaper, a second binary. `run` is what `pkg run NAME`
starts.

---

## Two sizes worth knowing

`download` allocates a fixed 256 KiB and refuses anything bigger; `pkg` will
not accept a package over `PKG_MAX`, which is 512 KiB. `bolt` warns when you
cross either, because both are miserable to diagnose from the far end of a
network. A program built from the templates is about 45 KiB, most of which is
the C library and the fonts.

## When it does not work

**The guest says `connection failed / timed out`.** Try it again before
anything else — the first outbound connection after signing in sometimes does
not land, and the second one does. After that, check the guest has an address
(`ifconfig`, then `dhcp` if it does not), and that the firewall let `bolt ship`
listen.

**`run` says `permission denied`.** Packages install with the filesystem's
default mode. `chmod 755 /bin/yourapp`, which is why `ship` prints that line.

**`pkg install` says the package does not match its index entry.** The `.pkg`
changed after the index was written. Re-run `bolt ship`, which rewrites both.
