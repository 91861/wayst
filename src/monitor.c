#define _GNU_SOURCE

#include "monitor.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/wait.h>
#ifdef __FreeBSD__
#include <utmpx.h>
#else
#include <utmp.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "vector.h"

typedef struct
{
    pid_t    child_pid;
    Monitor* instance;
} MonitorInfo;

DEF_VECTOR(MonitorInfo, NULL)

static Vector_MonitorInfo instances;

void sighandler(int sig)
{
    pid_t p;
    int   status;
    while ((p = waitpid(-1, &status, WNOHANG)) > 1) {
        for (size_t i = 0; i < instances.size; ++i) {
            MonitorInfo* info = Vector_at_MonitorInfo(&instances, i);
            if (!info) {
                break;
            }
            if (info->child_pid == p) {
                if (status) {
                    WRN("Child process %d exited with status %d\n", p, status);
                }

                if (info->instance->callbacks.on_exit && info->instance->callbacks.user_data) {
                    info->instance->callbacks.on_exit(info->instance->callbacks.user_data);
                }

                Vector_remove_at_MonitorInfo(&instances, i, 1);
                break;
            }
        }
    }
}

Monitor Monitor_new()
{
    static bool instances_initialized = false;
    if (!instances_initialized) {
        instances_initialized = true;
        instances             = Vector_new_MonitorInfo();
    }
    Monitor self       = { 0 };
    self.extra_fd      = 0;
    self.child_is_dead = true;

    return self;
}

void Monitor_fork_new_pty(Monitor* self, uint32_t cols, uint32_t rows)
{
    ASSERT(self->callbacks.on_exit && self->callbacks.user_data,
           "exit callbacks set before forking");

    struct sigaction sigact;
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = sighandler;
    sigaction(SIGCHLD, &sigact, NULL);

    struct winsize ws = { .ws_col = cols, .ws_row = rows };
    openpty(&self->child_fd, &self->parent_fd, NULL, NULL, &ws);
    self->child_pid = fork();
    if (self->child_pid == 0) {
        close(self->child_fd);
        login_tty(self->parent_fd);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");
        setenv("COLORTERM", "truecolor", 1);
        if (settings.vte_version.str) {
            setenv("VTE_VERSION", settings.vte_version.str, 1);
        }
        setenv("TERM", settings.term.str, 1);

        if (execvp(settings.shell.str, (char**)settings.shell_argv)) {
            printf(TERMCOLOR_RED "Failed to execute command: \'%s\'.\n%s\n\narguments: ",
                   settings.shell.str,
                   strerror(errno));
            for (int i = 0; i < settings.shell_argc; ++i) {
                printf("\'%s\'%s",
                       settings.shell_argv[i],
                       i == settings.shell_argc - 1 ? "" : ", ");
            }
            puts("\nPress Ctrl-c to exit");

            for (;;) {
                pause();
            }
        }
    } else if (self->child_pid < 0) {
        ERR("Failed to fork process %s", strerror(errno));
    }
    close(self->parent_fd);
    fcntl(self->child_fd, F_SETFL, fcntl(self->child_fd, F_GETFL) | O_NONBLOCK);
    Vector_push_MonitorInfo(&instances,
                            (MonitorInfo){ .child_pid = self->child_pid, .instance = self });
    self->child_is_dead = false;
}

bool Monitor_wait(Monitor* self, int timeout)
{
    memset(self->pollfds, 0, sizeof(self->pollfds));
    self->pollfds[CHILD_FD_IDX].fd     = self->child_fd;
    self->pollfds[CHILD_FD_IDX].events = POLLIN;
    self->pollfds[EXTRA_FD_IDX].fd     = self->extra_fd;
    self->pollfds[EXTRA_FD_IDX].events = POLLIN;

    errno = 0;
    if (poll(self->pollfds, 2, timeout) < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            ERR("poll failed %s", strerror(errno));
        }
    }

    self->read_info_up_to_date = true;
    return false;
}

ssize_t Monitor_read(Monitor* self)
{
    if (unlikely(self->child_is_dead)) {
        return -1;
    }

    if (!self->read_info_up_to_date) {
        memset(self->pollfds, 0, sizeof(self->pollfds[0]));
        self->pollfds[CHILD_FD_IDX].fd     = self->child_fd;
        self->pollfds[CHILD_FD_IDX].events = POLLIN;
        if (poll(self->pollfds, 1, 0) < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                ERR("poll failed %s\n", strerror(errno));
            }
        }
        self->read_info_up_to_date = true;
    }

    if (self->pollfds[CHILD_FD_IDX].revents & POLLIN) {
        size_t  to_read = unlikely(settings.debug_vt) ? 1 : sizeof(self->input_buffer);
        ssize_t rd      = read(self->child_fd, self->input_buffer, to_read);
        if (rd < 0) {
            return -1;
        }
        self->read_info_up_to_date = false;
        return rd;
    } else {
        self->read_info_up_to_date = false;
        return -1;
    }
}

ssize_t Monitor_write(Monitor* self, char* buffer, size_t bytes)
{
    for (uint_fast8_t i = 0; i < 2; ++i) {
        ssize_t ret = write(self->child_fd, buffer, bytes);

        if (ret == -1) {
            if (likely(errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* We can't write because the client program has not read enuogh data to free up the
                 * os provided buffer. A blocking write here could potentially (if the client also
                 * doesn't check for this) deadlock the main event loop. Just give up and try next
                 * time. */
                return MONITOR_WRITE_WOULD_BLOCK;
            } else if (errno != EINTR) {
                ERR("wirte to pty failed %s\n", strerror(errno));
            }
        } else {
            return ret;
        }
    }

    WRN("write to pty interrupted\n");
    return MONITOR_WRITE_WOULD_BLOCK;
}

void Monitor_kill(Monitor* self)
{
    if (self->child_pid > 1) {
        kill(self->child_pid, SIGHUP);
    }
    self->child_pid = 0;
}

void Monitor_watch_window_system_fd(Monitor* self, int fd)
{
    self->extra_fd = fd;
}

/**
 * Ensure the child proceses are killed even if we segfault */
__attribute__((destructor)) void destructor()
{
    for (size_t i = 0; i < instances.size; ++i) {
        Monitor_kill(instances.buf[i].instance);
    }
}
