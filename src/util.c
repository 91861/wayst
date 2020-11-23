#include "util.h"

/** check string equality case insensitive */
bool strneqci(const char* restrict s1, const char* restrict s2, const size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (tolower(s1[i]) != tolower(s2[i]))
            return false;
    return true;
}

/** match string against wildcard pattern */
bool streq_wildcard(const char* restrict str, const char* restrict pattern)
{
    while (*pattern && *str)
        switch (*pattern) {
            default:
                if (*pattern != *str)
                    return false;
                /* fall through */
            case '?':
                ++pattern;
                ++str;
                break;
            case '*':
                if (pattern[1] == '\0' || pattern[1] == '*')
                    return true;
                ++pattern;
                while (*pattern && *str)
                    if (streq_wildcard(str++, pattern))
                        return true;
                return false;
        }

    size_t num_stars = 0;
    for (const char* i = pattern; *i; ++i)
        if (*i == '*')
            ++num_stars;
    return strlen(str) == (strlen(pattern) - num_stars);
}

/**
 * Get hostname. Caller should free() */
char* get_hostname()
{
    char tmp[256];
    tmp[255] = 0; /* if truncation occurs, it is unspecified whether the returned buffer includes a
                     terminating null byte. */
    if (gethostname(tmp, sizeof(tmp) - 1)) {
        WRN("Could not get hostname %s\n", strerror(errno));
        return NULL;
    } else {
        return strdup(tmp);
    }
}

int spawn_process(const char* opt_work_directory,
                  const char* command,
                  char*       opt_argv[],
                  bool        detach,
                  bool        open_pipe_to_stdin)
{
    int pipefd[2] = { 0 };

    if (open_pipe_to_stdin) {
        pipe(pipefd);
    }

    pid_t pid = fork();

    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);

        if (open_pipe_to_stdin) {
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        }
        if (opt_work_directory) {
            chdir(opt_work_directory);
        }
        if (!opt_argv) {
            opt_argv    = calloc(2, sizeof(char*));
            opt_argv[0] = strdup(command);
        }

        if (detach) {
            if (setsid() < 0) {
                _exit(EXIT_FAILURE);
            }
            pid = fork();
            if (pid > 0) {
                _exit(EXIT_SUCCESS);
            } else if (pid < 0) {
                _exit(EXIT_FAILURE);
            }
        }

        if (opt_work_directory) {
            chdir(opt_work_directory);
        }

        umask(0);

        if (execvp(command, (char**)opt_argv)) {
            WRN("failed to execute \'%s\' %s\n", command, strerror(errno));
            _exit(EXIT_FAILURE);
        }
    } else if (pid < 0) {
        WRN("failed to fork\n");

        if (open_pipe_to_stdin) {
            close(pipefd[1]);
        }
    }

    if (open_pipe_to_stdin) {
        close(pipefd[0]);
    }

    return pipefd[1];
}
