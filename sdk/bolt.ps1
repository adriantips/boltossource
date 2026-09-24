# ===========================================================================
#  bolt.ps1 -- the BoltOS SDK for Windows.
#
#  sdk/build.sh compiles a program, and that is all it does. Everything after
#  the compiler -- making a project in the first place, packaging it, getting
#  it onto a machine that is not this one -- was left as an exercise, written
#  down in SDK.md as a list of commands to retype. This is that list, run for
#  you, from a Windows shell, with no msys2 bash in the way.
#
#      bolt new hello              a project that builds
#      bolt build                  an ELF the kernel will load
#      bolt pack                   a .pkg BoltOS can install
#      bolt ship                   serve it, and say what to type in the guest
#
#  The ABI it builds against is include/syscall.h, same as the kernel. SDK.md
#  is the specification; this is the tooling.
#
#  Invoked through bolt.cmd, which is what supplies -ExecutionPolicy Bypass.
# ===========================================================================
# No param() block on purpose. PowerShell's binder sees `-port 9000` and goes
# looking for a -port parameter to put it in, and an advanced function errors
# out rather than handing it over; $args gets the command line as typed.
$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$Command = "help"
$Rest = @()
if ($args.Count -gt 0) {
    $Command = [string]$args[0]
    if ($args.Count -gt 1) { $Rest = @($args[1..($args.Count - 1)] | ForEach-Object { [string]$_ }) }
}

$SdkDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root   = Split-Path -Parent $SdkDir

# ------------------------------------------------------------------- output --
# Everything user-facing goes through these so the transcript reads the same
# whether it came from a build error or a packaging one.
function Say  ([string] $m) { Write-Host "  $m" }
function Note ([string] $m) { Write-Host "[*] $m" -ForegroundColor Cyan }
function Good ([string] $m) { Write-Host "[+] $m" -ForegroundColor Green }
function Warn ([string] $m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Die  ([string] $m) { Write-Host "[x] $m" -ForegroundColor Red; exit 1 }

# --------------------------------------------------------------- toolchain ---
# The same three programs build.sh uses. BOLTOS_MSYS wins, then the place msys2
# installs itself; CLANG/LLD/NASM override one at a time for anyone whose
# toolchain came from somewhere else entirely.
function Resolve-Tool([string] $envName, [string] $guess, [string] $onPath) {
    $fromEnv = [Environment]::GetEnvironmentVariable($envName)
    if ($fromEnv) { return $fromEnv }
    if (Test-Path -LiteralPath $guess) { return $guess }
    $cmd = Get-Command $onPath -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Get-Toolchain {
    $msys = [Environment]::GetEnvironmentVariable("BOLTOS_MSYS")
    if (-not $msys) { $msys = "C:\msys64" }

    $tc = @{
        Clang = Resolve-Tool "CLANG" (Join-Path $msys "ucrt64\bin\clang.exe")  "clang"
        Lld   = Resolve-Tool "LLD"   (Join-Path $msys "ucrt64\bin\ld.lld.exe") "ld.lld"
        Nasm  = Resolve-Tool "NASM"  (Join-Path $msys "usr\bin\nasm.exe")      "nasm"
        Msys  = $msys
    }
    return $tc
}

function Require-Toolchain {
    $tc = Get-Toolchain
    $missing = @()
    if (-not $tc.Clang) { $missing += "clang" }
    if (-not $tc.Lld)   { $missing += "ld.lld" }
    if (-not $tc.Nasm)  { $missing += "nasm" }
    if ($missing.Count -gt 0) {
        Write-Host "[x] missing from the toolchain: $($missing -join ', ')" -ForegroundColor Red
        Say "BoltOS builds with clang, ld.lld and nasm. The usual source is msys2:"
        Say "    pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld nasm"
        Say "Looked under $($tc.Msys). Set BOLTOS_MSYS, or CLANG / LLD / NASM, to"
        Say "point somewhere else. 'bolt doctor' prints what it found."
        exit 1
    }
    return $tc
}

# Run a tool and stop on failure with its own output, not a PowerShell trace.
# 2>&1 on a native exe is a trap in 5.1 (every stderr line comes back as an
# ErrorRecord and $? goes false on a clean exit), so stderr stays where it is
# and we judge by the exit code alone.
function Invoke-Tool([string] $exe, [string[]] $toolArgs, [string] $what) {
    & $exe @toolArgs
    if ($LASTEXITCODE -ne 0) { Die "$what failed (exit $LASTEXITCODE)" }
}

# ---------------------------------------------------------------- manifest ---
# pkg.txt, which is BoltOS's own manifest -- the same file `pkg build` reads on
# the machine itself. The SDK adds one key, `sources`, and the kernel's parser
# ignores keys it does not know ("a newer repository must not be fatal"), so a
# project stays packageable on BoltOS as well as from here.
function Read-Manifest([string] $dir) {
    $path = Join-Path $dir "pkg.txt"
    if (-not (Test-Path -LiteralPath $path)) {
        Die "no pkg.txt in $dir -- is that a BoltOS project? ('bolt new NAME' makes one)"
    }

    $m = @{
        name = ""; version = "1.0"; kind = "program"; summary = ""
        requires = ""; run = ""; sources = @(); install = @()
    }

    foreach ($raw in (Get-Content -LiteralPath $path)) {
        $line = $raw.Trim()
        if ($line.Length -eq 0)     { continue }
        if ($line.StartsWith("#"))  { continue }

        $sp = $line.IndexOf(" ")
        if ($sp -lt 0) { $sp = $line.IndexOf("`t") }
        if ($sp -lt 0) { continue }
        $key = $line.Substring(0, $sp)
        $val = $line.Substring($sp + 1).Trim()

        switch ($key) {
            "package"  { $m.name     = $val }
            "version"  { $m.version  = $val }
            "kind"     { $m.kind     = $val }
            "summary"  { $m.summary  = $val }
            "requires" { $m.requires = $val }
            "run"      { $m.run      = $val }
            "sources"  { $m.sources += ($val -split '\s+') }
            "install"  { $m.install += ,$val }
            default    { }
        }
    }

    if (-not $m.name) { Die "pkg.txt has no 'package NAME' line" }
    return $m
}

# A project directory: what was passed, else wherever we were invoked. The
# options that take a value have to be stepped over, or `bolt ship -port 9000`
# goes looking for a directory called 9000.
$VALUE_OPTS = @("port", "bind", "dir", "kind")

function Get-Positional([string[]] $argv) {
    $skip = $false
    foreach ($a in $argv) {
        if ($skip) { $skip = $false; continue }
        if ($a.StartsWith("-")) {
            if ($VALUE_OPTS -contains $a.TrimStart('-').ToLower()) { $skip = $true }
            continue
        }
        return $a
    }
    return $null
}

function Resolve-Project([string[]] $argv) {
    $dir = Get-Positional $argv
    if (-not $dir) { $dir = (Get-Location).Path }
    if (-not (Test-Path -LiteralPath $dir)) { Die "no such directory: $dir" }
    return (Resolve-Path -LiteralPath $dir).Path
}

# Pull -Name VALUE out of the trailing arguments. Undersized compared to a real
# parameter binder, but the alternative is a param() block per subcommand.
function Get-Opt([string[]] $argv, [string] $name, [string] $fallback) {
    for ($i = 0; $i -lt $argv.Count; $i++) {
        if ($argv[$i].TrimStart('-') -ieq $name) {
            if ($i + 1 -lt $argv.Count) { return $argv[$i + 1] }
            Die "-$name needs a value"
        }
    }
    return $fallback
}

function Has-Flag([string[]] $argv, [string] $name) {
    foreach ($a in $argv) { if ($a.TrimStart('-') -ieq $name) { return $true } }
    return $false
}

# ------------------------------------------------------------------- build ---
# Verbatim from build.sh's userland flags. -fno-omit-frame-pointer is not
# decoration: it is what lets `core show` walk a backtrace out of a crash in
# your program, and it costs one register.
$UCFLAGS = @(
    "--target=x86_64-elf", "-ffreestanding", "-fno-stack-protector",
    "-fno-pic", "-fno-pie", "-mno-red-zone",
    "-msse", "-msse2", "-mno-mmx", "-mno-80387",
    "-fno-omit-frame-pointer",
    "-Wall", "-Wextra", "-O2", "-std=c11"
)

# crt0 and the C library the OS links its own programs against. Cached in the
# checkout's build/sdk (not %TEMP%: this toolchain is a Windows build and a
# msys-style temp path is not one it can open) and rebuilt only when a source
# moved under it.
$RUNTIME_SRC = @(
    "user\ulibc.c", "user\stdlib_ext.c", "user\libm.c",
    "libc\string.c", "libc\string_ext.c",
    # The bitmap fonts, linked into everything rather than made an option. A
    # graphical program needs text and there is no other way to get a glyph in
    # ring 3; 3 KiB of rodata in a console program that never asks for one is
    # cheaper than a build flag people have to learn about.
    "kernel\font8x8.c"
)

function Test-Stale([string] $obj, [string[]] $deps) {
    if (-not (Test-Path -LiteralPath $obj)) { return $true }
    $objTime = (Get-Item -LiteralPath $obj).LastWriteTimeUtc
    foreach ($d in $deps) {
        if (-not (Test-Path -LiteralPath $d)) { continue }
        if ((Get-Item -LiteralPath $d).LastWriteTimeUtc -gt $objTime) { return $true }
    }
    return $false
}

function Build-Runtime($tc) {
    $work = [Environment]::GetEnvironmentVariable("BOLTOS_SDK_WORK")
    if (-not $work) { $work = Join-Path $Root "build\sdk" }
    New-Item -ItemType Directory -Force -Path $work | Out-Null

    $crt0src = Join-Path $Root "user\crt0.asm"
    $crt0obj = Join-Path $work "crt0.o"
    if (Test-Stale $crt0obj @($crt0src)) {
        Invoke-Tool $tc.Nasm @("-f", "elf64", $crt0src, "-o", $crt0obj) "nasm on crt0.asm"
    }

    # Every runtime object depends on the published headers, so a syscall number
    # that changes invalidates the cache the way it should.
    $headers = @()
    foreach ($h in @("include\syscall.h", "user\ulibc.h", "include\string.h")) {
        $headers += (Join-Path $Root $h)
    }

    $objs = @($crt0obj)
    foreach ($rel in $RUNTIME_SRC) {
        $src = Join-Path $Root $rel
        $obj = Join-Path $work ([IO.Path]::GetFileNameWithoutExtension($rel) + ".o")
        if (Test-Stale $obj (@($src) + $headers)) {
            $a = $UCFLAGS + @("-I", (Join-Path $Root "user"), "-I", (Join-Path $Root "include"),
                              "-c", $src, "-o", $obj)
            Invoke-Tool $tc.Clang $a "compiling $rel"
        }
        $objs += $obj
    }
    return $objs
}

function Invoke-Build([string] $dir, [bool] $quiet) {
    $tc = Require-Toolchain
    $m  = Read-Manifest $dir

    if ($m.sources.Count -eq 0) {
        Die "pkg.txt lists no 'sources' line -- nothing to compile"
    }

    $out = Join-Path $dir "build"
    $objDir = Join-Path $out "obj"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null

    if (-not $quiet) { Note "building $($m.name) $($m.version)" }

    $runtime = Build-Runtime $tc

    # Header tracking, at the resolution of "any header in the project". A real
    # dependency scan means -MD and a makefile reader; touching one header and
    # rebuilding four files costs less than that does.
    $projHeaders = @(Get-ChildItem -LiteralPath $dir -Filter *.h -Recurse -File -ErrorAction SilentlyContinue |
                     Select-Object -ExpandProperty FullName)

    # Two sources with the same basename in different directories must not land
    # on the same object. The relative path, flattened, is unique by definition.
    $appObjs = @()
    foreach ($rel in $m.sources) {
        $src = Join-Path $dir ($rel -replace '/', '\')
        if (-not (Test-Path -LiteralPath $src)) { Die "source not found: $rel" }
        $flat = ($rel -replace '[\\/]', '_') -replace '\.c$', ''
        $obj  = Join-Path $objDir "$flat.o"
        if (Test-Stale $obj (@($src) + $projHeaders)) {
            if (-not $quiet) { Say "cc  $rel" }
            $a = $UCFLAGS + @("-I", $dir,
                              "-I", (Join-Path $Root "user"),
                              "-I", (Join-Path $Root "include"),
                              "-c", $src, "-o", $obj)
            Invoke-Tool $tc.Clang $a "compiling $rel"
        }
        $appObjs += $obj
    }

    # The app's objects go before the library's so a program can override a
    # library symbol, which is the order build.sh links in too.
    $bin = Join-Path $out $m.name
    $crt0 = $runtime[0]
    $lib  = $runtime[1..($runtime.Count - 1)]
    $largs = @("-m", "elf_x86_64", "-T", (Join-Path $Root "user\user.ld"), "-no-pie",
               "-o", $bin, $crt0) + $appObjs + $lib
    if (-not $quiet) { Say "ld  $($m.name)" }
    Invoke-Tool $tc.Lld $largs "linking $($m.name)"

    $size = (Get-Item -LiteralPath $bin).Length
    if (-not $quiet) { Good "$bin ($size bytes)" }

    # `download` allocates a fixed 256 KiB and refuses anything larger, which is
    # a confusing failure to hit at the far end of a network.
    if ($size -gt (256 * 1024)) {
        Warn "$size bytes is over the 256 KiB 'download' accepts -- ship it as a package instead"
    }
    return @{ Manifest = $m; Binary = $bin; Size = $size }
}

# ------------------------------------------------------------------ packing --
# FNV-1a over the package bytes, 16 hex digits, matching kernel/pkg.c exactly.
# It is not a signature and pkg.c does not present it as one; it catches a
# truncated download or a repository that moved under you. PowerShell promotes
# an overflowing [uint64] to double and loses the low bits, so the arithmetic
# has to happen somewhere that wraps.
Add-Type -TypeDefinition @'
public static class BoltFnv {
    public static string Hex(byte[] data) {
        ulong h = 0xcbf29ce484222325UL;
        for (int i = 0; i < data.Length; i++) { h ^= data[i]; h *= 1099511628211UL; }
        char[] o = new char[16];
        for (int i = 0; i < 16; i++) o[i] = "0123456789abcdef"[(int)((h >> ((15 - i) * 4)) & 0xF)];
        return new string(o);
    }
}
'@ -ErrorAction SilentlyContinue

# A .pkg is a text header, a blank line, then `file SIZE PATH` + that many raw
# bytes + a newline, repeated. Sizes are explicit and the payload is copied
# verbatim, which is the whole reason an ELF can travel in one. See include/pkg.h.
function New-Package([hashtable] $m, [string] $dir) {
    $mem = New-Object System.IO.MemoryStream
    $enc = New-Object System.Text.UTF8Encoding($false)

    function Put([string] $s) {
        $b = $enc.GetBytes($s)
        $mem.Write($b, 0, $b.Length)
    }

    Put "BOLTPKG 1`n"
    Put "package $($m.name)`n"
    Put "version $($m.version)`n"
    Put "kind $($m.kind)`n"
    Put "summary $($m.summary)`n"
    if ($m.requires) { Put "requires $($m.requires)`n" }
    if ($m.run)      { Put "run $($m.run)`n" }
    Put "`n"

    $count = 0
    foreach ($spec in $m.install) {
        $parts = $spec -split '\s+', 2
        if ($parts.Count -lt 2) { Warn "ignoring malformed install line: $spec"; continue }
        $src = $parts[0]; $dst = $parts[1].Trim()

        $full = $src
        if (-not [IO.Path]::IsPathRooted($src)) {
            $full = Join-Path $dir ($src -replace '/', '\')
        }
        if (-not (Test-Path -LiteralPath $full)) {
            Die "install line names $src, which is not there (did you 'bolt build' first?)"
        }

        $bytes = [IO.File]::ReadAllBytes($full)
        Put ("file {0} {1}`n" -f $bytes.Length, $dst)
        $mem.Write($bytes, 0, $bytes.Length)
        Put "`n"
        $count++
    }
    if ($count -eq 0) { Die "pkg.txt installs nothing -- add an 'install SRC /dest' line" }

    return $mem.ToArray()
}

function Invoke-Pack([string] $dir, [bool] $quiet) {
    $built = Invoke-Build $dir $quiet
    $m = $built.Manifest

    $out = Join-Path $dir "build"
    $img = New-Package $m $dir
    $pkgPath = Join-Path $out "$($m.name)-$($m.version).pkg"
    [IO.File]::WriteAllBytes($pkgPath, $img)

    if (-not $quiet) { Good "$pkgPath ($($img.Length) bytes)" }
    if ($img.Length -gt (512 * 1024)) {
        Warn "$($img.Length) bytes is over PKG_MAX -- BoltOS will refuse this package"
    }
    return @{ Manifest = $m; Binary = $built.Binary; Package = $pkgPath; Image = $img }
}

# ------------------------------------------------------------------- ship ----
# A repository is a directory of .pkg files plus an `index`: the same header
# stanzas, one per package, each naming its file and that file's hash. Serve
# that over HTTP and `pkg repo URL` on the guest reaches it -- the network and
# on-disk cases are the same code past fetch(), so there is nothing special
# about a repository that happens to live on a Windows box.
function Write-Repo([hashtable] $packed, [string] $dir) {
    $ship = Join-Path $dir "build\ship"
    New-Item -ItemType Directory -Force -Path $ship | Out-Null

    $m = $packed.Manifest
    $pkgName = "$($m.name)-$($m.version).pkg"
    Copy-Item -LiteralPath $packed.Package -Destination (Join-Path $ship $pkgName) -Force
    # The raw ELF too, so `download` works without going through the package
    # manager at all. It is the shorter path when you are iterating.
    Copy-Item -LiteralPath $packed.Binary -Destination (Join-Path $ship $m.name) -Force

    $sha = [BoltFnv]::Hex($packed.Image)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append("package $($m.name)`n")
    [void]$sb.Append("version $($m.version)`n")
    [void]$sb.Append("kind $($m.kind)`n")
    [void]$sb.Append("summary $($m.summary)`n")
    if ($m.requires) { [void]$sb.Append("requires $($m.requires)`n") }
    if ($m.run)      { [void]$sb.Append("run $($m.run)`n") }
    [void]$sb.Append("file $pkgName`n")
    [void]$sb.Append("sha $sha`n`n")

    $enc = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllBytes((Join-Path $ship "index"), $enc.GetBytes($sb.ToString()))
    return $ship
}

# A one-connection-at-a-time HTTP/1.1 server over a raw socket. HttpListener
# would be less code and wants a URL reservation for anything but localhost,
# which means an elevated prompt the first time somebody tries to install onto
# real hardware. A TcpListener needs no such thing.
function Serve-Directory([string] $root, [string] $bind, [int] $port, [hashtable] $m) {
    $addr = [System.Net.IPAddress]::Parse($bind)
    $listener = New-Object System.Net.Sockets.TcpListener($addr, $port)
    $listener.Start()
    $actual = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port

    # 10.0.2.2 is the host as QEMU's user-mode networking presents it; on real
    # hardware it is whatever address this machine has on the same network.
    $guestHost = "10.0.2.2"
    $url = "http://${guestHost}:$actual"

    Write-Host ""
    Good "serving $root on $($bind):$actual"
    Write-Host ""
    Write-Host "  In BoltOS (QEMU, user-mode networking) -- as a package:" -ForegroundColor White
    Say "    pkg repo $url"
    Say "    pkg update"
    Say "    pkg install $($m.name)"
    if ($m.run) { Say "    chmod 755 $($m.run)" }
    if ($m.run) { Say "    run $([IO.Path]::GetFileName($m.run))" }
    Write-Host ""
    Write-Host "  Or straight in, no package manager:" -ForegroundColor White
    Say "    download $url/$($m.name) /bin/$($m.name)"
    Say "    chmod 755 /bin/$($m.name)"
    Say "    run $($m.name)"
    Write-Host ""
    if ($bind -ne "127.0.0.1") {
        $ips = @()
        try {
            $ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
                     Where-Object { $_.IPAddress -ne "127.0.0.1" } |
                     Select-Object -ExpandProperty IPAddress)
        } catch { }
        if ($ips.Count -gt 0) {
            Say "On a real machine use this host's address instead: $($ips -join ', ')"
            Write-Host ""
        }
    }
    Say "Ctrl+C when you are done."
    Write-Host ""

    $enc = New-Object System.Text.UTF8Encoding($false)
    try {
        while ($true) {
            $client = $listener.AcceptTcpClient()
            try {
                $client.ReceiveTimeout = 5000
                $client.SendTimeout    = 15000
                $stream = $client.GetStream()

                # Read up to the end of the headers. The guest's request is a
                # couple of hundred bytes; anything that is not is not ours.
                $buf = New-Object byte[] 4096
                $req = ""
                while ($req -notmatch "`r`n`r`n") {
                    $n = $stream.Read($buf, 0, $buf.Length)
                    if ($n -le 0) { break }
                    $req += $enc.GetString($buf, 0, $n)
                    if ($req.Length -gt 16384) { break }
                }
                if (-not $req) { continue }

                $line = ($req -split "`r`n")[0]
                $bits = $line -split ' '
                $method = $bits[0]
                $path = "/"
                if ($bits.Count -gt 1) { $path = $bits[1] }
                $q = $path.IndexOf('?')
                if ($q -ge 0) { $path = $path.Substring(0, $q) }
                $name = [Uri]::UnescapeDataString($path.TrimStart('/'))

                $body = $null
                $status = "404 Not Found"
                # Nothing here is a directory, so any separator at all in the
                # name is either a typo or somebody walking up out of the repo.
                if ($name -and -not ($name -match '[\\/]') -and $name -ne ".." ) {
                    $file = Join-Path $root $name
                    if (Test-Path -LiteralPath $file -PathType Leaf) {
                        $body = [IO.File]::ReadAllBytes($file)
                        $status = "200 OK"
                    }
                }
                if ($null -eq $body) { $body = $enc.GetBytes("not here`n") }

                $head = "HTTP/1.1 $status`r`nContent-Type: application/octet-stream`r`n" +
                        "Content-Length: $($body.Length)`r`nConnection: close`r`n`r`n"
                $hb = $enc.GetBytes($head)
                $stream.Write($hb, 0, $hb.Length)
                if ($method -ne "HEAD") { $stream.Write($body, 0, $body.Length) }
                $stream.Flush()

                $mark = "->"
                if ($status -ne "200 OK") { $mark = "??" }
                Write-Host ("  {0} {1} {2} ({3} bytes)" -f $mark, $method, $path, $body.Length)
            } catch {
                # A client that hangs up mid-response is normal and not worth a
                # stack trace; keep serving.
                Write-Host "  .. dropped: $($_.Exception.Message)" -ForegroundColor DarkGray
            } finally {
                $client.Close()
            }
        }
    } finally {
        $listener.Stop()
        Write-Host ""
        Note "stopped serving"
    }
}

# -------------------------------------------------------------------- new ----
function New-Project([string] $name, [string] $kind, [string] $where) {
    if (-not $name) { Die "usage: bolt new NAME [-kind console|graphical] [-dir PATH]" }
    if ($name -notmatch '^[A-Za-z][A-Za-z0-9_-]*$') {
        Die "'$name' will not do as a package name -- letters, digits, - and _ , starting with a letter"
    }
    if ($kind -ne "console" -and $kind -ne "graphical") {
        Die "-kind is console or graphical, not '$kind'"
    }

    if (-not $where) { $where = (Get-Location).Path }
    $dir = Join-Path $where $name
    if (Test-Path -LiteralPath $dir) { Die "$dir already exists" }

    $tpl = Join-Path $SdkDir "templates\$kind"
    if (-not (Test-Path -LiteralPath $tpl)) { Die "missing template: $tpl" }

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($f in (Get-ChildItem -LiteralPath $tpl -File)) {
        $text = Get-Content -LiteralPath $f.FullName -Raw
        $text = $text.Replace("@NAME@", $name)
        $dest = Join-Path $dir $f.Name
        # LF, because these files are as likely to be read on BoltOS as here and
        # its editor has no opinion about carriage returns worth inheriting.
        $text = $text -replace "`r`n", "`n"
        [IO.File]::WriteAllText($dest, $text, (New-Object System.Text.UTF8Encoding($false)))
    }

    Good "$dir"
    Say "a $kind project. Next:"
    Say "    cd $name"
    Say "    bolt build"
    Say "    bolt ship"
}

# ------------------------------------------------------------------ doctor ---
function Invoke-Doctor {
    $tc = Get-Toolchain
    Note "BoltOS SDK, checking out $Root"
    Write-Host ""

    $rows = @(
        @("clang",  $tc.Clang),
        @("ld.lld", $tc.Lld),
        @("nasm",   $tc.Nasm)
    )
    $bad = 0
    foreach ($r in $rows) {
        if ($r[1]) {
            Write-Host ("  {0,-8} {1}" -f $r[0], $r[1]) -ForegroundColor Green
        } else {
            Write-Host ("  {0,-8} not found" -f $r[0]) -ForegroundColor Red
            $bad++
        }
    }

    Write-Host ""
    foreach ($need in @("include\syscall.h", "user\ulibc.h", "user\user.ld", "user\crt0.asm")) {
        $p = Join-Path $Root $need
        if (Test-Path -LiteralPath $p) {
            Write-Host ("  {0,-20} ok" -f $need) -ForegroundColor Green
        } else {
            Write-Host ("  {0,-20} MISSING" -f $need) -ForegroundColor Red
            $bad++
        }
    }

    Write-Host ""
    $abi = "?"
    $sh = Join-Path $Root "include\syscall.h"
    if (Test-Path -LiteralPath $sh) {
        $line = Select-String -LiteralPath $sh -Pattern 'BOLT_ABI_VERSION\s+(\d+)' |
                Select-Object -First 1
        if ($line) { $abi = $line.Matches[0].Groups[1].Value }
    }
    Say "builds against BoltOS ABI $abi"

    if ($bad -eq 0) { Good "ready" } else { Warn "$bad thing(s) to fix before this will build" }
    return $bad
}

# ------------------------------------------------------------------- clean ---
function Invoke-Clean([string] $dir) {
    $out = Join-Path $dir "build"
    if (Test-Path -LiteralPath $out) {
        Remove-Item -LiteralPath $out -Recurse -Force
        Good "removed $out"
    } else {
        Say "nothing to clean in $dir"
    }
}

function Show-Help {
    Write-Host @"
bolt -- the BoltOS SDK for Windows

  bolt new NAME [-kind console|graphical] [-dir PATH]
      Start a project: a pkg.txt manifest and a program that builds and runs.
      'console' writes to the terminal; 'graphical' takes the whole screen.

  bolt build [DIR]
      Compile and link. Out comes a static ELF64 the kernel will load, in
      DIR\build\NAME.

  bolt pack [DIR]
      Build, then wrap it in a .pkg -- the format `pkg install` reads.

  bolt ship [DIR] [-port N] [-bind ADDR]
      Build, pack, and serve the whole thing over HTTP, printing the commands
      to type in BoltOS. Default port 8420, bound to every interface so a
      machine that is not this one can reach it. Ctrl+C to stop.

  bolt clean [DIR]        throw away DIR\build
  bolt doctor             what the toolchain looks like from here

The ABI is include/syscall.h and SDK.md describes it. This tool builds against
that checkout, so a program built here matches the kernel beside it.
"@
}

# -------------------------------------------------------------------- main ---
switch ($Command.ToLower()) {
    "new" {
        New-Project (Get-Positional $Rest) (Get-Opt $Rest "kind" "console") (Get-Opt $Rest "dir" $null)
    }
    "build"  { [void](Invoke-Build (Resolve-Project $Rest) $false) }
    "pack"   { [void](Invoke-Pack  (Resolve-Project $Rest) $false) }
    "ship"   {
        $dir = Resolve-Project $Rest
        $packed = Invoke-Pack $dir $false
        $ship = Write-Repo $packed $dir
        if (Has-Flag $Rest "stage") {
            Good "staged $ship (index + package + binary), not serving"
        } else {
            $port = [int](Get-Opt $Rest "port" "8420")
            $bind = Get-Opt $Rest "bind" "0.0.0.0"
            Serve-Directory $ship $bind $port $packed.Manifest
        }
    }
    "clean"  { Invoke-Clean (Resolve-Project $Rest) }
    "doctor" { exit (Invoke-Doctor) }
    "help"   { Show-Help }
    "-h"     { Show-Help }
    "--help" { Show-Help }
    default  {
        Write-Host "[x] no such command: $Command" -ForegroundColor Red
        Write-Host ""
        Show-Help
        exit 1
    }
}
