/*
 *
 * Copyright (C) 2006-2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#ifndef __DEBUG_H
#define __DEBUG_H

#include <syslog.h>
#include <sys/time.h>
#ifdef _USE_VALGRIND
#include <valgrind/memcheck.h>
#endif

extern int xdebug, xnolog, xopenedlog;
extern char perrorbuff[80];

__attribute__((noreturn, format(printf, 2, 3)))
void __error(int excode, const char *fmt, ...);

#define error(FMT,...)		__error(3, "Internal error: "FMT"\n" ,##__VA_ARGS__)
#define oserror(FMT,...)	__error(1, FMT": errno %d (%m)\n" ,##__VA_ARGS__ ,errno)
#define cfgerror(FMT,...)	__error(2, "%s:%d:"FMT"\n", configfile, lineno ,##__VA_ARGS__)
#define opterror(FMT,...)	__error(2, FMT"\n" ,##__VA_ARGS__)

__attribute__((format(printf, 3, 4)))
void __message(int dlevel, int level, const char *fmt, ...);

#define warning(FMT,...)        __message(0,  LOG_WARNING,FMT"\n" ,##__VA_ARGS__)
#define notice(FMT,...)		__message(0,  LOG_NOTICE, FMT"\n" ,##__VA_ARGS__)
#define info(FMT,...)		__message(0,  LOG_INFO,   FMT"\n" ,##__VA_ARGS__)
#define debug(DL, FMT,...)	__message(DL, LOG_DEBUG,  FMT"\n" ,##__VA_ARGS__)
#define debug_nocr(DL, FMT,...) __message(DL, LOG_DEBUG,  FMT     ,##__VA_ARGS__)
#define dperror(FMT,...)        __message(0,  LOG_ERR,    FMT": %m\n", ##__VA_ARGS__)

#define err_chk(cond,rc,str,lbl) if (cond) {	\
		rc = errno;			\
		dperror(str);			\
		goto lbl;			\
	}

#define errchk_e(cond,rc,val,str,lbl) if (cond) {	\
		dperror(str);				\
		rc = val;				\
		goto lbl;				\
	}

#define err_chk_rc(cond,str,lbl) err_chk(cond,rc,str,lbl)


/* Some duration helpers for debugging */
static inline void timer_start(struct timeval *tv)
{
	gettimeofday(tv, (struct timezone *) 0);
}

static inline long unsigned timer_stop(struct timeval *start)
{
	long unsigned i, j;
	struct timeval stop;
	gettimeofday (&stop, (struct timezone *) 0);

	i = stop.tv_sec - start->tv_sec;
	j = stop.tv_usec - start->tv_usec;
	j += i * 1000000;

	return j;
}

#endif
