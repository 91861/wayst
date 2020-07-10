
#pragma once

#define _GNU_SOURCE

#ifdef __linux
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#include <termios.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#include <termios.h>
#endif

#include "settings.h"
#include "util.h"

#ifndef MONITOR_INPUT_BUFFER_SZ
#define MONITOR_INPUT_BUFFER_SZ 128
#endif

typedef struct
{
    int    master, slave, extra;
    fd_set wfdset, rfdset;
    pid_t  pid;
    bool   exit;
    char   input_buffer[MONITOR_INPUT_BUFFER_SZ];
} Monitor;

Monitor Monitor_fork_new_pty(uint32_t cols, uint32_t rows);
bool Monitor_wait(Monitor* self);
ssize_t Monitor_read(Monitor* self);
int Monitor_write(Monitor* self, char* buffer, size_t bytes);
void Monitor_kill(Monitor* self);
void Monitor_watch_fd(Monitor* self, int fd);
