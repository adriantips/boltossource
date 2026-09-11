#!/usr/bin/env python3
"""Structs in BoltCC: records, arrays of them, and lists built out of them.

A C compiler with no structs is a calculator with functions. This drives the
things you actually reach for: a record with fields, a function that reads one,
an array of them, and a linked list -- which needs a struct whose field refers
to its own type, and a null check on the end of it.

The one thing that is not C here, and the test says so out loud: a struct is a
reference, so passing one to a function does not copy it. In C that is what
passing a pointer does. The compiler has no types to tell the two apart with,
and quietly copying or quietly not copying are both wrong -- so it does the one
that makes `->` work, and the manual says which one it picked.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

POINT = [
    'struct Point { int x; int y; };',
    'int dist2(struct Point p) { return p.x * p.x + p.y * p.y; }',
    'void shift(struct Point p, int dx) { p.x += dx; }',
    'int main() {',
    '  struct Point a;',
    '  a.x = 3; a.y = 4;',
    '  printf(\\"d2=%d\\\\n\\", dist2(a));',
    '  shift(a, 10);',
    '  printf(\\"shifted=%d\\\\n\\", a.x);',
    '  struct Point *p;',
    '  p = &a;',
    '  p->y = 7;',
    '  printf(\\"through-pointer=%d\\\\n\\", a.y);',
    '  return 0;',
    '}',
]

ARR = [
    'struct Item { int id; int qty; };',
    'int main() {',
    '  struct Item cart[4];',
    '  int total = 0;',
    '  for (int i = 0; i < 4; i++) { cart[i].id = i; cart[i].qty = i * 2; }',
    '  for (int i = 0; i < 4; i++) total += cart[i].qty;',
    '  printf(\\"total=%d last=%d\\\\n\\", total, cart[3].id);',
    '  return 0;',
    '}',
]

LIST = [
    'struct Node { int val; struct Node *next; };',
    'int main() {',
    '  struct Node a; struct Node b; struct Node c;',
    '  a.val = 1; b.val = 2; c.val = 3;',
    '  a.next = &b; b.next = &c; c.next = 0;',
    '  int sum = 0; int n = 0;',
    '  struct Node *p = &a;',
    '  while (p) { sum += p->val; n++; p = p->next; }',
    '  printf(\\"nodes=%d sum=%d\\\\n\\", n, sum);',
    '  return 0;',
    '}',
]

TYPEDEF = [
    'typedef struct { int w; int h; } Box;',
    'int area(Box b) { return b.w * b.h; }',
    'int main() {',
    '  Box b;',
    '  b.w = 6; b.h = 7;',
    '  printf(\\"area=%d\\\\n\\", area(b));',
    '  return 0;',
    '}',
]

BADFIELD = [
    'struct Point { int x; int y; };',
    'int main() {',
    '  struct Point p;',
    '  p.z = 1;',
    '  return 0;',
    '}',
]


def write_file(vm, path, lines):
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def main():
    with BoltVM(name="struct", extra=["-vga", "none"]) as vm:
        vm.reach_shell(180, "2")
        vm.run("mkdir /st")

        write_file(vm, "/st/point.c", POINT)
        out = vm.run("cc /st/point.c", timeout=120)
        assert "d2=25" in out, "fields do not read:\n" + out[-700:]
        assert "shifted=13" in out, \
            "a struct passed to a function should be the caller's:\n" + out[-700:]
        assert "through-pointer=7" in out, "-> does not reach the same struct:\n" + out[-700:]
        print("  ok: fields, a struct argument, & and -> all reach one object")

        write_file(vm, "/st/arr.c", ARR)
        out = vm.run("cc /st/arr.c", timeout=120)
        assert "total=12" in out and "last=3" in out, \
            "an array of structs does not work:\n" + out[-700:]
        print("  ok: an array of structs is an array of structs, not of zeros")

        write_file(vm, "/st/list.c", LIST)
        out = vm.run("cc /st/list.c", timeout=120)
        assert "nodes=3" in out and "sum=6" in out, \
            "a linked list does not walk:\n" + out[-700:]
        print("  ok: a struct can refer to its own type, and the list ends at 0")

        write_file(vm, "/st/box.c", TYPEDEF)
        out = vm.run("cc /st/box.c", timeout=120)
        assert "area=42" in out, "typedef struct does not declare a usable type:\n" + out[-700:]
        print("  ok: typedef struct { ... } Name declares a type you can use")

        # A field that does not exist is an error that names it, not a zero.
        write_file(vm, "/st/bad.c", BADFIELD)
        out = vm.run("cc /st/bad.c", timeout=120)
        assert "no such field" in out and "z" in out, \
            "an unknown field was not reported:\n" + out[-700:]
        print("  ok: an unknown field is named, not silently read as zero")

        vm.expect_absent("KERNEL PANIC")

    print("PASS struct: records, arrays of them, and linked lists compile and run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
