#define _POSIX_C_SOURCE 200809L

#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static void logger_write_locked(Logger *logger, const char *text, size_t length)
{
    (void)write_all(STDOUT_FILENO, text, length);
    if (logger->fd >= 0)
        (void)write_all(logger->fd, text, length);
}

int logger_open(Logger *logger, const char *path)
{
    if (logger == NULL)
        return -1;

    logger->fd = -1;
    if (pthread_mutex_init(&logger->mutex, NULL) != 0)
        return -1;

    if (path != NULL) {
        logger->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logger->fd < 0) {
            perror(path);
            pthread_mutex_destroy(&logger->mutex);
            return -1;
        }
    }
    return 0;
}

void logger_close(Logger *logger)
{
    if (logger == NULL)
        return;
    if (logger->fd >= 0) {
        (void)close(logger->fd);
        logger->fd = -1;
    }
    (void)pthread_mutex_destroy(&logger->mutex);
}

void logger_event(Logger *logger, uint64_t model_time, const char *format, ...)
{
    char message[1024];
    char line[1200];
    va_list args;
    int message_len;
    int line_len;

    if (logger == NULL || format == NULL)
        return;

    va_start(args, format);
    message_len = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (message_len < 0)
        return;

    line_len = snprintf(line, sizeof(line), "[t=%6llu] %s\n",
                        (unsigned long long)model_time, message);
    if (line_len < 0)
        return;
    if ((size_t)line_len >= sizeof(line))
        line_len = (int)sizeof(line) - 1;

    pthread_mutex_lock(&logger->mutex);
    logger_write_locked(logger, line, (size_t)line_len);
    pthread_mutex_unlock(&logger->mutex);
}

void logger_line(Logger *logger, const char *format, ...)
{
    char line[2048];
    va_list args;
    int length;

    if (logger == NULL || format == NULL)
        return;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0)
        return;
    if ((size_t)length >= sizeof(line))
        length = (int)sizeof(line) - 1;

    pthread_mutex_lock(&logger->mutex);
    logger_write_locked(logger, line, (size_t)length);
    pthread_mutex_unlock(&logger->mutex);
}
