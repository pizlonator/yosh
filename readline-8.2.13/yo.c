/* yo.c -- LLM-powered shell assistant for readline */

/* Copyright (C) 2026 Epic Games, Inc.

   This file is part of the GNU Readline Library (Readline), a library
   for reading lines of text with interactive input and history editing.

   Readline is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Readline is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Readline.  If not, see <http://www.gnu.org/licenses/>.
*/

#define READLINE_LIBRARY

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <termios.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <pwd.h>
#include <errno.h>
#include <curl/curl.h>
#include <stdarg.h>

#include "readline.h"
#include "history.h"
#include "rlprivate.h"
#include "xmalloc.h"
#include "cJSON.h"
#include "yo.h"

#include <stdfil.h>

/* **************************************************************** */
/*                                                                  */
/*                        Configuration                             */
/*                                                                  */
/* **************************************************************** */

#define YO_DEFAULT_MODEL "claude-sonnet-4-5-20250929"
#define YO_DEFAULT_HISTORY_LIMIT 10
#define YO_DEFAULT_TOKEN_BUDGET 4096
#define YO_API_TIMEOUT 30L
#define YO_MAX_TOKENS 1024

/* Default color: red italics */
#define YO_DEFAULT_CHAT_COLOR "\033[3;36m"
#define YO_COLOR_RESET "\033[0m"

/* Scrollback defaults */
#define YO_DEFAULT_SCROLLBACK_LINES 1000
#define YO_DEFAULT_SCROLLBACK_BYTES (1024 * 1024)  /* 1MB */

/* **************************************************************** */
/*                                                                  */
/*                     Session Memory Types                         */
/*                                                                  */
/* **************************************************************** */

typedef struct {
    char *query;           /* "yo find python files" */
    char *response_type;   /* "command" or "chat" */
    char *response;        /* the command or chat text */
    char *tool_use_id;     /* tool_use.id from Claude's response */
    int executed;          /* 1 if user ran it, 0 if not */
    int pending;           /* 1 if response had "pending":true (multi-step) */
} yo_exchange_t;

/* **************************************************************** */
/*                                                                  */
/*                      Static Variables                            */
/*                                                                  */
/* **************************************************************** */

static int yo_is_enabled = 0;
static yo_exchange_t *yo_history = NULL;
static int yo_history_count = 0;
static int yo_history_capacity = 0;
static int yo_history_limit = YO_DEFAULT_HISTORY_LIMIT;
static int yo_token_budget = YO_DEFAULT_TOKEN_BUDGET;
static char *yo_model = NULL;
static char *yo_system_prompt = NULL;
static const char *yo_documentation = NULL;

/* Track if last command from yo was executed */
static int yo_last_was_command = 0;
static int yo_last_command_executed = 0;

/* Continuation state for multi-step command sequences */
static int yo_continuation_active = 0;     /* 1 if mid-plan (LLM returned pending:true) */
static rl_hook_func_t *yo_saved_startup_hook = NULL;  /* for chaining with bash's hook */
static char *yo_last_executed_command = NULL;  /* what the user actually ran (may differ from suggestion) */

/* **************************************************************** */
/*                                                                  */
/*                  PTY Proxy State Variables                       */
/*                                                                  */
/* **************************************************************** */

/* PTY file descriptors */
static int yo_pty_master = -1;
static int yo_pty_slave = -1;
static int yo_real_stdout = -1;      /* saved original stdout */
static int yo_real_stdin = -1;       /* saved original stdin */
static int yo_real_stderr = -1;      /* saved original stderr */

/* Saved original terminal settings for restoration on cleanup */
static struct termios yo_orig_termios;
static int yo_orig_termios_saved = 0;

/* Child shell PID (only valid in pump/parent process) */
static pid_t yo_child_pid = -1;

/* Scrollback buffer - allocated with mmap for sharing between pump and shell */
typedef struct {
    pthread_mutex_t lock;
    size_t capacity;        /* max buffer size (from YO_SCROLLBACK_BYTES) */
    size_t write_pos;       /* circular write position */
    size_t data_size;       /* current amount of data (up to capacity) */
    int max_lines;          /* max lines to track */
    char data[];            /* flexible array - data follows struct in shared memory */
} yo_scrollback_t;

static yo_scrollback_t *yo_scrollback = NULL;  /* mmap'd shared memory */
static size_t yo_scrollback_mmap_size = 0;

/* Configuration for scrollback */
static int yo_scrollback_enabled = 1;

/* Are we the pump process or the shell process? */
static int yo_is_pump = 0;

/* **************************************************************** */
/*                                                                  */
/*                    Forward Declarations                          */
/*                                                                  */
/* **************************************************************** */

static void yo_reload_config(void);
static char *yo_load_api_key(void);
static cJSON *yo_call_claude(const char *api_key, const char *query);
static cJSON *yo_call_claude_with_scrollback(const char *api_key, const char *query,
                                             const char *scrollback_request, const char *scrollback_data,
                                             const char *scrollback_tool_id);
static cJSON *yo_call_claude_with_docs(const char *api_key, const char *query, const char *docs_request,
                                       const char *docs_tool_id);
static int yo_parse_response(cJSON *tool_use, char **type, char **content, char **explanation,
                             char **tool_use_id, int *pending);
static void yo_display_chat(const char *response);
static void yo_history_add(const char *query, const char *type, const char *response, const char *tool_use_id, int executed, int pending);
static void yo_history_prune(void);
static int yo_estimate_tokens(void);
static cJSON *yo_build_messages(const char *current_query);
static cJSON *yo_build_messages_with_scrollback(const char *current_query, const char *scrollback_request,
                                                 const char *scrollback_data, const char *scrollback_tool_id);
static cJSON *yo_build_messages_with_docs(const char *current_query, const char *docs_request,
                                          const char *docs_tool_id);
static void yo_print_error_no_newlinev(const char *msg, va_list args);
static void yo_print_error_no_newline(const char *msg, ...);
static void yo_print_error(const char *msg, ...);
static void yo_print_thinking(void);
static void yo_clear_thinking(void);
static void yo_report_parse_error(cJSON *tool_use);
static const char *yo_get_chat_color(void);

/* Continuation hook and signal cleanup */
static int yo_continuation_hook(void);
static void yo_continuation_sigcleanup(int, void *);

/* Explanation retry - re-prompts LLM when command response is missing explanation */
static cJSON *yo_retry_for_explanation(const char *api_key, const char *query, cJSON *original_tool_use);

/* Request handling helpers (shared by continuation hook and accept line) */
static int yo_handle_requests(const char *api_key, const char *query,
                              cJSON **tool_use, char **type, char **content,
                              char **explanation, char **tool_use_id, int *pending, int max_turns);
static int yo_handle_explanation_retry(const char *api_key, const char *query,
                                       cJSON **tool_use, char **type, char **content,
                                       char **explanation, char **tool_use_id, int *pending);

/* PTY proxy functions */
static int yo_pty_init(void);
static void yo_pump_loop(void) __attribute__((noreturn));
static void yo_scrollback_append(const char *data, size_t len);
static void yo_scrollback_clear(void);
static void yo_forward_signal(int sig);

/* Distro detection */
static char *yo_detect_distro(void);

/* **************************************************************** */
/*                                                                  */
/*                      CURL Response Buffer                        */
/*                                                                  */
/* **************************************************************** */

typedef struct {
    char *data;
    size_t size;
} yo_response_buffer_t;

/* Self-pipe for immediate Ctrl-C response during API calls.
   When SIGINT arrives, the signal handler writes to the pipe.
   The curl multi loop select()s on both curl sockets and this pipe,
   allowing immediate cancellation. */
static int yo_sigint_pipe[2] = {-1, -1};  /* [0]=read, [1]=write */

/* Flag to track if request was cancelled by Ctrl-C */
static volatile sig_atomic_t yo_cancelled = 0;

/* SIGINT handler during API calls - writes to pipe for immediate wakeup */
static void
yo_sigint_handler(int sig)
{
    char c = 1;
    yo_cancelled = 1;
    /* Non-blocking write; if pipe full, that's fine - one byte is enough */
    (void)write(yo_sigint_pipe[1], &c, 1);
    (void)sig;
}

/* Initialize the self-pipe (called lazily on first use) */
static int
yo_init_sigint_pipe(void)
{
    int flags;

    if (yo_sigint_pipe[0] >= 0)
        return 0;  /* Already initialized */

    if (pipe(yo_sigint_pipe) < 0)
        return -1;

    /* Make write end non-blocking so signal handler never blocks */
    flags = fcntl(yo_sigint_pipe[1], F_GETFL);
    if (flags < 0)
        goto error;
    if (fcntl(yo_sigint_pipe[1], F_SETFL, flags | O_NONBLOCK) < 0)
        goto error;

    /* Also make read end non-blocking for drain operation */
    flags = fcntl(yo_sigint_pipe[0], F_GETFL);
    if (flags < 0)
        goto error;
    if (fcntl(yo_sigint_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0)
        goto error;

    return 0;

error:
    close(yo_sigint_pipe[0]);
    close(yo_sigint_pipe[1]);
    return -1;
}

/* Drain any stale bytes from the pipe before starting a request */
static void
yo_drain_sigint_pipe(void)
{
    char buf[16];
    if (yo_sigint_pipe[0] >= 0)
    {
        while (read(yo_sigint_pipe[0], buf, sizeof(buf)) > 0)
            ;
    }
    yo_cancelled = 0;
}

static size_t yo_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    yo_response_buffer_t *mem = (yo_response_buffer_t *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

/* **************************************************************** */
/*                                                                  */
/*                   Configuration Reload                           */
/*                                                                  */
/* **************************************************************** */

static void
yo_reload_config(void)
{
    const char *env_val;

    /* Reload model setting */
    if (yo_model)
    {
        free(yo_model);
        yo_model = NULL;
    }
    env_val = getenv("YO_MODEL");
    if (env_val && *env_val)
    {
        yo_model = strdup(env_val);
    }
    else
    {
        yo_model = strdup(YO_DEFAULT_MODEL);
    }

    /* Reload history limit */
    env_val = getenv("YO_HISTORY_LIMIT");
    if (env_val && *env_val)
    {
        yo_history_limit = atoi(env_val);
        if (yo_history_limit < 1)
            yo_history_limit = YO_DEFAULT_HISTORY_LIMIT;
    }
    else
    {
        yo_history_limit = YO_DEFAULT_HISTORY_LIMIT;
    }

    /* Reload token budget */
    env_val = getenv("YO_TOKEN_BUDGET");
    if (env_val && *env_val)
    {
        yo_token_budget = atoi(env_val);
        if (yo_token_budget < 100)
            yo_token_budget = YO_DEFAULT_TOKEN_BUDGET;
    }
    else
    {
        yo_token_budget = YO_DEFAULT_TOKEN_BUDGET;
    }
}

/* **************************************************************** */
/*                                                                  */
/*                   PTY Proxy Implementation                       */
/*                                                                  */
/* **************************************************************** */

/* Append data to the scrollback buffer (called from pump process) */
static void
yo_scrollback_append(const char *data, size_t len)
{
    if (!yo_scrollback || len == 0)
        return;

    pthread_mutex_lock(&yo_scrollback->lock);

    /* Write data to circular buffer */
    size_t capacity = yo_scrollback->capacity;
    size_t write_pos = yo_scrollback->write_pos;

    for (size_t i = 0; i < len; i++)
    {
        yo_scrollback->data[write_pos] = data[i];
        write_pos = (write_pos + 1) % capacity;
    }

    yo_scrollback->write_pos = write_pos;
    yo_scrollback->data_size += len;
    if (yo_scrollback->data_size > capacity)
        yo_scrollback->data_size = capacity;

    pthread_mutex_unlock(&yo_scrollback->lock);
}

/* Clear the scrollback buffer */
static void
yo_scrollback_clear(void)
{
    if (!yo_scrollback)
        return;

    pthread_mutex_lock(&yo_scrollback->lock);
    yo_scrollback->write_pos = 0;
    yo_scrollback->data_size = 0;
    pthread_mutex_unlock(&yo_scrollback->lock);
}

/* Signal handler to forward signals to child shell process */
static void
yo_forward_signal(int sig)
{
    if (yo_child_pid > 0)
        kill(yo_child_pid, sig);
}

/* SIGWINCH handler for pump - propagate window size to PTY and forward to child */
static void
yo_pump_sigwinch_handler(int sig)
{
    struct winsize ws;

    if (yo_pty_master >= 0 && yo_real_stdout >= 0)
    {
        /* Get current terminal size from real terminal */
        if (ioctl(yo_real_stdout, TIOCGWINSZ, &ws) == 0)
        {
            /* Propagate to PTY master */
            (void)ioctl(yo_pty_master, TIOCSWINSZ, &ws);
        }
    }

    /* Forward to child */
    yo_forward_signal(sig);
}

/* Helper to write all bytes, handling EINTR and partial writes.
   Returns 0 on success, -1 on error. */
static int
yo_write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;
    while (written < len)
    {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        written += n;
    }
    return 0;
}

/* The pump loop - runs in the parent process, forwards I/O and waits for child */
static void
yo_pump_loop(void)
{
    struct pollfd fds[2];
    char buf[4096];
    ssize_t n;
    int status = 0;
    int error_exit = 0;  /* If set, exit with 1 regardless of child status */
    struct sigaction sa;
    pid_t wpid;

    /* Set up signal forwarding for common signals */
    sa.sa_handler = yo_forward_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    /* SIGWINCH needs special handling - propagate window size too */
    sa.sa_handler = yo_pump_sigwinch_handler;
    sigaction(SIGWINCH, &sa, NULL);

    /* Set up poll fds:
       [0] = real stdin (read input from user)
       [1] = PTY master (read output from shell)
    */
    fds[0].fd = yo_real_stdin;
    fds[0].events = POLLIN;
    fds[1].fd = yo_pty_master;
    fds[1].events = POLLIN;

    for (;;)
    {
        int ret;

        /* Check if child is still alive */
        wpid = waitpid(yo_child_pid, &status, WNOHANG);
        if (wpid < 0)
        {
            if (errno == EINTR)
                continue;
            goto error;
        }
        if (wpid > 0)
        {
            /* Child exited - drain any remaining output */
            for (;;)
            {
                n = read(yo_pty_master, buf, sizeof(buf));
                if (n > 0)
                {
                    if (yo_write_all(yo_real_stdout, buf, n) < 0)
                        break;  /* Write error during drain, stop draining */
                    yo_scrollback_append(buf, n);
                }
                else if (n == 0)
                {
                    break;  /* EOF */
                }
                else if (errno != EINTR)
                {
                    break;  /* Read error during drain */
                }
            }
            goto cleanup;
        }

        ret = poll(fds, 2, 100);  /* 100ms timeout to check child status */

        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            goto error;
        }

        if (ret == 0)
            continue;  /* Timeout, check child status */

        /* Forward input from real stdin to PTY master */
        if (fds[0].revents & POLLIN)
        {
            n = read(yo_real_stdin, buf, sizeof(buf));
            if (n > 0)
            {
                if (yo_write_all(yo_pty_master, buf, n) < 0)
                    goto error;
            }
            else if (n == 0)
            {
                /* EOF on stdin */
                goto wait_child;
            }
            else if (errno != EINTR)
            {
                goto error;
            }
        }

        /* Forward output from PTY master to real stdout and scrollback */
        if (fds[1].revents & POLLIN)
        {
            n = read(yo_pty_master, buf, sizeof(buf));
            if (n > 0)
            {
                if (yo_write_all(yo_real_stdout, buf, n) < 0)
                    goto error;
                yo_scrollback_append(buf, n);
            }
            else if (n == 0)
            {
                /* PTY closed - shell exited */
                goto wait_child;
            }
            else if (errno != EINTR)
            {
                goto error;
            }
        }

        /* Check for hangup/error on PTY */
        if ((fds[1].revents & (POLLHUP | POLLERR)) && !(fds[1].revents & POLLIN))
            goto wait_child;
    }

error:
    /* Fatal error - kill child and wait for it */
    error_exit = 1;
    kill(yo_child_pid, SIGTERM);
    /* Fall through to wait_child */

wait_child:
    /* Wait for child to fully exit, handling EINTR */
    while (waitpid(yo_child_pid, &status, 0) < 0 && errno == EINTR)
        ;
    /* Fall through to cleanup */

cleanup:
    /* Restore terminal settings */
    if (yo_orig_termios_saved)
        tcsetattr(yo_real_stdin, TCSANOW, &yo_orig_termios);

    /* Exit with appropriate status */
    if (error_exit)
        _exit(1);
    else if (WIFEXITED(status))
        _exit(WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        _exit(128 + WTERMSIG(status));
    else
        _exit(1);
}

/* Initialize PTY proxy - called from rl_yo_enable()
   This function forks: parent becomes the I/O pump, child becomes the shell.
   Returns 0 on success (in the child/shell process).
   The parent never returns - it runs the pump loop and exits. */
static int
yo_pty_init(void)
{
    struct winsize ws;
    struct termios term;
    const char *env_val;
    size_t scrollback_bytes = YO_DEFAULT_SCROLLBACK_BYTES;
    int scrollback_lines = YO_DEFAULT_SCROLLBACK_LINES;
    pthread_mutexattr_t mutex_attr;
    pid_t pid;

    /* Check if scrollback is disabled */
    env_val = getenv("YO_SCROLLBACK_ENABLED");
    if (env_val && *env_val == '0')
    {
        yo_scrollback_enabled = 0;
        return 0;  /* Not an error, just disabled */
    }

    /* Check if stdin is a terminal */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    {
        yo_scrollback_enabled = 0;
        return 0;  /* Not a terminal, can't use PTY */
    }

    /* Read scrollback configuration */
    env_val = getenv("YO_SCROLLBACK_BYTES");
    if (env_val && *env_val)
    {
        long val = atol(env_val);
        if (val > 0)
            scrollback_bytes = (size_t)val;
    }

    env_val = getenv("YO_SCROLLBACK_LINES");
    if (env_val && *env_val)
    {
        int val = atoi(env_val);
        if (val > 0)
            scrollback_lines = val;
    }

    /* Save original terminal settings */
    if (tcgetattr(STDIN_FILENO, &yo_orig_termios) == 0)
        yo_orig_termios_saved = 1;

    /* Get current terminal size */
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0)
    {
        ws.ws_row = 24;
        ws.ws_col = 80;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
    }

    /* Get current terminal settings for PTY */
    if (tcgetattr(STDIN_FILENO, &term) < 0)
        goto fail;

    /* Create PTY pair */
    if (openpty(&yo_pty_master, &yo_pty_slave, NULL, &term, &ws) < 0)
        goto fail;

    /* Allocate scrollback buffer in shared memory (accessible by both pump and shell) */
    yo_scrollback_mmap_size = sizeof(yo_scrollback_t) + scrollback_bytes;
    yo_scrollback = mmap(NULL, yo_scrollback_mmap_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (yo_scrollback == MAP_FAILED)
    {
        yo_scrollback = NULL;
        goto fail_pty;
    }

    /* Initialize scrollback structure */
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&yo_scrollback->lock, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    yo_scrollback->capacity = scrollback_bytes;
    yo_scrollback->max_lines = scrollback_lines;
    yo_scrollback->write_pos = 0;
    yo_scrollback->data_size = 0;

    /* Fork: parent becomes pump, child becomes shell */
    pid = fork();
    if (pid < 0)
        goto fail_scrollback;

    if (pid > 0)
    {
        /* Parent process - becomes the I/O pump */
        yo_is_pump = 1;
        yo_child_pid = pid;

        /* Save original FDs for pump to use */
        yo_real_stdin = dup(STDIN_FILENO);
        yo_real_stdout = dup(STDOUT_FILENO);
        yo_real_stderr = dup(STDERR_FILENO);

        /* Close PTY slave in parent - only child uses it */
        close(yo_pty_slave);
        yo_pty_slave = -1;

        /* Put the real terminal into raw mode for the pump */
        {
            struct termios raw_term = yo_orig_termios;
            cfmakeraw(&raw_term);
            tcsetattr(yo_real_stdin, TCSANOW, &raw_term);
        }

        /* Run the pump loop - this never returns */
        yo_pump_loop();
        /* NOTREACHED */
    }

    /* Child process - becomes the shell */
    yo_is_pump = 0;
    yo_child_pid = -1;

    /* Close PTY master in child - only pump uses it */
    close(yo_pty_master);
    yo_pty_master = -1;

    /* Create new session so we can set controlling terminal */
    if (setsid() < 0)
        goto fail_scrollback;

    /* Redirect stdin/stdout/stderr to PTY slave */
    if (dup2(yo_pty_slave, STDIN_FILENO) < 0 ||
        dup2(yo_pty_slave, STDOUT_FILENO) < 0 ||
        dup2(yo_pty_slave, STDERR_FILENO) < 0)
        goto fail_scrollback;

    /* Close the extra slave FD - we have it on stdin/stdout/stderr now */
    close(yo_pty_slave);
    yo_pty_slave = -1;

    /* Make the PTY slave our controlling terminal */
    if (ioctl(STDIN_FILENO, TIOCSCTTY, 0) < 0)
        goto fail;

    yo_scrollback_enabled = 1;
    return 0;

fail_scrollback:
    if (yo_scrollback)
    {
        munmap(yo_scrollback, yo_scrollback_mmap_size);
        yo_scrollback = NULL;
    }

fail_pty:
    if (yo_pty_master >= 0)
        close(yo_pty_master);
    if (yo_pty_slave >= 0)
        close(yo_pty_slave);
    yo_pty_master = yo_pty_slave = -1;

fail:
    yo_scrollback_enabled = 0;
    return -1;
}

/* Get scrollback text - returns malloc'd string, caller must free.
   Returns up to max_lines lines from the end of the scrollback buffer.
   ANSI escape sequences are stripped for LLM readability. */
char *
rl_yo_get_scrollback(int max_lines)
{
    char *result = NULL;
    char *raw_data = NULL;
    size_t raw_size;
    int line_count;
    char *p, *start, *out;
    size_t out_size;

    if (!yo_scrollback_enabled || !yo_scrollback || yo_is_pump)
        return strdup("");

    if (max_lines <= 0)
        max_lines = yo_scrollback->max_lines;

    pthread_mutex_lock(&yo_scrollback->lock);

    if (yo_scrollback->data_size == 0)
    {
        pthread_mutex_unlock(&yo_scrollback->lock);
        return strdup("");
    }

    /* Extract data from circular buffer into linear buffer */
    raw_size = yo_scrollback->data_size;
    raw_data = malloc(raw_size + 1);
    if (!raw_data)
    {
        pthread_mutex_unlock(&yo_scrollback->lock);
        return strdup("");
    }

    if (yo_scrollback->data_size < yo_scrollback->capacity)
    {
        /* Buffer hasn't wrapped yet - data starts at 0 */
        memcpy(raw_data, yo_scrollback->data, raw_size);
    }
    else
    {
        /* Buffer has wrapped - data starts at write_pos */
        size_t first_part = yo_scrollback->capacity - yo_scrollback->write_pos;
        memcpy(raw_data, yo_scrollback->data + yo_scrollback->write_pos, first_part);
        memcpy(raw_data + first_part, yo_scrollback->data, yo_scrollback->write_pos);
    }
    raw_data[raw_size] = '\0';

    pthread_mutex_unlock(&yo_scrollback->lock);

    /* Count lines from end and find start position */
    line_count = 0;
    start = raw_data + raw_size;
    for (p = raw_data + raw_size - 1; p >= raw_data && line_count < max_lines; p--)
    {
        if (*p == '\n')
        {
            line_count++;
            if (line_count < max_lines)
                start = p + 1;
            else
                start = p + 1;  /* Don't include the newline before our start */
        }
    }
    if (line_count < max_lines && p < raw_data)
        start = raw_data;

    /* Allocate output buffer (same size is safe upper bound after stripping) */
    out_size = raw_size - (start - raw_data);
    result = malloc(out_size + 1);
    if (!result)
    {
        free(raw_data);
        return strdup("");
    }

    /* Copy while stripping ANSI escape sequences */
    out = result;
    for (p = start; *p; p++)
    {
        if (*p == '\033')
        {
            /* Skip ESC [ ... (letter) sequences */
            if (*(p + 1) == '[')
            {
                p += 2;
                while (*p && !(*p >= 'A' && *p <= 'Z') && !(*p >= 'a' && *p <= 'z'))
                    p++;
                if (!*p)
                    break;
                continue;  /* Skip the final letter */
            }
            /* Skip other ESC sequences (ESC followed by one char) */
            if (*(p + 1))
            {
                p++;
                continue;
            }
        }
        *out++ = *p;
    }
    *out = '\0';

    free(raw_data);
    return result;
}

/* **************************************************************** */
/*                                                                  */
/*                    Public API Functions                          */
/*                                                                  */
/* **************************************************************** */

/* Detect Linux distribution name and version by reading /etc/os-release.
   Returns a malloc'd string like "Ubuntu 22.04.3 LTS" or NULL if unknown.
   Caller may leak the result (Fil-C style). */
static char *
yo_detect_distro(void)
{
    FILE *fp;
    char line[512];
    char *pretty_name = NULL;
    char *name = NULL;
    char *version = NULL;
    char *result;

    fp = fopen("/etc/os-release", "r");
    if (!fp)
        return NULL;

    while (fgets(line, sizeof(line), fp))
    {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (strncmp(line, "PRETTY_NAME=", 12) == 0)
        {
            char *val = line + 12;
            /* Strip surrounding quotes */
            if (*val == '"')
            {
                val++;
                char *end = strrchr(val, '"');
                if (end)
                    *end = '\0';
            }
            pretty_name = strdup(val);
        }
        else if (strncmp(line, "NAME=", 5) == 0)
        {
            char *val = line + 5;
            if (*val == '"')
            {
                val++;
                char *end = strrchr(val, '"');
                if (end)
                    *end = '\0';
            }
            name = strdup(val);
        }
        else if (strncmp(line, "VERSION=", 8) == 0)
        {
            char *val = line + 8;
            if (*val == '"')
            {
                val++;
                char *end = strrchr(val, '"');
                if (end)
                    *end = '\0';
            }
            version = strdup(val);
        }
    }

    fclose(fp);

    if (pretty_name && *pretty_name)
    {
        result = pretty_name;
    }
    else if (name && *name)
    {
        if (version && *version)
            asprintf(&result, "%s %s", name, version);
        else
            result = name;
    }
    else
    {
        result = NULL;
    }

    return result;
}

void
rl_yo_enable(const char *system_prompt, const char *documentation)
{
    if (yo_is_enabled)
        return;

    /* Initialize PTY proxy for scrollback capture (optional - may fail silently) */
    yo_pty_init();

    /* Store documentation from caller */
    yo_documentation = documentation;

    /* Store system prompt from caller. Tool definitions handle the response format;
       the system prompt provides behavioral guidance. */
    asprintf(
        &yo_system_prompt,
        "%s\n"
        "\n"
        "You have four tools available. Choose the most appropriate one:\n"
        "\n"
        "- command: Generate a shell command for the user to review and execute. Always provide\n"
        "  a brief explanation. You will not see the output unless you request it.\n"
        "  Prefer short, focused commands. For multi-step tasks, set pending=true and you'll\n"
        "  receive terminal output after execution to continue with the next step.\n"
        "  If a task requires a command, you MUST use this tool - never describe a command\n"
        "  in a chat response instead of providing it as an actual command.\n"
        "\n"
        "- chat: Respond with text ONLY when no command is needed (pure questions,\n"
        "  explanations, or conversational replies). Never use chat to suggest a command.\n"
        "\n"
        "- scrollback: Request recent terminal output when you need to see what happened\n"
        "  (errors, command results, etc.). You'll get another turn to respond after.\n"
        "  Note: scrollback captures raw terminal I/O, so it may contain duplicate or\n"
        "  garbled-looking lines from readline editing (e.g. the user pressing up/down\n"
        "  arrows to navigate history). Ignore these artifacts and focus on actual output.\n"
        "\n"
        "- docs: Request yosh documentation when the user asks about yosh features,\n"
        "  configuration, environment variables, or usage.\n"
        "\n"
        "Multi-step sequences: When you set pending=true on a command, you'll receive a\n"
        "[continuation] message with terminal output after the user executes it. Continue\n"
        "with the next command or use chat to wrap up. If the user edited the command\n"
        "substantially, acknowledge and wrap up with chat (don't continue the sequence).\n"
        "The last command in a sequence should NOT have pending=true.",
        system_prompt);

    /* Detect distro and append to system prompt if available */
    {
        char *distro = yo_detect_distro();
        if (distro && *distro)
            asprintf(&yo_system_prompt, "%s\nThe user is running %s.", yo_system_prompt, distro);
    }

    /* Bind Enter key to our yo-aware accept-line */
    rl_bind_key('\n', rl_yo_accept_line);
    rl_bind_key('\r', rl_yo_accept_line);

    yo_is_enabled = 1;
}

int
rl_yo_enabled(void)
{
    return yo_is_enabled;
}

void
rl_yo_clear_history(void)
{
    int i;

    for (i = 0; i < yo_history_count; i++)
    {
        if (yo_history[i].query)
            free(yo_history[i].query);
        if (yo_history[i].response_type)
            free(yo_history[i].response_type);
        if (yo_history[i].response)
            free(yo_history[i].response);
        if (yo_history[i].tool_use_id)
            free(yo_history[i].tool_use_id);
    }

    if (yo_history)
    {
        free(yo_history);
        yo_history = NULL;
    }

    yo_history_count = 0;
    yo_history_capacity = 0;
}

/* **************************************************************** */
/*                                                                  */
/*                  Continuation Hook (Multi-Step)                  */
/*                                                                  */
/* **************************************************************** */

/* Signal cleanup: called by readline's signal handler when Ctrl-C is
   pressed during line editing.  Clears continuation state so the user
   isn't surprised by a hook firing on the next prompt. */
static void
yo_continuation_sigcleanup(int sig, void *arg)
{
    if (sig == SIGINT)
    {
        yo_continuation_active = 0;
        yo_last_was_command = 0;
    }
}

/* Install the sigcleanup hook.  Called whenever we set up continuation
   state (pending command prefilled in the prompt).  The hook is one-shot:
   readline clears _rl_sigcleanup after it fires. */
static void
yo_install_continuation_sigcleanup(void)
{
    _rl_sigcleanup = yo_continuation_sigcleanup;
    _rl_sigcleanarg = NULL;
}

/* **************************************************************** */
/*                                                                  */
/*            Request Handling & Explanation-Retry Helpers          */
/*                                                                  */
/* **************************************************************** */

/* Process scrollback and docs requests in a loop until a final response is
   received or max_turns is exhausted.  Updates tool_use/type/content/explanation/
   tool_use_id/pending in place.  Returns 1 on success, 0 on failure (error already
   printed, *tool_use set to NULL). */
static int
yo_handle_requests(const char *api_key, const char *query,
                   cJSON **tool_use, char **type, char **content,
                   char **explanation, char **tool_use_id, int *pending, int max_turns)
{
    while (max_turns > 0)
    {
        if (strcmp(*type, "scrollback") == 0)
        {
            char *scrollback_data = NULL;
            char *saved_tool_id = NULL;
            char *saved_content = NULL;
            int lines_requested;

            /* Save the tool_use_id and content (lines count) before freeing */
            saved_tool_id = *tool_use_id;
            *tool_use_id = NULL;
            saved_content = *content;
            *content = NULL;

            lines_requested = atoi(saved_content);
            if (lines_requested <= 0) lines_requested = 50;
            if (lines_requested > 1000) lines_requested = 1000;

            scrollback_data = rl_yo_get_scrollback(lines_requested);
            if (!scrollback_data || !*scrollback_data)
            {
                if (scrollback_data) free(scrollback_data);
                scrollback_data = strdup("(No terminal output available)");
            }

            free(*type); *type = NULL;
            if (*explanation) { free(*explanation); *explanation = NULL; }
            cJSON_Delete(*tool_use); *tool_use = NULL;

            *tool_use = yo_call_claude_with_scrollback(api_key, query,
                                                       saved_content, scrollback_data, saved_tool_id);
            free(saved_tool_id);
            free(saved_content);
            free(scrollback_data);

            if (!*tool_use)
                return 0;

            if (!yo_parse_response(*tool_use, type, content, explanation, tool_use_id, pending))
            {
                yo_report_parse_error(*tool_use);
                cJSON_Delete(*tool_use);
                *tool_use = NULL;
                return 0;
            }

            max_turns--;
        }
        else if (strcmp(*type, "docs") == 0)
        {
            char *saved_tool_id = NULL;

            /* Save the tool_use_id before freeing */
            saved_tool_id = *tool_use_id;
            *tool_use_id = NULL;

            free(*type); *type = NULL;
            free(*content); *content = NULL;
            if (*explanation) { free(*explanation); *explanation = NULL; }
            cJSON_Delete(*tool_use); *tool_use = NULL;

            *tool_use = yo_call_claude_with_docs(api_key, query, "", saved_tool_id);
            free(saved_tool_id);

            if (!*tool_use)
                return 0;

            if (!yo_parse_response(*tool_use, type, content, explanation, tool_use_id, pending))
            {
                yo_report_parse_error(*tool_use);
                cJSON_Delete(*tool_use);
                *tool_use = NULL;
                return 0;
            }

            max_turns--;
        }
        else
        {
            /* Not a request type - we're done */
            break;
        }
    }

    return 1;
}

/* When a command response is missing the explanation field, retry once asking
   the LLM to include it.  Updates tool_use/type/content/explanation/tool_use_id/pending
   in place if the retry succeeds.
   Returns 1 to continue normally, 0 if the user cancelled (caller should abort). */
static int
yo_handle_explanation_retry(const char *api_key, const char *query,
                            cJSON **tool_use, char **type, char **content,
                            char **explanation, char **tool_use_id, int *pending)
{
    cJSON *retry_tool_use;

    if (strcmp(*type, "command") != 0 || (*explanation && **explanation))
        return 1;  /* No retry needed */

    retry_tool_use = yo_retry_for_explanation(api_key, query, *tool_use);
    if (retry_tool_use)
    {
        char *r_type = NULL, *r_content = NULL, *r_explanation = NULL, *r_tool_use_id = NULL;
        int r_pending = 0;
        if (yo_parse_response(retry_tool_use, &r_type, &r_content, &r_explanation, &r_tool_use_id, &r_pending)
            && r_type && strcmp(r_type, "command") == 0
            && r_explanation && *r_explanation)
        {
            /* Retry succeeded — use the new response */
            free(*type); *type = r_type;
            free(*content); *content = r_content;
            if (*explanation) free(*explanation);
            *explanation = r_explanation;
            if (*tool_use_id) free(*tool_use_id);
            *tool_use_id = r_tool_use_id;
            *pending = r_pending;
            cJSON_Delete(*tool_use); *tool_use = retry_tool_use;
        }
        else
        {
            /* Retry didn't produce a valid command with explanation — use original */
            if (r_type) free(r_type);
            if (r_content) free(r_content);
            if (r_explanation) free(r_explanation);
            if (r_tool_use_id) free(r_tool_use_id);
            cJSON_Delete(retry_tool_use);
        }
    }
    else if (yo_cancelled)
    {
        return 0;  /* Cancelled */
    }

    return 1;
}

/* One-shot rl_startup_hook that fires on the next readline() call after
   the user executes a pending command.  Grabs scrollback, calls Claude
   with a synthetic [continuation] query, and prefills the next command
   (or displays a chat response). */
static int
yo_continuation_hook(void)
{
    char *api_key = NULL;
    char *scrollback = NULL;
    char *cont_query = NULL;
    cJSON *tool_use = NULL;
    char *type = NULL;
    char *content = NULL;
    char *explanation = NULL;
    char *tool_use_id = NULL;
    int pending = 0;

    /* One-shot: uninstall ourselves immediately, restore previous hook */
    rl_startup_hook = yo_saved_startup_hook;
    yo_saved_startup_hook = NULL;

    /* Safety check */
    if (!yo_continuation_active)
        return 0;

    yo_print_thinking();

    /* Load API key */
    api_key = yo_load_api_key();
    if (!api_key)
    {
        yo_clear_thinking();
        yo_continuation_active = 0;
        return 0;
    }

    /* Grab scrollback (limit to 200 lines for continuation) */
    scrollback = rl_yo_get_scrollback(200);
    if (!scrollback || !*scrollback)
    {
        if (scrollback) free(scrollback);
        scrollback = strdup("(no output)");
    }

    /* Build continuation query, noting if the user edited the command */
    {
        const char *suggested = (yo_history_count > 0) ? yo_history[yo_history_count - 1].response : NULL;
        int edited = (suggested && yo_last_executed_command &&
                      strcmp(suggested, yo_last_executed_command) != 0);

        if (edited)
            asprintf(&cont_query,
                     "[continuation] You suggested: %s\n"
                     "The user edited and executed: %s\n"
                     "Here is the terminal output:\n```\n%s\n```",
                     suggested, yo_last_executed_command, scrollback);
        else
            asprintf(&cont_query,
                     "[continuation] The user executed the previous command. "
                     "Here is the terminal output:\n```\n%s\n```",
                     scrollback);
    }
    free(scrollback);
    if (yo_last_executed_command)
    {
        free(yo_last_executed_command);
        yo_last_executed_command = NULL;
    }

    if (!cont_query)
    {
        yo_clear_thinking();
        yo_continuation_active = 0;
        free(api_key);
        return 0;
    }

    /* Call Claude API */
    tool_use = yo_call_claude(api_key, cont_query);
    if (!tool_use)
    {
        /* Error/cancellation — already printed by yo_call_claude */
        yo_continuation_active = 0;
        free(api_key);
        free(cont_query);
        return 0;
    }

    /* Parse response */
    if (!yo_parse_response(tool_use, &type, &content, &explanation, &tool_use_id, &pending))
    {
        yo_report_parse_error(tool_use);
        yo_continuation_active = 0;
        cJSON_Delete(tool_use);
        free(api_key);
        free(cont_query);
        return 0;
    }

    /* Handle scrollback requests */
    if (!yo_handle_requests(api_key, cont_query, &tool_use, &type, &content,
                            &explanation, &tool_use_id, &pending, 3))
    {
        yo_continuation_active = 0;
        free(api_key);
        free(cont_query);
        return 0;
    }

    /* If command response is missing explanation, retry once asking for it */
    if (!yo_handle_explanation_retry(api_key, cont_query, &tool_use, &type, &content,
                                     &explanation, &tool_use_id, &pending))
    {
        /* User cancelled during retry */
        yo_continuation_active = 0;
        cJSON_Delete(tool_use);
        free(api_key);
        free(cont_query);
        if (type) free(type);
        if (content) free(content);
        if (explanation) free(explanation);
        if (tool_use_id) free(tool_use_id);
        return 0;
    }

    /* Clear thinking indicator */
    yo_clear_thinking();

    /* Handle the response */
    if (strcmp(type, "command") == 0)
    {
        if (explanation && *explanation)
            yo_display_chat(explanation);

        /* Add continuation exchange to session history */
        yo_history_add(cont_query, type, content, tool_use_id, 0, pending);

        /* Prefill the command */
        rl_replace_line(content, 0);
        rl_point = rl_end;
        yo_last_was_command = 1;

        /* Continue or finish? */
        yo_continuation_active = pending;
        if (pending)
            yo_install_continuation_sigcleanup();
    }
    else if (strcmp(type, "chat") == 0)
    {
        yo_display_chat(content);
        yo_history_add(cont_query, type, content, tool_use_id, 1, 0);
        rl_replace_line("", 0);
        yo_continuation_active = 0;
    }
    else
    {
        /* Unknown type or exceeded scrollback turns */
        rl_replace_line("", 0);
        yo_continuation_active = 0;
    }

    /* Cleanup */
    cJSON_Delete(tool_use);
    free(api_key);
    free(cont_query);
    if (type) free(type);
    if (content) free(content);
    if (explanation) free(explanation);
    if (tool_use_id) free(tool_use_id);

    return 0;
}

/* **************************************************************** */
/*                                                                  */
/*                   Main Accept Line Handler                       */
/*                                                                  */
/* **************************************************************** */

int
rl_yo_accept_line(int count, int key)
{
    char *api_key = NULL;
    cJSON *tool_use = NULL;
    char *type = NULL;
    char *content = NULL;
    char *explanation = NULL;
    char *tool_use_id = NULL;
    char *saved_query = NULL;
    int pending = 0;

    /* Track if previous yo command was executed */
    if (yo_last_was_command)
    {
        /* Check if we're executing the command (line wasn't modified to start with "yo ") */
        if (rl_line_buffer && strncmp(rl_line_buffer, "yo ", 3) != 0)
        {
            /* User is executing the command (or something else) */
            if (yo_history_count > 0)
                yo_history[yo_history_count - 1].executed = 1;

            /* If continuation is active, install startup hook for next prompt */
            if (yo_continuation_active && rl_line_buffer[0] != '\0')
            {
                /* Save what the user actually executed (may differ from suggestion) */
                if (yo_last_executed_command)
                    free(yo_last_executed_command);
                yo_last_executed_command = strdup(rl_line_buffer);

                yo_saved_startup_hook = rl_startup_hook;
                rl_startup_hook = yo_continuation_hook;
            }
            else
            {
                /* Empty line = user cancelled continuation */
                yo_continuation_active = 0;
            }
        }
        else
        {
            /* User typed a new "yo " query — cancel any continuation */
            yo_continuation_active = 0;
        }
        yo_last_was_command = 0;
    }

    /* Check if input starts with "yo " */
    if (!rl_line_buffer || strncmp(rl_line_buffer, "yo ", 3) != 0)
    {
        /* Not a yo command, use normal accept-line */
        return rl_newline(count, key);
    }

    /* It's a yo command - process it */

    /* Handle "yo reset" — clear LLM context without calling the API */
    if (strcmp(rl_line_buffer, "yo reset") == 0)
    {
        rl_crlf();
        rl_yo_clear_history();
        yo_scrollback_clear();
        yo_continuation_active = 0;
        yo_last_was_command = 0;
        fprintf(rl_outstream, "%sContext reset%s\n", yo_get_chat_color(), YO_COLOR_RESET);
        fflush(rl_outstream);
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        return 0;
    }

    /* Reload config from environment (allows mid-session changes) */
    yo_reload_config();

    /* Save the query for potential follow-up calls */
    saved_query = strdup(rl_line_buffer);

    /* Add the yo command itself to shell history, then reset history state
       so UP arrow finds this entry and any saved line state is cleared. */
    add_history(saved_query);
    _rl_start_using_history();

    /* Load API key fresh each time */
    api_key = yo_load_api_key();
    if (!api_key)
    {
        /* Error already printed by yo_load_api_key */
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        free(saved_query);
        return 0;
    }

    /* Show thinking indicator after a newline */
    fprintf(rl_outstream, "\n");
    yo_print_thinking();

    /* Call Claude API - handles its own error/cancellation messages */
    tool_use = yo_call_claude(api_key, saved_query);

    if (!tool_use)
    {
        /* Error already printed by yo_call_claude */
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        free(api_key);
        free(saved_query);
        return 0;
    }

    /* Parse the response */
    if (!yo_parse_response(tool_use, &type, &content, &explanation, &tool_use_id, &pending))
    {
        yo_report_parse_error(tool_use);
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        cJSON_Delete(tool_use);
        free(api_key);
        free(saved_query);
        return 0;
    }

    /* Handle scrollback requests with follow-up calls */
    if (!yo_handle_requests(api_key, saved_query, &tool_use, &type, &content,
                            &explanation, &tool_use_id, &pending, 3))
    {
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        free(api_key);
        free(saved_query);
        return 0;
    }

    /* If command response is missing explanation, retry once asking for it */
    if (pending && !yo_handle_explanation_retry(api_key, saved_query, &tool_use, &type, &content,
                                                &explanation, &tool_use_id, &pending))
    {
        /* User cancelled during retry */
        cJSON_Delete(tool_use);
        free(api_key);
        free(saved_query);
        free(type);
        free(content);
        if (explanation) free(explanation);
        if (tool_use_id) free(tool_use_id);
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        return 0;
    }

    cJSON_Delete(tool_use);
    free(api_key);

    /* Clear thinking indicator on success */
    yo_clear_thinking();

    if (strcmp(type, "command") == 0)
    {
        /* Command mode: replace input with generated command */

        /* Print explanation if present */
        if (explanation && *explanation)
        {
            yo_display_chat(explanation);
        }

        /* Add to session history (not executed yet) */
        yo_history_add(saved_query, type, content, tool_use_id, 0, pending);

        /* Replace line with the command */
        rl_replace_line(content, 0);
        rl_point = rl_end;

        /* Mark that we just generated a command */
        yo_last_was_command = 1;

        /* Set continuation state if response is pending */
        yo_continuation_active = pending;
        if (pending)
            yo_install_continuation_sigcleanup();

        /* Redisplay with new content */
        rl_on_new_line();
        rl_redisplay();
    }
    else if (strcmp(type, "chat") == 0)
    {
        /* Chat mode: display response, return to fresh prompt */
        yo_display_chat(content);

        /* Add to session history */
        yo_history_add(saved_query, type, content, tool_use_id, 1, 0);

        /* Clear any active continuation */
        yo_continuation_active = 0;

        /* Clear the line and show fresh prompt */
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
    }
    else if (strcmp(type, "scrollback") == 0)
    {
        /* Exceeded max scrollback turns */
        yo_print_error("Too many scrollback requests");
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
    }
    else
    {
        yo_print_error("Unknown response type from Claude (full tool use response: %s)",
                       cJSON_PrintUnformatted(tool_use));
    }

    free(saved_query);
    if (type)
        free(type);
    if (content)
        free(content);
    if (explanation)
        free(explanation);
    if (tool_use_id)
        free(tool_use_id);

    return 0;
}

/* **************************************************************** */
/*                                                                  */
/*                    API Key Management                            */
/*                                                                  */
/* **************************************************************** */

static char *
yo_load_api_key(void)
{
    char *home;
    char path[1024];
    struct stat st;
    FILE *fp;
    char *key;
    size_t len;
    char *end;

    /* Get home directory */
    home = getenv("HOME");
    if (!home)
    {
        struct passwd *pw = getpwuid(getuid());
        if (pw)
            home = pw->pw_dir;
    }

    if (!home)
    {
        yo_print_error("Cannot determine home directory");
        return NULL;
    }

    snprintf(path, sizeof(path), "%s/.yoshkey", home);

    /* Check if file exists */
    if (stat(path, &st) != 0)
    {
        yo_print_error("Create ~/.yoshkey with your Anthropic API key (mode 0600)");
        return NULL;
    }

    /* Check permissions - must be 0600 */
    if ((st.st_mode & 0777) != 0600)
    {
        yo_print_error("~/.yoshkey must have mode 0600 (current: %04o)", st.st_mode & 0777);
        return NULL;
    }

    /* Read the key */
    fp = fopen(path, "r");
    if (!fp)
    {
        yo_print_error("Cannot read ~/.yoshkey: %s", strerror(errno));
        return NULL;
    }

    key = malloc(256);

    if (!fgets(key, 256, fp))
    {
        fclose(fp);
        free(key);
        yo_print_error("~/.yoshkey is empty");
        return NULL;
    }

    fclose(fp);

    /* Trim whitespace */
    len = strlen(key);
    while (len > 0 && (key[len-1] == '\n' || key[len-1] == '\r' || key[len-1] == ' ' || key[len-1] == '\t'))
    {
        key[--len] = '\0';
    }

    /* Trim leading whitespace */
    end = key;
    while (*end == ' ' || *end == '\t')
        end++;

    if (end != key)
        memmove(key, end, strlen(end) + 1);

    if (strlen(key) == 0)
    {
        free(key);
        yo_print_error("~/.yoshkey is empty");
        return NULL;
    }

    return key;
}

/* **************************************************************** */
/*                                                                  */
/*                      Claude API Call                             */
/*                                                                  */
/* **************************************************************** */

/* Build the tools array for the API request */
static cJSON *
yo_build_tools(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool, *schema, *props, *prop, *required;

    /* Tool: command - execute a shell command */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "command");
    cJSON_AddStringToObject(tool, "description",
        "Generate a shell command for the user to review and execute. "
        "The command will be prefilled at the prompt for the user to edit or run.");
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "The shell command to execute");
    cJSON_AddItemToObject(props, "command", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description",
        "Brief explanation of what this command does, shown to user before the command");
    cJSON_AddItemToObject(props, "explanation", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "boolean");
    cJSON_AddStringToObject(prop, "description",
        "Set to true if this is part of a multi-step sequence and you need to see "
        "the output before providing the next command. After the user executes this "
        "command, you will automatically receive the terminal output.");
    cJSON_AddItemToObject(props, "pending", prop);
    cJSON_AddItemToObject(schema, "properties", props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToArray(required, cJSON_CreateString("explanation"));
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: chat - respond with text */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "chat");
    cJSON_AddStringToObject(tool, "description",
        "Respond with a text message for questions, explanations, or when no command is needed.");
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "Your text response to the user");
    cJSON_AddItemToObject(props, "response", prop);
    cJSON_AddItemToObject(schema, "properties", props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("response"));
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: scrollback - request terminal output */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "scrollback");
    cJSON_AddStringToObject(tool, "description",
        "Request recent terminal output to see command results, error messages, or context. "
        "Use this when you need to see what happened in the terminal.");
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "integer");
    cJSON_AddStringToObject(prop, "description", "Number of recent lines to retrieve (max 1000)");
    cJSON_AddItemToObject(props, "lines", prop);
    cJSON_AddItemToObject(schema, "properties", props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("lines"));
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: docs - request yosh documentation */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "docs");
    cJSON_AddStringToObject(tool, "description",
        "Request yosh documentation to answer questions about yosh features, configuration, "
        "environment variables, API key setup, or usage.");
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    cJSON_AddItemToObject(schema, "properties", props);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    return tools;
}

/* Forward declaration for retry logic */
static cJSON *yo_call_claude_with_messages_internal(const char *api_key, cJSON *messages, int is_retry);

/* Internal function to call Claude API with pre-built messages array.
   The messages array is consumed (added to request JSON and freed).
   Returns the tool_use cJSON object (caller must free with cJSON_Delete). */
static cJSON *
yo_call_claude_with_messages(const char *api_key, cJSON *messages)
{
    return yo_call_claude_with_messages_internal(api_key, messages, 0);
}

static cJSON *
yo_call_claude_with_messages_internal(const char *api_key, cJSON *messages, int is_retry)
{
    CURL *curl;
    CURLM *multi;
    struct curl_slist *headers = NULL;
    yo_response_buffer_t response_buf = {0};
    cJSON *request_json = NULL;
    cJSON *tools = NULL;
    cJSON *tool_choice = NULL;
    char *request_body = NULL;
    char auth_header[300];
    cJSON *result = NULL;
    cJSON *response_json = NULL;
    cJSON *content_array = NULL;
    cJSON *content_item = NULL;
    cJSON *type_item = NULL;
    struct sigaction sa, old_sa;
    int still_running = 1;
    int cancelled = 0;
    int i, content_count;

    /* Initialize self-pipe for Ctrl-C handling */
    if (yo_init_sigint_pipe() < 0)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to initialize signal handling: %s", strerror(errno));
        cJSON_Delete(messages);
        return NULL;
    }

    /* Drain any stale signals and reset cancelled flag */
    yo_drain_sigint_pipe();

    curl = curl_easy_init();
    if (!curl)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to initialize HTTP client (curl_easy_init returned NULL)");
        cJSON_Delete(messages);
        return NULL;
    }

    multi = curl_multi_init();
    if (!multi)
    {
        curl_easy_cleanup(curl);
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to initialize HTTP client (curl_multi_init returned NULL)");
        cJSON_Delete(messages);
        return NULL;
    }

    /* Build tools array */
    tools = yo_build_tools();

    /* Build request JSON */
    request_json = cJSON_CreateObject();

    cJSON_AddStringToObject(request_json, "model", yo_model ? yo_model : YO_DEFAULT_MODEL);
    cJSON_AddNumberToObject(request_json, "max_tokens", YO_MAX_TOKENS);
    cJSON_AddStringToObject(request_json, "system", yo_system_prompt);

    /* Add messages array to request (takes ownership) */
    cJSON_AddItemToObject(request_json, "messages", messages);

    /* Add tools array (takes ownership) */
    cJSON_AddItemToObject(request_json, "tools", tools);

    /* Force tool use with tool_choice: {"type": "any"} */
    tool_choice = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_choice, "type", "any");
    cJSON_AddItemToObject(request_json, "tool_choice", tool_choice);

    request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);

    /* Set up headers */
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    /* Configure CURL easy handle */
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, yo_curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, YO_API_TIMEOUT);

    /* Add easy handle to multi handle */
    curl_multi_add_handle(multi, curl);

    /* Install our SIGINT handler for the duration of the request */
    sa.sa_handler = yo_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sa);

    /* Multi interface loop with curl_multi_poll() */
    CURLMcode mc;
    bool had_curl_error = false;
    while (still_running && !cancelled)
    {
        int numfds;
        struct curl_waitfd extra_fd;

        /* Let curl do any immediate work */
        mc = curl_multi_perform(multi, &still_running);
        if (mc != CURLM_OK)
        {
            had_curl_error = true;
            break;
        }

        if (!still_running)
            break;

        /* Set up our signal pipe as an extra fd to poll */
        extra_fd.fd = yo_sigint_pipe[0];
        extra_fd.events = CURL_WAIT_POLLIN;
        extra_fd.revents = 0;

        /* Wait for activity on curl sockets or our signal pipe */
        mc = curl_multi_poll(multi, &extra_fd, 1, 1000, &numfds);
        if (mc != CURLM_OK)
        {
            had_curl_error = true;
            break;
        }

        /* Check if signal pipe has data (SIGINT was received) */
        if (extra_fd.revents & CURL_WAIT_POLLIN)
        {
            cancelled = 1;
            break;
        }

        /* Also check the flag in case signal arrived but poll didn't catch it */
        if (yo_cancelled)
        {
            cancelled = 1;
            break;
        }
    }

    /* Restore original SIGINT handler */
    sigaction(SIGINT, &old_sa, NULL);

    if (had_curl_error)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("HTTP error: %s", curl_multi_strerror(mc));
        goto curl_error;
    }

    /* Handle cancellation */
    if (cancelled)
    {
        yo_clear_thinking();
        fprintf(rl_outstream, "%sCancelled%s\n", yo_get_chat_color(), YO_COLOR_RESET);
        fflush(rl_outstream);
        goto curl_error;
    }

    CURLMsg *msg;
    int msgs_left;
    while ((msg = curl_multi_info_read(multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            CURL *easy = msg->easy_handle;
            ZASSERT(easy == curl);
            CURLcode result = msg->data.result;
            
            if (result != CURLE_OK) {
                yo_clear_thinking();
                yo_print_error_no_newline("HTTP error: %s", curl_easy_strerror(result));
                goto curl_error;
            }
            
            // Even if result == CURLE_OK, check the HTTP status code
            long http_code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code == 200)
                break;

            yo_clear_thinking();
            yo_print_error_no_newline("Unexpected HTTP status code: %ld", http_code);
            goto curl_error;
        }
    }

    /* Clean up curl handles */
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    free(request_body);
    curl_easy_cleanup(curl);

    if (!response_buf.data)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("No response from API");
        return NULL;
    }

    /* Parse API response */
    response_json = cJSON_Parse(response_buf.data);
    free(response_buf.data);

    if (!response_json)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to parse API response: %s", response_buf.data);
        return NULL;
    }

    /* Extract content array from response */
    content_array = cJSON_GetObjectItem(response_json, "content");
    if (!content_array || !cJSON_IsArray(content_array))
    {
        /* Check for error response */
        cJSON *error = cJSON_GetObjectItem(response_json, "error");
        yo_clear_thinking();
        if (error)
        {
            cJSON *msg = cJSON_GetObjectItem(error, "message");
            if (msg && cJSON_IsString(msg))
            {
                fprintf(rl_outstream, "%sAPI error: %s%s\n",
                        yo_get_chat_color(), msg->valuestring, YO_COLOR_RESET);
                fflush(rl_outstream);
            }
            else
            {
                yo_print_error_no_newline("API returned an error");
            }
        }
        else
        {
            yo_print_error_no_newline("Unexpected API response format: %s",
                                      cJSON_PrintUnformatted(response_json));
        }
        cJSON_Delete(response_json);
        return NULL;
    }

    /* Count tool_use blocks and find text blocks */
    content_count = cJSON_GetArraySize(content_array);
    {
        int tool_use_count = 0;
        int first_tool_use_idx = -1;
        int text_idx = -1;
        char *text_content = NULL;

        for (i = 0; i < content_count; i++)
        {
            content_item = cJSON_GetArrayItem(content_array, i);
            if (!content_item)
                continue;

            type_item = cJSON_GetObjectItem(content_item, "type");
            if (!type_item || !cJSON_IsString(type_item))
                continue;

            if (strcmp(type_item->valuestring, "tool_use") == 0)
            {
                tool_use_count++;
                if (first_tool_use_idx < 0)
                    first_tool_use_idx = i;
            }
            else if (strcmp(type_item->valuestring, "text") == 0 && text_idx < 0)
            {
                text_idx = i;
            }
        }

        if (tool_use_count == 0)
        {
            /* No tool_use - convert text to synthetic chat tool_use */
            if (text_idx >= 0)
            {
                cJSON *text_block = cJSON_GetArrayItem(content_array, text_idx);
                cJSON *text_item = cJSON_GetObjectItem(text_block, "text");
                if (text_item && cJSON_IsString(text_item))
                    text_content = text_item->valuestring;
            }

            /* Build synthetic chat tool_use */
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "type", "tool_use");
            cJSON_AddStringToObject(result, "id", "synthetic_text_response");
            cJSON_AddStringToObject(result, "name", "chat");
            {
                cJSON *input = cJSON_CreateObject();
                cJSON_AddStringToObject(input, "response",
                    text_content ? text_content : "(empty response)");
                cJSON_AddItemToObject(result, "input", input);
            }
            cJSON_Delete(response_json);
            return result;
        }
        else if (tool_use_count == 1)
        {
            /* Exactly one tool_use - return it */
            result = cJSON_DetachItemFromArray(content_array, first_tool_use_idx);
            cJSON_Delete(response_json);
            return result;
        }
        else if (is_retry)
        {
            /* Multiple tool_use blocks on retry - just take the first one */
            result = cJSON_DetachItemFromArray(content_array, first_tool_use_idx);
            cJSON_Delete(response_json);
            return result;
        }
        else
        {
            /* Multiple tool_use blocks - ask Claude to pick exactly one */
            cJSON *retry_messages = cJSON_CreateArray();
            cJSON *assistant_msg = cJSON_CreateObject();
            cJSON *user_msg = cJSON_CreateObject();
            cJSON *content_copy = cJSON_Duplicate(content_array, 1);

            /* Build assistant message with the multi-tool response */
            cJSON_AddStringToObject(assistant_msg, "role", "assistant");
            cJSON_AddItemToObject(assistant_msg, "content", content_copy);
            cJSON_AddItemToArray(retry_messages, assistant_msg);

            /* Build user message asking to pick one */
            cJSON_AddStringToObject(user_msg, "role", "user");
            cJSON_AddStringToObject(user_msg, "content",
                "You provided multiple tool calls. Please respond with exactly one tool call - "
                "the most appropriate one for the user's request.");
            cJSON_AddItemToArray(retry_messages, user_msg);

            cJSON_Delete(response_json);

            /* Retry with the follow-up messages */
            return yo_call_claude_with_messages_internal(api_key, retry_messages, 1);
        }
    }

curl_error:
    if (response_buf.data)
        free(response_buf.data);
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    free(request_body);
    curl_easy_cleanup(curl);
    return NULL;
}

/* Main API call function - builds messages from query and calls API */
static cJSON *
yo_call_claude(const char *api_key, const char *query)
{
    cJSON *messages = yo_build_messages(query);
    return yo_call_claude_with_messages(api_key, messages);
}

/* API call with scrollback context - for follow-up after scrollback request */
static cJSON *
yo_call_claude_with_scrollback(const char *api_key, const char *query,
                               const char *scrollback_request, const char *scrollback_data,
                               const char *scrollback_tool_id)
{
    cJSON *messages = yo_build_messages_with_scrollback(query, scrollback_request,
                                                        scrollback_data, scrollback_tool_id);
    return yo_call_claude_with_messages(api_key, messages);
}

/* **************************************************************** */
/*                                                                  */
/*                  Explanation Retry Logic                         */
/*                                                                  */
/* **************************************************************** */

/* When the LLM returns a command response without the required explanation
   field, re-prompt it once with the original tool_use as context and a
   request to include the explanation.  Returns the new tool_use cJSON
   (caller must free with cJSON_Delete), or NULL on failure/cancellation. */
static cJSON *
yo_retry_for_explanation(const char *api_key, const char *query, cJSON *original_tool_use)
{
    cJSON *messages;
    cJSON *msg;
    cJSON *content_array;
    cJSON *tool_result;
    cJSON *tool_use_copy;
    cJSON *id_item;
    const char *tool_use_id;

    /* Get the tool_use id */
    id_item = cJSON_GetObjectItem(original_tool_use, "id");
    if (!id_item || !cJSON_IsString(id_item))
        return NULL;
    tool_use_id = id_item->valuestring;

    /* Build the normal message history including the current query */
    messages = yo_build_messages(query);

    /* Append the assistant's original tool_use (the one missing explanation) */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    content_array = cJSON_CreateArray();
    tool_use_copy = cJSON_Duplicate(original_tool_use, 1);
    cJSON_AddItemToArray(content_array, tool_use_copy);
    cJSON_AddItemToObject(msg, "content", content_array);
    cJSON_AddItemToArray(messages, msg);

    /* Append a user message with tool_result requesting the explanation */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    content_array = cJSON_CreateArray();
    tool_result = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_result, "type", "tool_result");
    cJSON_AddStringToObject(tool_result, "tool_use_id", tool_use_id);
    cJSON_AddStringToObject(tool_result, "content",
        "Your command response is missing the required \"explanation\" field. "
        "Please respond again with the same command but include a brief explanation. "
        "The explanation is shown to the user before the command and is essential "
        "for them to understand what the command does.");
    cJSON_AddItemToArray(content_array, tool_result);
    cJSON_AddItemToObject(msg, "content", content_array);
    cJSON_AddItemToArray(messages, msg);

    return yo_call_claude_with_messages(api_key, messages);
}

/* **************************************************************** */
/*                                                                  */
/*                    Response Parsing                              */
/*                                                                  */
/* **************************************************************** */

/* Parse a tool_use cJSON object (from API response).
   Extracts the tool name as type, the tool_use id, and fields from input.
   The tool_use object is NOT freed - caller retains ownership. */
static int
yo_parse_response(cJSON *tool_use, char **type, char **content, char **explanation,
                  char **tool_use_id, int *pending)
{
    cJSON *name_item;
    cJSON *id_item;
    cJSON *input;
    cJSON *content_item;
    cJSON *explanation_item;
    cJSON *lines_item;
    cJSON *pending_item;

    *type = NULL;
    *content = NULL;
    *explanation = NULL;
    *tool_use_id = NULL;
    *pending = 0;

    if (!tool_use)
        return 0;

    /* Extract tool name as type */
    name_item = cJSON_GetObjectItem(tool_use, "name");
    if (!name_item || !cJSON_IsString(name_item))
        return 0;

    *type = strdup(name_item->valuestring);

    /* Extract tool_use id */
    id_item = cJSON_GetObjectItem(tool_use, "id");
    if (id_item && cJSON_IsString(id_item))
        *tool_use_id = strdup(id_item->valuestring);

    /* Extract input object */
    input = cJSON_GetObjectItem(tool_use, "input");
    if (!input)
    {
        /* docs tool may have empty input */
        if (strcmp(*type, "docs") == 0)
        {
            *content = strdup("");
            return 1;
        }
        free(*type);
        *type = NULL;
        if (*tool_use_id) { free(*tool_use_id); *tool_use_id = NULL; }
        return 0;
    }

    if (strcmp(*type, "command") == 0)
    {
        content_item = cJSON_GetObjectItem(input, "command");
        if (content_item && cJSON_IsString(content_item))
            *content = strdup(content_item->valuestring);

        explanation_item = cJSON_GetObjectItem(input, "explanation");
        if (explanation_item && cJSON_IsString(explanation_item))
            *explanation = strdup(explanation_item->valuestring);

        pending_item = cJSON_GetObjectItem(input, "pending");
        if (pending_item && cJSON_IsTrue(pending_item))
            *pending = 1;
    }
    else if (strcmp(*type, "chat") == 0)
    {
        content_item = cJSON_GetObjectItem(input, "response");
        if (content_item && cJSON_IsString(content_item))
            *content = strdup(content_item->valuestring);
    }
    else if (strcmp(*type, "scrollback") == 0)
    {
        lines_item = cJSON_GetObjectItem(input, "lines");
        if (lines_item && cJSON_IsNumber(lines_item))
        {
            char lines_str[32];
            snprintf(lines_str, sizeof(lines_str), "%d", (int)lines_item->valuedouble);
            *content = strdup(lines_str);
        }
        else
        {
            *content = strdup("50");
        }
    }
    else if (strcmp(*type, "docs") == 0)
    {
        *content = strdup("");
    }

    if (!*content)
    {
        free(*type);
        *type = NULL;
        if (*tool_use_id) { free(*tool_use_id); *tool_use_id = NULL; }
        return 0;
    }

    return 1;
}

/* **************************************************************** */
/*                                                                  */
/*                    Display Functions                             */
/*                                                                  */
/* **************************************************************** */

static const char *
yo_get_chat_color(void)
{
    const char *env_val = getenv("YO_CHAT_COLOR");
    if (env_val && *env_val)
        return env_val;
    return YO_DEFAULT_CHAT_COLOR;
}

static void
yo_display_chat(const char *response)
{
    fprintf(rl_outstream, "%s%s%s\n", yo_get_chat_color(), response, YO_COLOR_RESET);
    fflush(rl_outstream);
}

static void
yo_print_error_no_newlinev(const char *msg, va_list args)
{
    fprintf(rl_outstream, "%sError: ", yo_get_chat_color());
    vfprintf(rl_outstream, msg, args);
    fprintf(rl_outstream, "%s\n", YO_COLOR_RESET);
    fflush(rl_outstream);
}

static void
yo_print_error_no_newline(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    yo_print_error_no_newlinev(msg, args);
    va_end(args);
}

static void
yo_print_error(const char *msg, ...)
{
    fprintf(rl_outstream, "\n");
    va_list args;
    va_start(args, msg);
    yo_print_error_no_newlinev(msg, args);
    va_end(args);
}

static void
yo_print_thinking(void)
{
    fprintf(rl_outstream, "%sThinking...%s", yo_get_chat_color(), YO_COLOR_RESET);
    fflush(rl_outstream);
}

static void
yo_clear_thinking(void)
{
    int saved_errno = errno;
    /* Move cursor back and clear the line */
    fprintf(rl_outstream, "\r\033[K");
    fflush(rl_outstream);
    errno = saved_errno;
}

static void
yo_report_parse_error(cJSON *tool_use)
{
    yo_clear_thinking();
    yo_print_error_no_newline("Failed to parse tool_use from Claude: %s",
                              tool_use ? cJSON_PrintUnformatted(tool_use) : "(null)");
}

/* **************************************************************** */
/*                                                                  */
/*                   Session Memory Functions                       */
/*                                                                  */
/* **************************************************************** */

static void
yo_history_add(const char *query, const char *type, const char *response, const char *tool_use_id, int executed, int pending)
{
    /* Prune if necessary */
    yo_history_prune();

    /* Grow array if needed */
    if (yo_history_count >= yo_history_capacity)
    {
        int new_capacity = yo_history_capacity == 0 ? 8 : yo_history_capacity * 2;
        yo_exchange_t *new_history = realloc(yo_history, new_capacity * sizeof(yo_exchange_t));
        if (!new_history)
            return;
        yo_history = new_history;
        yo_history_capacity = new_capacity;
    }

    /* Add new entry */
    yo_history[yo_history_count].query = strdup(query);
    yo_history[yo_history_count].response_type = strdup(type);
    yo_history[yo_history_count].response = strdup(response);
    yo_history[yo_history_count].tool_use_id = tool_use_id ? strdup(tool_use_id) : NULL;
    yo_history[yo_history_count].executed = executed;
    yo_history[yo_history_count].pending = pending;
    yo_history_count++;
}

static void
yo_history_prune(void)
{
    /* Check count limit */
    while (yo_history_count >= yo_history_limit)
    {
        /* Remove oldest entry */
        if (yo_history[0].query)
            free(yo_history[0].query);
        if (yo_history[0].response_type)
            free(yo_history[0].response_type);
        if (yo_history[0].response)
            free(yo_history[0].response);
        if (yo_history[0].tool_use_id)
            free(yo_history[0].tool_use_id);

        memmove(&yo_history[0], &yo_history[1], (yo_history_count - 1) * sizeof(yo_exchange_t));
        yo_history_count--;
    }

    /* Check token budget */
    while (yo_history_count > 0 && yo_estimate_tokens() > yo_token_budget)
    {
        /* Remove oldest entry */
        if (yo_history[0].query)
            free(yo_history[0].query);
        if (yo_history[0].response_type)
            free(yo_history[0].response_type);
        if (yo_history[0].response)
            free(yo_history[0].response);
        if (yo_history[0].tool_use_id)
            free(yo_history[0].tool_use_id);

        memmove(&yo_history[0], &yo_history[1], (yo_history_count - 1) * sizeof(yo_exchange_t));
        yo_history_count--;
    }
}

static int
yo_estimate_tokens(void)
{
    int total = 0;
    int i;

    for (i = 0; i < yo_history_count; i++)
    {
        if (yo_history[i].query)
            total += strlen(yo_history[i].query);
        if (yo_history[i].response)
            total += strlen(yo_history[i].response);
    }

    /* Rough estimate: 4 chars per token */
    return total / 4;
}

/* Helper to build a tool_use content block for history reconstruction */
static cJSON *
yo_build_tool_use_block(const char *tool_use_id, const char *type, const char *response, int pending)
{
    cJSON *tool_use = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();

    cJSON_AddStringToObject(tool_use, "type", "tool_use");
    cJSON_AddStringToObject(tool_use, "id", tool_use_id);
    cJSON_AddStringToObject(tool_use, "name", type);

    if (strcmp(type, "command") == 0)
    {
        cJSON_AddStringToObject(input, "command", response);
        cJSON_AddStringToObject(input, "explanation", "(from history)");
        if (pending)
            cJSON_AddTrueToObject(input, "pending");
    }
    else if (strcmp(type, "chat") == 0)
    {
        cJSON_AddStringToObject(input, "response", response);
    }

    cJSON_AddItemToObject(tool_use, "input", input);
    return tool_use;
}

/* Helper to build a tool_result content block */
static cJSON *
yo_build_tool_result_block(const char *tool_use_id, const char *result_content)
{
    cJSON *tool_result = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_result, "type", "tool_result");
    cJSON_AddStringToObject(tool_result, "tool_use_id", tool_use_id);
    cJSON_AddStringToObject(tool_result, "content", result_content);
    return tool_result;
}

static cJSON *
yo_build_messages(const char *current_query)
{
    cJSON *messages;
    cJSON *msg;
    cJSON *content_array;
    int i;

    messages = cJSON_CreateArray();

    /* Add history entries */
    for (i = 0; i < yo_history_count; i++)
    {
        /* User message with query */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", yo_history[i].query);
        cJSON_AddItemToArray(messages, msg);

        /* Assistant message with tool_use */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "assistant");
        content_array = cJSON_CreateArray();
        cJSON_AddItemToArray(content_array,
            yo_build_tool_use_block(yo_history[i].tool_use_id,
                                    yo_history[i].response_type,
                                    yo_history[i].response,
                                    yo_history[i].pending));
        cJSON_AddItemToObject(msg, "content", content_array);
        cJSON_AddItemToArray(messages, msg);

        /* User message with tool_result */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        content_array = cJSON_CreateArray();

        if (strcmp(yo_history[i].response_type, "command") == 0)
        {
            cJSON_AddItemToArray(content_array,
                yo_build_tool_result_block(yo_history[i].tool_use_id,
                    yo_history[i].executed ? "User executed the command" : "User did not execute the command"));
        }
        else
        {
            cJSON_AddItemToArray(content_array,
                yo_build_tool_result_block(yo_history[i].tool_use_id, "Acknowledged"));
        }
        cJSON_AddItemToObject(msg, "content", content_array);
        cJSON_AddItemToArray(messages, msg);
    }

    /* Add current query */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", current_query);
    cJSON_AddItemToArray(messages, msg);

    return messages;
}

/* Build messages array for a follow-up call after a scrollback request.
   This adds the assistant's scrollback tool_use, user's tool_result with scrollback data,
   and then expects assistant to give final response. */
static cJSON *
yo_build_messages_with_scrollback(const char *current_query, const char *scrollback_request,
                                  const char *scrollback_data, const char *scrollback_tool_id)
{
    cJSON *messages;
    cJSON *msg;
    cJSON *content_array;
    cJSON *tool_use;
    cJSON *tool_input;
    int i;
    char *scrollback_msg;

    /* scrollback_request contains the lines count as a string */
    int lines_requested = atoi(scrollback_request);
    if (lines_requested <= 0)
        lines_requested = 50;

    messages = cJSON_CreateArray();

    /* Add history entries (same as yo_build_messages) */
    for (i = 0; i < yo_history_count; i++)
    {
        /* User message with query */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", yo_history[i].query);
        cJSON_AddItemToArray(messages, msg);

        /* Assistant message with tool_use */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "assistant");
        content_array = cJSON_CreateArray();
        cJSON_AddItemToArray(content_array,
            yo_build_tool_use_block(yo_history[i].tool_use_id,
                                    yo_history[i].response_type,
                                    yo_history[i].response,
                                    yo_history[i].pending));
        cJSON_AddItemToObject(msg, "content", content_array);
        cJSON_AddItemToArray(messages, msg);

        /* User message with tool_result */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        content_array = cJSON_CreateArray();
        if (strcmp(yo_history[i].response_type, "command") == 0)
        {
            cJSON_AddItemToArray(content_array,
                yo_build_tool_result_block(yo_history[i].tool_use_id,
                    yo_history[i].executed ? "User executed the command" : "User did not execute the command"));
        }
        else
        {
            cJSON_AddItemToArray(content_array,
                yo_build_tool_result_block(yo_history[i].tool_use_id, "Acknowledged"));
        }
        cJSON_AddItemToObject(msg, "content", content_array);
        cJSON_AddItemToArray(messages, msg);
    }

    /* Add current query */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", current_query);
    cJSON_AddItemToArray(messages, msg);

    /* Add assistant's scrollback tool_use */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    content_array = cJSON_CreateArray();
    tool_use = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_use, "type", "tool_use");
    cJSON_AddStringToObject(tool_use, "id", scrollback_tool_id);
    cJSON_AddStringToObject(tool_use, "name", "scrollback");
    tool_input = cJSON_CreateObject();
    cJSON_AddNumberToObject(tool_input, "lines", lines_requested);
    cJSON_AddItemToObject(tool_use, "input", tool_input);
    cJSON_AddItemToArray(content_array, tool_use);
    cJSON_AddItemToObject(msg, "content", content_array);
    cJSON_AddItemToArray(messages, msg);

    /* Add user's tool_result with scrollback data */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    content_array = cJSON_CreateArray();
    asprintf(&scrollback_msg, "Here is the recent terminal output you requested:\n```\n%s\n```",
             scrollback_data);
    cJSON_AddItemToArray(content_array,
        yo_build_tool_result_block(scrollback_tool_id, scrollback_msg));
    free(scrollback_msg);
    cJSON_AddItemToObject(msg, "content", content_array);
    cJSON_AddItemToArray(messages, msg);

    return messages;
}

/* Build messages array for a follow-up call after a docs request.
   This adds the assistant's docs tool_use, user's tool_result with documentation,
   and then expects assistant to give final response. */
static cJSON *
yo_build_messages_with_docs(const char *current_query, const char *docs_request,
                            const char *docs_tool_id)
{
    cJSON *messages;
    cJSON *msg;
    cJSON *content_array;
    cJSON *tool_use;
    cJSON *tool_input;
    int i;
    char *docs_msg;

    (void)docs_request;  /* unused with tool use - we just need the tool_id */

    messages = cJSON_CreateArray();

    /* Add history entries (same as yo_build_messages) */
    for (i = 0; i < yo_history_count; i++)
    {
        /* User message with query */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", yo_history[i].query);
        cJSON_AddItemToArray(messages, msg);

        /* Assistant message with tool_use */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "assistant");
        content_array = cJSON_CreateArray();
        cJSON_AddItemToArray(content_array,
            yo_build_tool_use_block(yo_history[i].tool_use_id,
                                    yo_history[i].response_type,
                                    yo_history[i].response,
                                    yo_history[i].pending));
        cJSON_AddItemToObject(msg, "content", content_array);
        cJSON_AddItemToArray(messages, msg);

        /* User message with tool_result */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        content_array = cJSON_CreateArray();
        if (strcmp(yo_history[i].response_type, "command") == 0)
        {
            cJSON_AddItemToArray(content_array,
                yo_build_tool_result_block(yo_history[i].tool_use_id,
                    yo_history[i].executed ? "User executed the command" : "User did not execute the command"));
        }
        else
        {
            cJSON_AddItemToArray(content_array,
                yo_build_tool_result_block(yo_history[i].tool_use_id, "Acknowledged"));
        }
        cJSON_AddItemToObject(msg, "content", content_array);
        cJSON_AddItemToArray(messages, msg);
    }

    /* Add current query */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", current_query);
    cJSON_AddItemToArray(messages, msg);

    /* Add assistant's docs tool_use */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    content_array = cJSON_CreateArray();
    tool_use = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_use, "type", "tool_use");
    cJSON_AddStringToObject(tool_use, "id", docs_tool_id);
    cJSON_AddStringToObject(tool_use, "name", "docs");
    tool_input = cJSON_CreateObject();
    cJSON_AddItemToObject(tool_use, "input", tool_input);
    cJSON_AddItemToArray(content_array, tool_use);
    cJSON_AddItemToObject(msg, "content", content_array);
    cJSON_AddItemToArray(messages, msg);

    /* Add user's tool_result with documentation */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    content_array = cJSON_CreateArray();
    asprintf(&docs_msg, "Here is the yosh documentation:\n\n%s\n\n"
             "Now please answer the user's original question based on this documentation.",
             yo_documentation);
    cJSON_AddItemToArray(content_array,
        yo_build_tool_result_block(docs_tool_id, docs_msg));
    free(docs_msg);
    cJSON_AddItemToObject(msg, "content", content_array);
    cJSON_AddItemToArray(messages, msg);

    return messages;
}

/* API call with docs context - for follow-up after docs request */
static cJSON *
yo_call_claude_with_docs(const char *api_key, const char *query, const char *docs_request,
                         const char *docs_tool_id)
{
    cJSON *messages = yo_build_messages_with_docs(query, docs_request, docs_tool_id);
    return yo_call_claude_with_messages(api_key, messages);
}
