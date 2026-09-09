# BoltShell development tools

BoltShell uses the same interpreter at the text prompt, in the GUI Terminal,
and for `sh FILE [args...]`. `help COMMAND` and `man COMMAND` show a command's
synopsis and explanation. `man -k WORD` searches the manual; `man -c` checks
that every registered command has a page. Pages identify legacy placeholders
and advisory controls so they are not confused with working drivers or services.

## Editing commands

- Left/Right and Home/End move the caret. Typing inserts at the caret;
  Backspace and Delete remove characters on either side.
- Up/Down browse the last 48 commands. Down restores the draft you were typing.
- Tab at the end of the line completes command names or filesystem paths.
  It extends a common prefix, or lists alternatives. Directories end in `/`.
  Completion currently expects unquoted paths without spaces.
- Ctrl+C clears input in the text shell. In the GUI Terminal, Ctrl+C/Ctrl+V
  retain their clipboard meanings.

History is shared by the local shell front ends for the current account.
It is saved to `~/.bolt_history` on account changes, logout, poweroff or reboot,
and loaded when history is first needed. The file is mode 0600. The bootstrap
shell, which has no signed-in home, uses `/tmp/.bolt_history`.

A leading space excludes a command from history. Lines containing known
credential-taking commands (`users`, `useradd`, `passwd`, `install`, `vault`)
are also excluded. Use the leading-space convention for other sensitive input;
history filtering does not redact the serial console or kernel logs. Unsaved
history can be lost on a hard power cut. Switching accounts clears shell
variables and functions as well as the in-memory history.

## Variables and quoting

```sh
NAME=world
echo "hello $NAME"
LITERAL='$NAME stays literal'
echo "$LITERAL"
false ; echo status=$?
TOTAL=$((2 * (3 + 4)))
echo total=$TOTAL
```

`set` lists variables; `set NAME VALUE` and `unset NAME` update them. `$NAME`
and `${NAME}` expand variables, `$?` is the last returned status, and `$0`..`$9`
and `$#` expose script or function arguments. Unset variables expand to an empty
string. Expansion keeps the result in one argument; there is no implicit word
splitting, globbing, command substitution, exported process environment or tilde
expansion.

Single quotes preserve their contents literally, including backslashes. Double
quotes permit expansion; `\"`, `\\`, and `\$` escape those three characters.
Other double-quoted escapes, such as `\n` in C source, remain intact. Outside
quotes a backslash escapes the following character. Comments start with `#`
at a word boundary and continue to the newline.

Arithmetic supports integer `+ - * / %`, parentheses, unary signs and comparisons
`< <= > >= == !=`. Bare variable names are accepted inside `$((...))`.
Arithmetic wraps at 64 bits; division by zero yields zero. This is a bounded
integer expression evaluator, not the full POSIX shell arithmetic language.

## Conditions, loops and functions

Save this as a file with `edit /tmp/count.sh`, then run `sh /tmp/count.sh Ada`:

```sh
greet() {
    echo "hello $1"
}
greet "$1"
total=0
for n in 1 2 3 4; do
    total=$((total + n))
done
if test $total -eq 10; then
    echo sum-correct
elif test $total -gt 10; then
    echo sum-too-large
else
    echo sum-too-small
fi
i=0
while true; do
    i=$((i + 1))
    if test $i -ge 3; then break; fi
done
exit 3
```

`test` supports `-f`, `-d`, `-e`, `-z`, `-n`, string `=`/`!=`, and integer
`-eq -ne -lt -le -gt -ge`. `[ EXPR ]` is the same command with a required closing
bracket. `true` returns 0, `false` returns 1. Unknown commands return 127.
Existing commands supply their own status; a legacy command may still report
an error in its text while returning zero.

`break` and `continue` affect the nearest loop. `return N` leaves the function
even from a nested conditional. `exit N` ends the current script invocation,
returning N to the caller; it does not power off the machine. Variables and
function definitions remain in the current shell session. `source` and `.`
are aliases for `sh`, since there are no subshells.

At the interactive prompt a compound statement must fit on one line, separated
by semicolons. In script files use newlines or semicolons. A function header
must end its segment: `greet() {; echo "hello $1"; }` works on one line.
The interpreter is a shell subset, not a drop-in replacement for Bash or `sh`.

## Redirection and pipelines

```sh
echo first > /tmp/result
echo second >> /tmp/result
cat /tmp/result | grep second
sh /tmp/count.sh Ada > /tmp/count-output
```

`>` replaces a file; `>>` appends. The destination's write permission is checked
before executing the command. Quoted `">"` and `"|"` are ordinary arguments.
Each invocation has its own capture buffer, so a script can redirect one of its
commands without corrupting its own redirected output.

Pipelines retain BoltShell's existing file-operand convention: an intermediate
stage writes a temporary file and its path is appended to the next command's
arguments. They are sequential, not concurrent stdin/stdout Unix pipes. Temporary
files are removed after the pipeline. Console output still reaches the serial
log. Input redirection, `&&`/`||`, and background job control are not supported.

## Limits and diagnostics

The prompt accepts 255 characters; expanded commands accept 24 arguments in a
4 KiB buffer. A script accepts 64 KiB and 512 statements. There are 48 variables
(23-character names, 191-character values), 16 functions with bodies below
1 KiB, and nine positional arguments of up to 95 characters each.

Nested execution is bounded to eight statement/script levels. Each top-level
invocation has a budget of 10,000 execution steps, including loop conditions;
a runaway loop reports `execution limit reached` and returns status 2. Redirection
accepts up to 32 KiB of output and reports overflow instead of saving truncated
data. These limits protect the kernel shell's finite stack and heap.

## Building native user programs

See [SDK.md](SDK.md) for the static ELF64 ABI, headers, runtime and host build
helper. `run NAME [args...]` resolves `/bin/NAME`; `run ./app` uses a relative
path. Set execute permission with `chmod 755 /bin/NAME` after downloading.
The program's exit code becomes `$?`. `run` waits up to 60 seconds; a timed-out
process continues running and can be managed with `ps` and `kill`.

The on-machine `cc` and `build` commands still execute BoltCC bytecode.
Native code generation, a native linker and self-hosting are separate roadmap
items; the SDK does not implement them.

## Validation

After building the OS, run:

```sh
python test/run_all.py man script sdk shell gui security
```

These tests exercise real QEMU guests: manual coverage, quoting and script
control flow, bounded runaway execution, nested redirection, SDK downloads and
exit statuses, malformed ELF rejection, history persistence and account changes,
plus editing in both the text shell and GUI Terminal.

The September 2026 continuation also passed BIOS boot/menu, UEFI boot,
filesystem operations and persistence, compiler, core-dump, package, project,
and scheduler regressions: 16 selected tests in total. The first combined run
passed 15/16; the GUI test used an obsolete desktop-icon coordinate. After
switching that test to Start search, its rerun passed with actual Terminal
command, editing, history and completion assertions.
