#pragma once
/* BoltShell: a line-editing command interpreter driven by the PS/2 keyboard and
 * rendered through the framebuffer console. Never returns. */
void shell_run(void);
void shell_exec_line(char *line);  /* tokenize + dispatch one line (GUI terminal) */
