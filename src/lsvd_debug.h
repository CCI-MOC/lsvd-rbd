#pragma once

/* lightweight printf to buffer, retrieve via get_logbuf or lsvd.logbuf
 */
void do_log(const char *fmt, ...);
int get_logbuf(char *buf);

void fp_log(const char *fmt, ...);
