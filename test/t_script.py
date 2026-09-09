#!/usr/bin/env python3
"""The shell as a language: variables, conditions, loops, functions.

BoltShell could run a command, a list of commands, and a pipeline. It could not
ask a question about the answer -- which is the difference between a shell you
type at and one you can automate with, and on a machine meant for development
it is the difference between doing a job once and doing it every time.

Two things are checked, because they are two claims:

  * the same language works typed at the prompt and stored in a file -- there
    is one interpreter, not an interactive dialect and a scripting one
  * it has what a script actually needs: `$?`, variables, arithmetic, if/elif/
    else, for, while with break, functions with arguments, and an exit status
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

# Written with single quotes so the shell stores the `$` rather than expanding
# it -- which is itself the thing single quotes are for.
SCRIPT = [
    '# count.sh -- everything a small script needs',
    'greet() {',
    '  echo hello $1',
    '}',
    'greet world',
    'total=0',
    'for n in 1 2 3 4; do',
    '  total=$((total + n))',
    'done',
    'echo total=$total',
    'if test $total -eq 10; then',
    '  echo sum-correct',
    'else',
    '  echo sum-wrong',
    'fi',
    'i=0',
    'while true; do',
    '  i=$((i + 1))',
    '  if test $i -ge 3; then break; fi',
    'done',
    'echo looped=$i',
    'echo args=$# first=$1',
    'exit 3',
]


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    vm.wait_prompt()


def main():
    with BoltVM(name="script", extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        # --- at the prompt ---------------------------------------------------
        out = vm.run("NAME=world ; echo hello $NAME")
        assert "hello world" in out, "a variable did not expand:\n" + out
        out = vm.run("echo '$NAME stays'")
        assert "$NAME stays" in out, "single quotes did not protect the $:\n" + out
        print("  ok: variables expand, and single quotes stop them")

        out = vm.run("true ; echo a=$? ; false ; echo b=$?")
        assert "a=0" in out and "b=1" in out, "$? does not follow the exit status:\n" + out
        print("  ok: `$?` is the last command's status")

        out = vm.run("if test -f /etc/motd; then echo has-motd; else echo no-motd; fi", timeout=60)
        assert "has-motd" in out, "the `if` did not take the true branch:\n" + out
        out = vm.run("if test 1 -eq 2; then echo wrong; elif test 2 -eq 2; then echo elif-works; fi",
                     timeout=60)
        assert "elif-works" in out, "`elif` did not run:\n" + out
        print("  ok: if / elif / else, on one line")

        out = vm.run("for i in a b c; do echo n=$i; done", timeout=60)
        assert "n=a" in out and "n=b" in out and "n=c" in out, "the `for` loop:\n" + out
        out = vm.run("N=0 ; while test $N -lt 3; do echo loop$N; N=$((N + 1)); done", timeout=60)
        assert "loop0" in out and "loop2" in out and "loop3" not in out, \
            "the `while` loop, or its arithmetic:\n" + out
        print("  ok: for and while, with `$(( ))` to move the counter")

        out = vm.run("echo sum=$((2 * (3 + 4)))")
        assert "sum=14" in out, "arithmetic precedence is wrong:\n" + out

        out = vm.run("VALUE='$NAME literal' ; echo value=$VALUE")
        assert "value=$NAME literal" in out, out
        out = vm.run('echo ">" "|" ";"')
        assert "\n> | ;\r\n" in out, out
        out = vm.run('echo preserved > /tmp/redirect ; cat /tmp/redirect')
        assert "preserved\r\nbolt:" in out, out
        out = vm.run('echo more >> /tmp/redirect ; cat /tmp/redirect')
        assert "preserved\r\nmore\r\n" in out, out
        out = vm.run('echo piped | cat')
        assert "piped\r\nbolt:" in out, out
        out = vm.run('for x in "two words" three; do echo word=$x; done')
        assert "word=two words" in out and "word=three" in out, out
        out = vm.run('answer() {; if true; then return 9; fi; echo unreachable; }; answer; echo answer=$?')
        assert "answer=9" in out and "\nunreachable\r\n" not in out, out
        out = vm.run('nosuchcommand ; echo missing=$?')
        assert "missing=127" in out, out
        print("  ok: quoting, redirection, pipelines, nested return, and failure status")

        out = vm.run('echo arithmetic=$((-9223372036854775808 / -1))')
        assert 'arithmetic=-9223372036854775808' in out, out
        out = vm.run('while true; do true; done', timeout=30)
        assert 'execution limit reached' in out, out
        out = vm.run('echo after-limit=$?')
        assert 'after-limit=2' in out, out
        out = vm.run('if true; then echo invalid; done')
        assert '`if` with no `fi`' in out, out
        vm.run("write /recursive.sh 'sh /recursive.sh'")
        out = vm.run('sh /recursive.sh', timeout=30)
        assert 'nested too deeply' in out, out
        vm.run("write /nested.sh 'echo outer-one; echo inner > /tmp/inner; echo outer-two'")
        vm.run('sh /nested.sh > /tmp/outer')
        out = vm.run('cat /tmp/outer')
        assert 'outer-one\r\nouter-two\r\n' in out, out
        assert '\ninner\r\n' not in out, out
        out = vm.run('cat /tmp/inner')
        assert '\ninner\r\n' in out, out
        print('  ok: arithmetic bounds, runaway limits, recursive scripts and nested redirection')

        # --- and the same language, in a file --------------------------------
        vm.run("write /count.sh '%s'" % SCRIPT[0])
        for line in SCRIPT[1:]:
            vm.run("write -a /count.sh '%s'" % line)

        out = vm.run("sh /count.sh alpha beta ; echo exit=$?", timeout=120)
        assert "hello world" in out, "the function did not run with its argument:\n" + out
        assert "total=10" in out, "the for loop did not accumulate:\n" + out
        assert "sum-correct" in out, "the multi-line `if` did not run:\n" + out
        assert "looped=3" in out, "`break` did not leave the loop:\n" + out
        assert "args=2 first=alpha" in out, \
            "the script's arguments did not arrive as $# and $1:\n" + out
        assert "exit=3" in out, "`exit 3` did not become the script's status:\n" + out
        print("  ok: the same language, in a file, with arguments and an exit status")

        # A script is not a special case: `sh` runs the same interpreter.
        out = vm.run("sh /nosuch.sh")
        assert "cannot read" in out, "a missing script was not reported:\n" + out

        vm.expect_absent("KERNEL PANIC")

    print("PASS script: the shell is a language, at the prompt and in a file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
