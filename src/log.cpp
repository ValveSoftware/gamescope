#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <memory>

#include "Utils/Process.h"
#include "Utils/Defer.h"
#include "log.hpp"

LogScope::LogScope(const char *name) {
	this->name = name;
	this->priority = LOG_DEBUG;
}

LogScope::LogScope(const char *name, enum LogPriority priority) {
	this->name = name;
	this->priority = priority;
}

bool LogScope::has(enum LogPriority priority) {
	return priority <= this->priority;
}

static const char *GetLogName( LogPriority ePriority )
{
	switch ( ePriority )
	{
		case LOG_SILENT:	return "[\e[0;37m" "Shh.." "\e[0m]";
		case LOG_ERROR:		return "[\e[0;31m" "Error" "\e[0m]";
		case LOG_WARNING:	return "[\e[0;33m" "Warn" "\e[0m] ";
		case LOG_DEBUG:		return "[\e[0;35m" "Debug" "\e[0m]";
		default:
		case LOG_INFO:		return "[\e[0;34m" "Info" "\e[0m] ";
	}
}

void LogScope::vlogf(enum LogPriority priority, const char *fmt, va_list args) {
	if (!this->has(priority)) {
		return;
	}

	char *buf = nullptr;
	vasprintf(&buf, fmt, args);
	if (!buf)
		return;
	defer( free(buf); );

	for (auto& listener : m_LoggingListeners)
		listener.second( priority, this->name, buf );

	if ( bPrefixEnabled )
		fprintf(stderr, "[%s] %s \e[0;37m%s:\e[0m %s\n", gamescope::Process::GetProcessName(), GetLogName( priority ), this->name, buf);
	else
	 	fprintf(stderr, "%s\n", buf);
}

void LogScope::logf(enum LogPriority priority, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	this->vlogf(priority, fmt, args);
	va_end(args);
}

void LogScope::warnf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	this->vlogf(LOG_WARNING, fmt, args);
	va_end(args);
}

void LogScope::errorf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	this->vlogf(LOG_ERROR, fmt, args);
	va_end(args);
}

void LogScope::infof(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	this->vlogf(LOG_INFO, fmt, args);
	va_end(args);
}

void LogScope::debugf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	this->vlogf(LOG_DEBUG, fmt, args);
	va_end(args);
}

void LogScope::errorf_errno(const char *fmt, ...) {
	const char *err = strerror(errno);

	static char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	this->logf(LOG_ERROR, "%s: %s", buf, err);
}
