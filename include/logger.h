#ifndef LOGGER_H
#define LOGGER_H

#include <pthread.h>
#include <stdint.h>

typedef struct {
    int fd;
    pthread_mutex_t mutex;
} Logger;

int logger_open(Logger *logger, const char *path);
void logger_close(Logger *logger);
void logger_event(Logger *logger, uint64_t model_time, const char *format, ...);
void logger_line(Logger *logger, const char *format, ...);

#endif
