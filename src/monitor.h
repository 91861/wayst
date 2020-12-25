
#pragma once

#define _GNU_SOURCE

#ifdef __linux
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
#include <termios.h>
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#include <termios.h>
#endif

#include <sys/poll.h>

#include "settings.h"
#include "util.h"

#ifndef MONITOR_INPUT_BUFFER_SZ
#define MONITOR_INPUT_BUFFER_SZ 256
#endif

typedef struct
{
    int           child_fd, parent_fd, extra_fd;
    struct pollfd pollfds[2];
#define CHILD_FD_IDX 0
#define EXTRA_FD_IDX 1
    bool  read_info_up_to_date;
    pid_t child_pid;
    bool  child_is_dead;
    char  input_buffer[MONITOR_INPUT_BUFFER_SZ];

    struct MonitorCallbacks
    {
        void* user_data;
        void (*on_exit)(void*);
    } callbacks;

} Monitor;

/**
 * Create a new monitor object */
Monitor Monitor_new();

/**
 * fork and set up a pty connection */
void Monitor_fork_new_pty(Monitor* self, uint32_t cols, uint32_t rows);

/**
 * Wait for any activity */
bool Monitor_wait(Monitor* self, int timeout);

/**
 * Try to read data from the child process */
ssize_t Monitor_read(Monitor* self);

/**
 * Write data to the child process */
ssize_t Monitor_write(Monitor* self, char* buffer, size_t bytes);

/**
 * Kill the child process */
void Monitor_kill(Monitor* self);

/**
 * Set an extra file descriptor to monitor for activity when wait()-ing */
void Monitor_watch_window_system_fd(Monitor* self, int fd);

/**
 * Check read can be performed on the 'extra' fd */
static bool Monitor_are_window_system_events_pending(Monitor* self)
{
    return self->pollfds[EXTRA_FD_IDX].revents & POLLIN;
}
