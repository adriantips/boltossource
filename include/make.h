#pragma once
#include <stdint.h>

/* BoltMake. `target: prerequisites` and a recipe, which is how software has
 * been built since 1976 and the reason a project is a thing you type one word
 * at rather than a sequence you have to remember.
 *
 * What it understands: variables (`=`, `:=`, `+=`), `$(NAME)` and `${NAME}`,
 * the automatic `$@`, `$<` and `$^`, pattern rules with one `%`, `.PHONY`,
 * `#` comments, several targets sharing one recipe, and a default goal that is
 * the first target in the file whose name does not start with a dot. Recipes
 * run through sh_run_script(), so a recipe line is an ordinary BoltShell line
 * and gets pipes, redirection and shell variables for free.
 *
 * A recipe may be indented with spaces as well as with a tab. make(1) says
 * "missing separator" to that and has made a generation of people miserable;
 * here it is also a practical necessity, because `write` takes no escapes and
 * the shell reads a typed Tab as a completion request, so a makefile authored
 * on this machine cannot contain a tab at all.
 *
 * What it does not: conditionals, includes, functions like $(wildcard),
 * suffix rules, parallel jobs, recursive variable semantics (everything is
 * expanded when it is read, so `:=` and `=` do the same thing here). Each of
 * those is absent because nothing on this machine has wanted it yet, not
 * because it would not fit.
 *
 * It decides what is out of date by comparing each prerequisite against what
 * it looked like when the target was last built, not by asking which is newer.
 * kernel/make.c explains why at length -- the short version is that mtime here
 * is uptime ticks, so "newer" is not a thing that survives a reboot.
 */

/* Run `path` as a makefile. `goals` are the targets asked for on the command
 * line, or none for the default. `dry` prints recipes without running them,
 * `silent` runs them without printing. Returns 0 if everything asked for is
 * current, -1 having printed why (or, for a failure before any recipe ran,
 * with a message in err). */
int make_run(const char *path, const char *const *goals, int ngoal,
             int dry, int silent, char *err, uint32_t errcap);
