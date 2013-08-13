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

#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>

int xdebug, xnolog, xopenedlog;

__attribute__((noreturn, format(printf, 2, 3)))
void __error(int excode, const char *fmt, ...)
{
	va_list va;

	if (xnolog) {
		va_start(va, fmt);
		vfprintf(stderr, fmt, va);
		va_end(va);
	}
	else {
		if (!xopenedlog) {
			openlog("cachefilesd", LOG_PID, LOG_DAEMON);
			xopenedlog = 1;
		}

		va_start(va, fmt);
		vsyslog(LOG_ERR, fmt, va);
		va_end(va);

		closelog();
	}

	exit(excode);
}

__attribute__((format(printf, 3, 4)))
void __message(int dlevel, int level, const char *fmt, ...)
{
	va_list va;

	if (dlevel <= xdebug) {
		if (xnolog) {
			va_start(va, fmt);
			vfprintf(stderr, fmt, va);
			va_end(va);
		}
		else if (!xnolog) {
			if (!xopenedlog) {
				openlog("cachefilesd", LOG_PID, LOG_DAEMON);
				xopenedlog = 1;
			}

			va_start(va, fmt);
			vsyslog(level, fmt, va);
			va_end(va);

			closelog();
		}
	}
}
