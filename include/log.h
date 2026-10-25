#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3
} LogLevel;

/* Initialize log system: attempts to open log file, starts background worker thread */
void log_init(void);

/* Shutdown log system: flushes remaining logs, stops worker, closes file */
void log_shutdown(void);

/* Queue an event to the circular buffer (lock-protected, no I/O in caller thread, hook-safe) */
void log_event(LogLevel level, const char* fmt, ...);

/* Queue an event and request a rate-limited on-screen notification */
void log_notify(const char* fmt, ...);

/* Flush queued log entries to file and process pending notifications */
void log_flush(void);

#endif // LOG_H
