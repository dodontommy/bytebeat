/* ui.h -- the ncurses front end. */
#ifndef UI_H
#define UI_H

/* use_tty: open /dev/tty for the display instead of stdout. Needed when
 * stdout is carrying raw PCM to a pipe. */
int  ui_init(int use_tty);
void ui_shutdown(void);

/* Runs until the user quits. Owns the poll loop, which also drives
 * sink_service() and bb_reclaim(). */
void ui_run(void);

/* Transient message on the status line (about 2 seconds). */
void ui_status(const char *fmt, ...);

/* Persistent warning shown at startup, e.g. "no SCHED_FIFO". */
void ui_set_warning(const char *msg);

/* Load example number i (wrapping) into the focused layer plus its knobs. */
void ui_load_example(int i);

/* First launch: build a five-part noise groove so the instrument makes a
 * complete, editable beat the moment it opens. */
void ui_first_run(void);

#endif /* UI_H */
