#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#ifdef __GNUC__
#define ATTRIB_PRINTF(start, end) __attribute__((format(printf, start, end)))
#else
#define ATTRIB_PRINTF(start, end)
#endif

enum LogPriority {
	LOG_SILENT,
	LOG_ERROR,
	LOG_INFO,
	LOG_DEBUG,
};

class LogScope {
	const char *name;
	enum LogPriority priority;

	bool has(enum LogPriority priority);
	void vprintf(enum LogPriority priority, const char *fmt, va_list args) ATTRIB_PRINTF(3, 0);
	void logf(enum LogPriority priority, const char *fmt, ...) ATTRIB_PRINTF(3, 4);

public:
	LogScope(const char *name);
	LogScope(const char *name, enum LogPriority priority);

	void vlogf(enum LogPriority priority, const char *fmt, va_list args) ATTRIB_PRINTF(3, 0);

	void errorf(const char *fmt, ...) ATTRIB_PRINTF(2, 3);
	void infof(const char *fmt, ...) ATTRIB_PRINTF(2, 3);
	void debugf(const char *fmt, ...) ATTRIB_PRINTF(2, 3);

	void errorf_errno(const char *fmt, ...) ATTRIB_PRINTF(2, 3);
};
