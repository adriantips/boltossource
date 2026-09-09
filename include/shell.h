#pragma once
/* BoltShell: a line-editing command interpreter driven by the PS/2 keyboard and
 * rendered through the framebuffer console. Never returns. */
void shell_run(void);
void shell_exec_line(char *line);  /* tokenize + dispatch one line (GUI terminal) */

/* ---------------------------------------------------------------------------
 *  Line-editing services (kernel/shline.c), shared by the serial/framebuffer
 *  shell and the GUI Terminal window. A shell that completes a filename in one
 *  window and not in the other is worse than one that never does.
 * ------------------------------------------------------------------------- */
void        sh_history_add(const char *line);
const char *sh_history_get(int back);   /* 1 = most recent, 0 when out of range */
int         sh_history_count(void);
void        sh_history_load(void);      /* from ~/.bolt_history; idempotent    */
void        sh_history_save(void);      /* called when a session ends          */
void        sh_history_reset(void);     /* discard the departing user's state */

/* Complete the token the caret is in (the caret is at `len`). Returns the new
 * length. When several candidates share no further characters they are printed
 * and *printed is set, so the caller knows to redraw its prompt. */
int         sh_complete(char *line, int cap, int len, int *printed);

/* ---------------------------------------------------------------------------
 *  Scripting (kernel/shscript.c). Everything the shell runs -- a line typed at
 *  the prompt, a line from the GUI terminal, a file given to `sh` -- goes
 *  through sh_run_script(), so there is one language rather than an
 *  interactive dialect and a scripting one. It calls back into
 *  sh_exec_simple() for anything that is just a command.
 * ------------------------------------------------------------------------- */
int         sh_run_script(const char *text, int argc, char **argv);
int         sh_exec_simple(char *seg);      /* one command: pipes, redirection */
int         sh_words(const char *text, char *buf, int cap, char **argv, int maxarg,
                     unsigned char *operators);
void        sh_script_reset(void);

const char *sh_var_get(const char *name);
int         sh_var_set(const char *name, const char *value);
void        sh_var_unset(const char *name);
int         sh_expand(const char *in, char *out, int cap);
void        sh_expand_at(const char **p, char *out, int cap, int *o);
int         sh_status(void);
void        sh_set_status(int status);
