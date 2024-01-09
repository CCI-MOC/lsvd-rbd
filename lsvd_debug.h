/*
 * file:        lsvd_debug.h
 * description: extern functions for unit tests
 */

#ifndef __LSVD_DEBUG_H__
#define __LSVD_DEBUG_H__

/* lightweight printf to buffer, retrieve via get_logbuf or lsvd.logbuf
 */
void do_log(const char *fmt, ...);
int get_logbuf(char *buf);

void fp_log(const char *fmt, ...);

#endif
