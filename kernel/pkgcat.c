#include <stdint.h>
#include "pkg.h"

/* ===========================================================================
 *  BoltOS  -  kernel/pkgcat.c
 *  The catalogue that ships with the system.
 *
 *  Small on purpose. A curated set is worth more than a long one here: each of
 *  these adds something the machine could not do a moment before, and between
 *  them they exercise every part of the package format -- a script, headers a
 *  BoltCC program can `#include`, and a whole project with dependencies that
 *  `build` picks up and runs the moment it is installed.
 *
 *  Everything here is written into /var/lib/pkg/repo as real .pkg files the
 *  first time a repository is needed, so `pkg update` and `pkg install` go
 *  through exactly the same code they would for a package fetched over HTTP.
 * ===========================================================================*/

/* --------------------------------------------------------------- scripts -- */
static const pkgfile_t F_HELLO[] = {
    { "/apps/hello.py", "print('Hello from a BoltOS package!')\n" },
};
static const pkgfile_t F_FIB[] = {
    { "/apps/fib.py",
      "a = 0\nb = 1\ni = 0\nwhile i < 15:\n    print(a)\n    t = a + b\n"
      "    a = b\n    b = t\n    i = i + 1\n" },
};
static const pkgfile_t F_PRIMES[] = {
    { "/apps/primes.py",
      "n = 2\nwhile n < 50:\n    d = 2\n    p = 1\n    while d * d <= n:\n"
      "        if n % d == 0:\n            p = 0\n        d = d + 1\n"
      "    if p == 1:\n        print(n)\n    n = n + 1\n" },
};

/* ------------------------------------------------------- headers for `cc` -- */
/* Both of these are written to be compiled by BoltCC, which has no pointers and
 * no structs: integers, strings and functions. Defining the functions in the
 * header is right for a compiler whose only build model is a unity build. */
static const pkgfile_t F_BOLTLIB[] = {
    { "/usr/include/bolt.h",
      "#ifndef BOLT_H\n"
      "#define BOLT_H\n"
      "/* Small things every program ends up writing for itself. */\n"
      "#define MIN(a, b)        ((a) < (b) ? (a) : (b))\n"
      "#define MAX(a, b)        ((a) > (b) ? (a) : (b))\n"
      "#define CLAMP(x, lo, hi) MIN(MAX((x), (lo)), (hi))\n"
      "#define SQR(x)           ((x) * (x))\n"
      "#define EVEN(x)          (((x) % 2) == 0)\n"
      "/* A check that says what failed instead of just stopping. */\n"
      "#define CHECK(cond, msg) if (!(cond)) printf(\"CHECK failed: %s\\n\", msg)\n"
      "#endif\n" },
};
static const pkgfile_t F_MATHX[] = {
    { "/usr/include/mathx.h",
      "#ifndef MATHX_H\n"
      "#define MATHX_H\n"
      "/* Integer maths, for a machine with no floating point in its compiler. */\n"
      "int ipow(int b, int e) {\n"
      "    int r = 1;\n"
      "    for (int i = 0; i < e; i++) r = r * b;\n"
      "    return r;\n"
      "}\n"
      "int isqrt(int n) {\n"
      "    int r = 0;\n"
      "    while ((r + 1) * (r + 1) <= n) r++;\n"
      "    return r;\n"
      "}\n"
      "int fact(int n) {\n"
      "    int r = 1;\n"
      "    for (int i = 2; i <= n; i++) r = r * i;\n"
      "    return r;\n"
      "}\n"
      "int is_prime(int n) {\n"
      "    if (n < 2) return 0;\n"
      "    for (int d = 2; d * d <= n; d++) if (n % d == 0) return 0;\n"
      "    return 1;\n"
      "}\n"
      "#endif\n" },
};

/* ------------------------------------------------------- a whole project --- */
/* Installing this leaves a project on disk that `build` compiles and runs
 * without another thing being typed -- which is the point of a package manager
 * on a machine you develop on. */
static const pkgfile_t F_CPROJECT[] = {
    { "/home/Documents/newproj/build.txt",
      "# What `build` compiles here. See: help build\n"
      "target newproj\n"
      "  src main.c util.c\n" },
    { "/home/Documents/newproj/util.h",
      "#ifndef UTIL_H\n"
      "#define UTIL_H\n"
      "int score(int n);\n"
      "#endif\n" },
    { "/home/Documents/newproj/util.c",
      "#include \"util.h\"\n"
      "#include <mathx.h>\n"
      "\n"
      "int score(int n) {\n"
      "    return fact(n) + isqrt(n);\n"
      "}\n" },
    { "/home/Documents/newproj/main.c",
      "#include <bolt.h>\n"
      "#include <mathx.h>\n"
      "#include \"util.h\"\n"
      "\n"
      "int main() {\n"
      "    printf(\"newproj: a project the package manager installed\\n\");\n"
      "    for (int i = 1; i <= 5; i++)\n"
      "        printf(\"  score(%d) = %d\\n\", i, score(i));\n"
      "    printf(\"  max=%d ipow(2,10)=%d primes<10:\", MAX(3, 8), ipow(2, 10));\n"
      "    for (int i = 2; i < 10; i++) if (is_prime(i)) printf(\" %d\", i);\n"
      "    printf(\"\\n\");\n"
      "    CHECK(score(3) == 7, \"score(3)\");\n"
      "    return 0;\n"
      "}\n" },
};

#define NF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const pkgdef_t CATALOG[] = {
    { "hello",  "1.0", "script", "a greeting, in BoltPython",
      "", "/apps/hello.py",  F_HELLO,  NF(F_HELLO) },
    { "fib",    "1.0", "script", "the first 15 Fibonacci numbers",
      "", "/apps/fib.py",    F_FIB,    NF(F_FIB) },
    { "primes", "1.0", "script", "the primes below 50",
      "", "/apps/primes.py", F_PRIMES, NF(F_PRIMES) },
    { "boltlib", "1.0", "source", "MIN/MAX/CLAMP/CHECK macros for cc",
      "", "", F_BOLTLIB, NF(F_BOLTLIB) },
    { "mathx",   "1.0", "source", "integer maths: ipow, isqrt, fact, is_prime",
      "", "", F_MATHX,   NF(F_MATHX) },
    { "cproject", "1.0", "source", "a starter multi-file C project in ~/Documents",
      "boltlib mathx", "", F_CPROJECT, NF(F_CPROJECT) },
};

const pkgdef_t *pkg_builtin(int *count) {
    if (count) *count = (int)(sizeof(CATALOG) / sizeof(CATALOG[0]));
    return CATALOG;
}
