#define _GNU_SOURCE

#include "monitor.h"

#include <signal.h>
#include <sys/select.h>
#include <utmp.h>

#include <errno.h> 
#include <stdbool.h>
#include <stdint.h>

Monitor Monitor_fork_new_pty(struct winsize* ws)
{
    Monitor self;
    self.exit = false;
    self.extra = 0;
    openpty(&self.master, &self.slave, NULL, NULL, ws);
    self.pid = fork();
    if (self.pid == 0) {
        close(self.master);
        login_tty(self.slave);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");
        setenv("COLORTERM", "truecolor", 1);
        setenv("VTE_VERSION", "5602", 1);
        setenv("TERM", settings.term, 1);
        if (execvp(settings.shell, (char* const*)settings.shell_argv))
            printf(TERMCOLOR_RED "Failed to execute command");
    } else if (self.pid < 0)
        ERR("Failed to fork process %s", strerror(errno));

    close(self.slave);
    return self;
}

bool Monitor_wait(Monitor* self)
{
    // TODO: poll()
    
    FD_ZERO(&self->rfdset);
    FD_ZERO(&self->wfdset);
    FD_SET(self->master, &self->rfdset);
    FD_SET(self->master, &self->wfdset);
    if (pselect(MAX(self->master, self->extra) + 1, &self->rfdset, &self->wfdset, NULL, NULL,
                NULL) < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            errno = 0;
            return true;
        } else
            WRN("pselect failed: %s\n", strerror(errno));
    }

    return false;
}

ssize_t Monitor_read(Monitor* self)
{
    if (FD_ISSET(self->master, &self->rfdset)) {
        ssize_t rd = read(self->master, self->input_buffer, sizeof(self->input_buffer));
        if (rd < 0) {
            self->exit = true;
            return 0;
        }
        return rd;
    }
    return 0;
}

int Monitor_write(Monitor* self, char* buffer, size_t bytes)
{
    if (FD_ISSET(self->master, &self->wfdset)) {
        return write(self->master, buffer, bytes);
    }
    return 0;
}

void Monitor_kill(Monitor* self)
{
    if (self->pid > 1)
        kill(self->pid, SIGKILL);
    self->pid = 0;
}

void Monitor_watch_fd(Monitor* self, int fd)
{
    self->extra = fd;
}
