# The BoltOS SDK

Everything a program needs to run on BoltOS in ring 3, written down, so that
building one does not require reading the kernel.

BoltOS already ran ring-3 programs — the JavaScript interpreter, the web
browser, `netget` — but each of them was started by a command the kernel was
built with. There was no way to run a program the kernel had never heard of,
and no document saying what a program could rely on. Both of those are the same
problem: an operating system whose userland is a fixed list is not one you can
write software for.

- The ABI is `include/syscall.h`. It is the *only* copy of the syscall numbers;
  the userland C library includes that same header rather than repeating it.
- The C library is `user/ulibc.h` plus `user/stdio.h`, `user/stdlib.h`,
  `user/math.h`, `user/ctype.h` and `include/string.h`.
- `sdk/build.sh` links a program the way the kernel expects one.
- `run PATH [args]` starts it and reports what it did.

---

## 1. What a BoltOS program is

A **static ELF64 executable for x86-64**, not position-independent, with its
segments loaded exactly where they say and control entering at `_start`.

| | |
| --- | --- |
| Format | ELF64, `ET_EXEC`, `EM_X86_64`, static |
| Load address | `0x400000` (see `user/user.ld`) |
| Entry | `_start` (`user/crt0.asm`), which calls `main(argc, argv)` |
| Stack | 256 KiB, top at `0x0000700000000000`, 16-byte aligned entry RSP; `argc`, `argv[]`, `NULL`, `envp NULL`, `AT_NULL` |
| Heap | `brk` starts after the last segment; `mmap` allocations start at `0x0000400000000000` |
| Floating point | SSE is on and the FPU state is saved across context switches — `double` works |
| Frame pointers | kept (`-fno-omit-frame-pointer`), so `core show` can walk a crash |

There is no dynamic loader, no shared library, no `.interp`. A program is one
file, and it is the whole program.

The loader validates ELF header sizes, file bounds, and the entry point before
mapping segments. Loadable segments must fit between address `0x1000` and the
mmap region at `0x0000400000000000`; the entry point must be in a segment marked
executable. The initial argument vector supports 16 entries including argv[0],
with strings and the vector fitting together in the top 4 KiB stack page.

## 2. The syscall convention

```
rax = syscall number
rdi, rsi, rdx, r10, r8 = arguments 1..5
syscall
rax = result   (-1 on failure)
```

`r10` rather than `rcx` for the fourth argument, because the `syscall`
instruction clobbers `rcx` (and `r11`). This is the System V convention Linux
uses, so a compiler's inline-asm idioms carry over unchanged.

Most errors return `-1`; `SYS_BRK` returns the previous break on allocation
failure. There is no `errno`. `SYS_HTTPGET` also supplies an HTTP status or a
network failure code through its status pointer. Unsupported syscall numbers
return `-1`. `rcx`, `r11`, condition flags and memory must be treated as clobbered
by inline syscall assembly.

## 3. The syscalls

### Files and processes

| # | Name | Arguments | Returns |
| --- | --- | --- | --- |
| 0 | `SYS_READ` | fd, buf, len | bytes read, 0 at EOF, −1 |
| 1 | `SYS_WRITE` | fd, buf, len | bytes written, −1 |
| 2 | `SYS_OPEN` | path, flags, mode | fd, −1 |
| 3 | `SYS_CLOSE` | fd | 0, −1 |
| 5 | `SYS_FSTAT` | fd, `stat_t*` | 0, −1 |
| 8 | `SYS_LSEEK` | fd, offset, whence | new offset, −1 |
| 9 | `SYS_MMAP` | addr, len, prot, flags, fd | address, −1 |
| 12 | `SYS_BRK` | addr (0 to query) | the new break |
| 24 | `SYS_YIELD` | — | 0 |
| 39 | `SYS_GETPID` | — | pid |
| 60 | `SYS_EXIT` | code | does not return |
| 106 | `SYS_ABI` | — | the ABI version (see §7) |

fds 0, 1 and 2 are the console. `O_RDONLY`/`O_WRONLY`/`O_RDWR`/`O_CREAT`/
`O_TRUNC`/`O_APPEND` and `SEEK_SET`/`SEEK_CUR`/`SEEK_END` have their usual
values; `stat_t` is `{ uint64_t size; uint64_t is_dir; }`.

This is a small BoltOS ABI, not Linux/POSIX binary compatibility. `SYS_OPEN`
currently ignores `mode` and the C wrapper takes only path and flags.
`SYS_MMAP` allocates zeroed anonymous pages: `addr`, `flags`, and `fd` are ignored;
only length and the write-protection bit affect the mapping. There is no munmap
or file-backed mmap. `SYS_YIELD` returns to the preemptive scheduler's normal
timer-driven execution; it does not force an immediate context switch.

### Sockets

A socket is an ordinary fd: `read`, `write` and `close` work on one. IPv4
addresses are passed in **host** byte order, as everywhere else in the stack.
IPv6 addresses are sixteen bytes in wire order, passed by pointer, because an
address that size does not fit in a register argument.

| # | Name | Arguments | Returns |
| --- | --- | --- | --- |
| 41 | `SYS_SOCKET` | domain (`AF_INET` / `AF_INET6`), type (`SOCK_STREAM`/`SOCK_DGRAM`) | fd, −1 |
| 42 | `SYS_CONNECT` | fd, ip, port, timeout_ms | 0, −1 |
| 107 | `SYS_CONNECT6` | fd, `const uint8_t addr[16]`, port, timeout_ms | 0, −1 |
| 50 | `SYS_LISTEN` | fd, port | 0, −1 |
| 43 | `SYS_ACCEPT` | fd, `uint32_t *ip`, `uint16_t *port`, timeout_ms | fd, −1 |
| 108 | `SYS_ACCEPT6` | fd, `uint8_t peer[16]`, `uint16_t *port`, timeout_ms | fd, −1 |
| 105 | `SYS_RESOLVE` | name, `uint32_t *ip` | 0, −1 (`/etc/hosts`, then an A lookup) |
| 109 | `SYS_RESOLVE6` | name, `uint8_t out[16]` | 0, −1 (an AAAA lookup) |
| 102 | `SYS_HTTPGET` | url, buf, cap, `int *status` | body length, −1 |

The v6 calls are separate numbers rather than changes to 42, 43 and 105: a
binary built against ABI 1 cannot be broken by a syscall it never makes, and
folding sixteen bytes into those calls' arguments would have broken all of
them. The family is not recorded on the socket -- `AF_INET6` and `AF_INET`
both give you a stream socket, and which `connect` you call decides where it
goes. A **listening** socket takes connections of either family without being
told, so there is no `listen6`; `SYS_ACCEPT` reports a v6 peer's address as 0
(it has no `uint32_t` form) and `SYS_ACCEPT6` reports a v4 peer's as `::` --
neither invents a v4-mapped address, because nothing else in this kernel uses
one. Call the one that matches the connections you expect.

### The screen and the keyboard

A program can take the whole framebuffer — that is how `/bin/browser` runs in
ring 3 while the compositor waits. The drivers stay in ring 0; a program reaches
them only through these.

| # | Name | Arguments | Returns |
| --- | --- | --- | --- |
| 100 | `SYS_FBINFO` | `struct user_fbinfo*` | 0, −1 — claims the panel and fills in `w`, `h`, `pitch_px` |
| 104 | `SYS_FBPRESENT` | packed `w*h` xRGB buffer | 0, −1 — blit one frame |
| 103 | `SYS_FBEND` | — | 0 — give the screen back |
| 101 | `SYS_GETKEY` | — | a key, or −1 if none (non-blocking) |

The compositor is paused between `SYS_FBINFO` and `SYS_FBEND`; process exit
releases the panel too, so a program that crashes does not take the desktop
with it.

## 4. The C library

`user/ulibc.h` wraps every syscall above (`open`, `read`, `write`, `lseek`,
`fstat`, `mmap`, `getpid`, `yield`, `exit`, `bolt_abi`, `socket`, `connect_ip`,
`connect_ip6`, `listen_port`, `accept_conn`, `accept_conn6`, `resolve_host`,
`resolve_host6`, `fb_map`, `fb_present`, `fb_release`, `getkey`,
`http_get_u`). On top of that:

- `user/stdio.h` — `printf`, `snprintf`, `puts`, `putchar`, `fopen`/`fread`/
  `fwrite`/`fclose`. `fprintf` takes a file descriptor, not a `FILE *`.
- `user/stdlib.h` — `malloc`, `free`, `realloc`, `calloc`, `atoi`, `strtol`,
  `qsort`, `abs`, `rand`
- `include/string.h` — the usual `str*` and `mem*`
- `user/math.h` — `sqrt`, `pow`, `sin`, `cos`, `log`, … (real SSE floating point)
- `user/ctype.h` — `isalpha` and friends

`libctest` tests the kernel's freestanding library. `test/t_sdk.py` exercises
the separate ring-3 runtime, including file I/O, arguments, and floating point.

## 5. Building one

```sh
sdk/build.sh -o myapp myapp.c [more.c ...]
```

It uses the same compiler, linker script and flags the OS builds its own
userland with. Set `BOLTOS_MSYS` (or `CLANG`/`LLD`/`NASM`) if your toolchain is
somewhere unusual. What it does, if you would rather do it yourself:

```sh
clang --target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone \
      -msse -msse2 -mno-mmx -mno-80387 -fno-omit-frame-pointer \
      -O2 -std=c11 -Iuser -Iinclude -c myapp.c -o myapp.o
ld.lld -m elf_x86_64 -T user/user.ld -no-pie -o myapp \
       build/u_crt0.o myapp.o build/u_ulibc.o build/u_stdlib.o \
       build/u_libm.o build/u_string.o build/u_string_ext.o
```

Keep `-fno-omit-frame-pointer`: it is what makes `core show` able to walk a
backtrace out of your program's crash, and it costs one register.

## 6. Getting it onto a machine, and running it

Three ways, none of which need a host toolchain on the BoltOS side:

```
download http://host/myapp /bin/myapp      # over the network
chmod 755 /bin/myapp                      # permit execution for a signed-in user
run myapp                                  # ELF checked, argv passed, status reported
```

`run` looks in `/bin` for a name with no `/` in it, passes the rest of the
command line through as `argv[1..]`, waits up to 60 seconds, and prints the exit status.
After a timeout the process continues running; use `ps` and `kill` to manage it.
`$?` receives the exit code; a CPU fault returns `128 + vector`, and `kill`
returns status 137 to a waiting `run`. If the
program faults instead, it is killed on its own and a core dump is written —
`core` and `core show` read it back with the frames named out of your binary's
symbol table.

To package it, put it in a directory with a `pkg.txt`:

```
package myapp
version 1.0
kind    program
summary what it does
run     /bin/myapp
install myapp /bin/myapp
```

then `pkg build DIR`, `pkg publish DIR/myapp-1.0.pkg /somewhere/repo`.
Install it with `pkg install myapp` after selecting that repository. Package
files currently use the filesystem's default mode; use `chmod 755 /bin/myapp`
before launching through `run` as an ordinary user.

The SDK build runs on the host using Clang, NASM and LLD. Paths are relative
to the caller's working directory, so an external project can call
`/path/to/boltOS/sdk/build.sh -o myapp main.c util.c`. Native BoltCC code
generation, linking and self-hosting inside BoltOS remain roadmap items.

## 7. What is stable

`BOLT_ABI_VERSION` (currently **1**) is bumped when a syscall's **number,
arguments or meaning** changes. It is *not* bumped when one is added: a program
that does not call a new syscall cannot be broken by it.

```c
if (bolt_abi() != BOLT_ABI_VERSION) {
    printf("built for BoltOS ABI %d, running on %d\n",
           BOLT_ABI_VERSION, bolt_abi());
    return 1;
}
```

Everything in §2 and §3 is covered by that promise. The C library above it is
ordinary source you compile into your program, so it changes when you rebuild
and never underneath you.

`sdk/hello.c` is a complete program that uses argv, the filesystem, the ABI
check and an exit status; `test/t_sdk.py` builds it with the host toolchain,
serves it over HTTP, downloads it into a running BoltOS and runs it there —
which is the whole path this document describes, exercised end to end.
