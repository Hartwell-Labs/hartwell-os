/* Error codes: one stable code per failure, one line of context, and a book
 * (docs/errors.md) that explains every code end to end. */
#ifndef HWO_ERRORS_H
#define HWO_ERRORS_H

#include <stdio.h>

/* Severity: 'F' fatal, 'E' error, 'W' warning. Appends to /run/hwo.err and
 * echoes on stderr, which is the serial console. */
void hwo_note(char sev, const char *code, const char *fmt, ...);

int hwo_errs_known(const char *code);
const char *hwo_err_title(const char *code);
const char *hwo_err_fix(const char *code);
void hwo_errs_split(int *errs, int *warns);
int hwo_last_error(char *code_out, unsigned long n, char *msg_out, unsigned long mn);
int hwo_errs_count(void);
int hwo_errs_print(FILE *out);

#endif
