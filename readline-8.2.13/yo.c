/* yo.c -- LLM-powered shell assistant for readline */

/* Copyright (C) 2026 Epic Games, Inc.
   Copyright (C) 2026 Filip Pizlo

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
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
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

/* Context compaction.  When the estimated size of a request (history + system
   prompt + query) exceeds half of the effective context window, the oldest
   ~75% of the session history (by estimated tokens) is summarized by the LLM
   and replaced with a single synthetic summary exchange; the most recent
   ~25% is kept verbatim. */
#define YO_COMPACTION_MIN_TOKENS 256        /* don't bother below this history size */
#define YO_COMPACTION_OLD_PART_PCT 75       /* summarize the first ~75% of history tokens */
#define YO_SUMMARY_MAX_OUTPUT_TOKENS 2048L  /* cap for the summarizer request (min with configured max_output_tokens) */

/* System prompt for the compaction summarizer request (NOT the yosh shell
   system prompt — the summarizer only needs to condense the transcript). */
#define YO_SUMMARIZER_SYSTEM_PROMPT \
    "You are a concise summarization assistant. Summarize the conversation as instructed."

/* Final user message of the compaction summarizer request (the conversation
   transcript follows it in the same message). */
#define YO_COMPACTION_PROMPT \
    "[compaction] Summarize the following conversation transcript in at most 300 words. " \
    "Preserve: the user's goals, commands that were run and their outcomes, " \
    "key facts learned, and anything unresolved. Output only the summary text."

/* The synthetic history exchange that replaces the summarized part. */
#define YO_COMPACTION_QUERY "[context compacted] Summary of the earlier conversation:"
#define YO_COMPACTION_TOOL_USE_ID "compaction_summary"
#define YO_DEFAULT_OPENAI_MODEL "gpt-5.2"
#define YO_DEFAULT_KIMI_MODEL "kimi-k2.5"
#define YO_DEFAULT_DEEPSEEK_MODEL "deepseek-v4-flash"
#define YO_DEFAULT_QWEN_MODEL "qwen-plus"
#define YO_DEFAULT_ZAI_MODEL "glm-5.2"
#define YO_DEFAULT_META_MODEL "muse-spark-1.3"
#define YO_DEFAULT_OPENROUTER_MODEL "meta/muse-spark-1.3"

/* Model limits.  When the model is unknown to the built-in registry (ported
   from brainstorm-3 lib/shared/model_registry.rb), we assume these defaults. */
#define YO_REGISTRY_DEFAULT_MAX_OUTPUT_TOKENS 16000L  /* known context, unknown output */
#define YO_UNKNOWN_MODEL_CONTEXT_WINDOW 131072L       /* model not in registry at all */
#define YO_UNKNOWN_MODEL_MAX_OUTPUT_TOKENS 16384L

/* Timeout (seconds) for the best-effort model-info API GETs. */
#define YO_MODEL_INFO_TIMEOUT 10L

/* Thinking levels, from the ~/.yoconf "thinking" directive.
   YO_THINKING_UNSET means the user did not configure thinking (disabled);
   YO_THINKING_OFF is the explicit "off"/"none" (also disabled). */
#define YO_THINKING_UNSET   (-1)
#define YO_THINKING_OFF     0
#define YO_THINKING_MINIMAL 1
#define YO_THINKING_LOW     2
#define YO_THINKING_MEDIUM  3
#define YO_THINKING_HIGH    4
#define YO_THINKING_XHIGH   5
#define YO_THINKING_MAX     6

/* Default styling.  Base text is italic cyan (color_prefix).
   Since base is already italic, markdown *italic* toggles italic OFF;
   leaving *italic* toggles it back ON.  Bold is straightforward on/off.
   These are individual SGR toggles — they compose with existing state. */
#define YO_DEFAULT_COLOR_PREFIX "\033[3;36m"
#define YO_DEFAULT_COLOR_RESET "\033[0m"
#define YO_DEFAULT_ENABLE_ITALIC "\033[23m"   /* disable terminal italic (toggle off from italic base) */
#define YO_DEFAULT_DISABLE_ITALIC "\033[3m"   /* re-enable terminal italic (back to base) */
#define YO_DEFAULT_ENABLE_BOLD "\033[1m"      /* turn bold on */
#define YO_DEFAULT_DISABLE_BOLD "\033[22m"    /* turn bold off */
#define YO_DEFAULT_ENABLE_STRIKETHROUGH "\033[9m"   /* turn strikethrough on */
#define YO_DEFAULT_DISABLE_STRIKETHROUGH "\033[29m" /* turn strikethrough off */
#define YO_DEFAULT_CODE_DELIMITER "\033[0;3;38;5;23m" /* reset, italic, dark cyan (256-color #23) */

/* Scrollback defaults */
#define YO_DEFAULT_SCROLLBACK_LINES 1000
#define YO_DEFAULT_SCROLLBACK_BYTES (1024 * 1024)  /* 1MB */

/* **************************************************************** */
/*                                                                  */
/*                     Session Memory Types                         */
/*                                                                  */
/* **************************************************************** */

typedef enum {
    YO_RESPONSE_COMMAND,
    YO_RESPONSE_CHAT,
    YO_RESPONSE_SCROLLBACK,
    YO_RESPONSE_DOCS,
    YO_RESPONSE_ERROR
} yo_response_type_t;

typedef enum {
    YO_PROVIDER_ANTHROPIC,
    YO_PROVIDER_OPENAI,
    YO_PROVIDER_KIMI,
    YO_PROVIDER_DEEPSEEK,
    YO_PROVIDER_QWEN,
    YO_PROVIDER_ZAI,
    YO_PROVIDER_META,
    YO_PROVIDER_OPENROUTER
} yo_provider_t;

static const char *yo_provider_to_string(yo_provider_t provider);

typedef struct {
    yo_response_type_t type;
    char *content;        /* command string, chat text, scrollback lines count, etc. */
    char *explanation;    /* command explanation (may be NULL) */
    char *tool_use_id;    /* Anthropic tool_use "id" field */
    int pending;          /* 1 if multi-step continuation */
    cJSON *raw_tool_use;  /* raw cJSON tool_use block (owned by this struct) */
    char *reasoning_content;  /* Kimi reasoning content when thinking is enabled */
    char *reasoning_items_json;  /* serialized reasoning-items array (Responses API
                                    reasoning replay; NULL when absent) */
} yo_response_t;

#define YO_LLM_RETRY_EXPLANATION            (1 << 0)
#define YO_LLM_RETRY_EXPLANATION_IF_PENDING  (1 << 1)

typedef struct {
    char *query;                   /* "yo find python files" */
    yo_response_type_t response_type;  /* YO_RESPONSE_COMMAND, YO_RESPONSE_CHAT, etc. */
    char *response;                /* the command or chat text */
    char *tool_use_id;             /* tool_use.id from LLM response */
    int executed;                  /* 1 if user ran it, 0 if not */
    int pending;                   /* 1 if response had "pending":true (multi-step) */
    char *reasoning_content;       /* Kimi reasoning content when thinking is enabled */
    char *reasoning_items_json;    /* serialized reasoning-items array for Responses
                                      API reasoning replay (NULL when absent) */
} yo_exchange_t;

/* **************************************************************** */
/*                                                                  */
/*                    Tool Definition Types                         */
/*                                                                  */
/* **************************************************************** */

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
static int yo_token_budget_set = 0;   /* 1 when ~/.yoconf sets token_budget explicitly (it
                                         then acts as an alias of the context window for
                                         compaction/usage-display purposes) */
static char *yo_model = NULL;
static char *yo_system_prompt = NULL;
static const char *yo_name = NULL;
static rl_yo_docs_callback_t yo_documentation_callback = NULL;
static rl_yo_prompt_callback_t yo_prompt_callback = NULL;
static int yo_server_web_enabled = 1;
static yo_provider_t yo_provider = YO_PROVIDER_ANTHROPIC;
static char *yo_api_key = NULL;
static char *yo_config_model = NULL;  /* model from ~/.yoconf, before env override */
static char *yo_chat_prefix = NULL;   /* from ~/.yoconf chat_prefix, default: empty */
static char *yo_color_prefix = NULL;  /* from ~/.yoconf color_prefix, default: cyan italic */
static char *yo_color_reset = NULL;   /* from ~/.yoconf color_reset, default: reset */
static char *yo_enable_italic = NULL;   /* from ~/.yoconf enable_italic */
static char *yo_disable_italic = NULL;  /* from ~/.yoconf disable_italic */
static char *yo_enable_bold = NULL;     /* from ~/.yoconf enable_bold */
static char *yo_disable_bold = NULL;    /* from ~/.yoconf disable_bold */
static char *yo_enable_strikethrough = NULL;  /* from ~/.yoconf enable_strikethrough */
static char *yo_disable_strikethrough = NULL; /* from ~/.yoconf disable_strikethrough */
static char *yo_code_delimiter = NULL;  /* from ~/.yoconf code_delimiter */
static int yo_config_scrollback_enabled = -1; /* -1 = not set (use default: enabled) */
static long yo_config_scrollback_bytes = -1;  /* -1 = not set (use default) */
static int yo_config_scrollback_lines = -1;   /* -1 = not set (use default) */
static char *yo_base_url = NULL;              /* from ~/.yoconf base_url, overrides default API URL */
static long yo_config_context_window = -1;    /* from ~/.yoconf context_window, -1 = unset (0 also means unset) */
static long yo_config_max_output_tokens = -1; /* from ~/.yoconf max_output_tokens, -1 = unset (0 also means unset) */
static int yo_config_thinking = YO_THINKING_UNSET; /* from ~/.yoconf thinking (-1 unset, 0 off, else level) */

/* OpenRouter API style, from the ~/.yoconf "openrouter_api" directive.
   OpenRouter supports both the OpenAI Chat Completions API and the OpenAI
   Responses API; the style selects which request builder/parser is used.
   Only meaningful when the provider is openrouter (default: chat). */
#define YO_OPENROUTER_API_CHAT      0
#define YO_OPENROUTER_API_RESPONSES 1
static int yo_openrouter_api_style = YO_OPENROUTER_API_CHAT;

/* Whether to ask Responses API providers for encrypted reasoning content via
   "include": ["reasoning.encrypted_content"], from the ~/.yoconf
   "include_reasoning" directive.  YO_INCLUDE_REASONING_UNSET means the user
   did not configure it and per-provider defaults apply:
   openai — yes when the model plausibly supports reasoning; meta — always
   (Muse Spark is a reasoning model); openrouter — no (OpenRouter only
   forwards encrypted reasoning for some models; sending "include" can 400). */
#define YO_INCLUDE_REASONING_UNSET (-1)
static int yo_include_reasoning = YO_INCLUDE_REASONING_UNSET;

/* Track if last command from yo was executed */
static int yo_last_was_command = 0;

/* 1 while the "Fetching model info..." indicator is on screen (see
   yo_print_fetching / yo_clear_fetching). */
static int yo_fetching_shown = 0;

/* 1 while the "[N.N%] Thinking..." indicator is on screen (set by
   yo_print_thinking, cleared by yo_clear_thinking; every code path that
   erases the thinking line goes through yo_clear_thinking, so the flag
   cannot go stale).  yo_print_fetching refuses to print while this is set:
   its leading "\r\033[K" would erase the thinking line, and nothing would
   redraw it until the LLM response arrives. */
static int yo_thinking_shown = 0;

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
    size_t capacity;        /* max buffer size (from scrollback_bytes config) */
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

enum yo_load_config_mode {
    yo_load_config_in_pty_init,
    yo_load_config_on_prompt
};
static bool yo_load_config(enum yo_load_config_mode mode);
static cJSON *yo_build_tools_anthropic(void);
static cJSON *yo_build_tools_responses_api(void);
static cJSON *yo_build_tools_responses_api_compat(int include_web_search);
static char *yo_sanitize_scrollback(const char *input);
static void yo_msg_add_tool_use(cJSON *messages, const char *tool_use_id,
                                const char *tool_name, cJSON *input,
                                const char *reasoning_content,
                                const char *reasoning_items_json);
static void yo_msg_add_tool_result(cJSON *messages, const char *tool_use_id,
                                   const char *result_content);
static cJSON *yo_build_history_tool_input(int idx);
static const char *yo_response_type_to_string(yo_response_type_t type);
static cJSON *yo_call_api(const char *query);
static cJSON *yo_call_api_with_scrollback(const char *query,
                                          const char *scrollback_request, const char *scrollback_data,
                                          const char *scrollback_tool_id,
                                          const char *reasoning_content,
                                          const char *reasoning_items_json);
static cJSON *yo_call_api_with_docs(const char *query, const char *docs_request,
                                    const char *docs_tool_id,
                                    const char *reasoning_content,
                                    const char *reasoning_items_json);
static int yo_parse_response(cJSON *tool_use, yo_response_t *resp);
static void yo_display_chat(const char *response);
static void yo_history_add(const char *query, yo_response_type_t type, const char *response, const char *tool_use_id, int executed, int pending, const char *reasoning_content, const char *reasoning_items_json);
static void yo_history_prune(void);
static int yo_estimate_tokens(void);
static int yo_estimate_request_tokens(const char *query);
static long yo_get_effective_context_window(void);
static long yo_get_max_output_tokens(void);
static long yo_usage_permille(long estimate, long window);
static int yo_compact_history(void);
static cJSON *yo_build_messages(const char *current_query);
static cJSON *yo_build_messages_with_scrollback(const char *current_query, const char *scrollback_request,
                                                 const char *scrollback_data, const char *scrollback_tool_id,
                                                 const char *reasoning_content,
                                                 const char *reasoning_items_json);
static cJSON *yo_build_messages_with_docs(const char *current_query, const char *docs_request,
                                          const char *docs_tool_id,
                                          const char *reasoning_content,
                                          const char *reasoning_items_json);
static void yo_print_error_no_newlinev(const char *msg, va_list args);
static void yo_print_error_no_newline(const char *msg, ...);
static void yo_print_error(const char *msg, ...);
static void yo_print_thinking(int pct);
static void yo_clear_thinking(void);
static void yo_print_fetching(void);
static void yo_clear_fetching(void);
static char *yo_build_config_info_lines(void);
static char *yo_shell_tuned_prompt(void);
static char *yo_prompt_append_paragraph(char *prompt, const char *para);
static char *yo_build_prompt_core(void);
static void yo_report_parse_error(cJSON *tool_use);
static const char *yo_get_chat_prefix(void);
static const char *yo_get_color_prefix(void);
static const char *yo_get_color_reset(void);
static const char *yo_get_enable_italic(void);
static const char *yo_get_disable_italic(void);
static const char *yo_get_enable_bold(void);
static const char *yo_get_disable_bold(void);
static const char *yo_get_enable_strikethrough(void);
static const char *yo_get_disable_strikethrough(void);
static const char *yo_get_code_delimiter(void);

/* Continuation hook and signal cleanup */
static int yo_continuation_hook(void);
static void yo_continuation_sigcleanup(int, void *);

/* Explanation retry - re-prompts LLM when command response is missing explanation */
static cJSON *yo_retry_for_explanation(const char *query, cJSON *original_tool_use);

/* Response type helpers */
static const char *yo_response_type_to_string(yo_response_type_t type);
static yo_response_type_t yo_response_type_from_string(const char *str);
static void yo_response_free(yo_response_t *resp);

/* Unified LLM call: call_claude → parse → handle_requests → explanation_retry */
static int yo_call_llm(const char *query, int flags, yo_response_t *resp);

/* Request handling helpers (shared by yo_call_llm internals) */
static int yo_handle_requests(const char *query,
                              yo_response_t *resp, int max_turns);
static int yo_handle_explanation_retry(const char *query,
                                       yo_response_t *resp);

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

/* Wrapper for pidfd_open(2) since glibc does not provide one. */
static int
yo_pidfd_open(pid_t pid, unsigned int flags)
{
    return (int)syscall(SYS_pidfd_open, pid, flags);
}

/* The pump loop - runs in the parent process, forwards I/O and waits for child.
   Uses pidfd to get notified of child exit via poll(), eliminating the need
   for periodic waitpid() polling with a timeout. */
static void
yo_pump_loop(void)
{
    struct pollfd fds[3];
    char buf[4096];
    ssize_t n;
    int status = 0;
    int error_exit = 0;  /* If set, exit with 1 regardless of child status */
    struct sigaction sa;
    int pidfd;
    int nfds;

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

    /* Open a pidfd for the child process.  When the child exits, the pidfd
       becomes readable (POLLIN), so we can detect child exit purely through
       poll() without needing a timeout or periodic waitpid(WNOHANG). */
    pidfd = yo_pidfd_open(yo_child_pid, 0);

    /* Set up poll fds:
       [0] = real stdin (read input from user)
       [1] = PTY master (read output from shell)
       [2] = pidfd (child exit notification) — only if pidfd_open succeeded
    */
    fds[0].fd = yo_real_stdin;
    fds[0].events = POLLIN;
    fds[1].fd = yo_pty_master;
    fds[1].events = POLLIN;
    if (pidfd >= 0)
    {
        fds[2].fd = pidfd;
        fds[2].events = POLLIN;
        nfds = 3;
    }
    else
    {
        /* pidfd_open failed (old kernel?) — we'll rely on PTY hangup */
        fds[2].fd = -1;
        fds[2].events = 0;
        nfds = 2;
    }

    for (;;)
    {
        int ret;

        ret = poll(fds, nfds, -1);  /* No timeout needed — pidfd signals child exit */

        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            goto error;
        }

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

        /* Check for hangup/error on PTY (only when no data to read) */
        if ((fds[1].revents & (POLLHUP | POLLERR)) && !(fds[1].revents & POLLIN))
            goto wait_child;

        /* Child exit notification via pidfd */
        if (pidfd >= 0 && (fds[2].revents & POLLIN))
        {
            /* Child has exited — drain any remaining PTY output */
            int fl = fcntl(yo_pty_master, F_GETFL);
            if (fl >= 0)
                fcntl(yo_pty_master, F_SETFL, fl | O_NONBLOCK);
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
            goto wait_child;
        }
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
    /* Close pidfd if we opened one */
    if (pidfd >= 0)
        close(pidfd);

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
    size_t scrollback_bytes = YO_DEFAULT_SCROLLBACK_BYTES;
    int scrollback_lines = YO_DEFAULT_SCROLLBACK_LINES;
    pthread_mutexattr_t mutex_attr;
    pid_t pid;

    /* Load config early so scrollback settings from ~/.yoconf are available.
       If config loading fails, disable scrollback rather than aborting init. */
    if (!yo_load_config(yo_load_config_in_pty_init))
    {
        yo_display_chat("Disabling scrollback because of ~/.yoconf error");
        yo_scrollback_enabled = 0;
        return 0;
    }

    /* Check if scrollback is disabled */
    if (yo_config_scrollback_enabled == 0)
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

    /* Read scrollback configuration from config statics */
    if (yo_config_scrollback_bytes > 0)
        scrollback_bytes = (size_t)yo_config_scrollback_bytes;

    if (yo_config_scrollback_lines > 0)
        scrollback_lines = yo_config_scrollback_lines;

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
    char *result = NULL;

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
        {
            if (asprintf(&result, "%s %s", name, version) < 0)
                result = name;  /* asprintf failed: fall back to the bare name */
        }
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
rl_yo_enable(const char* name, const char *system_prompt, rl_yo_docs_callback_t documentation_callback,
             rl_yo_prompt_callback_t prompt_callback)
{
    if (yo_is_enabled)
        return;

    /* Initialize PTY proxy for scrollback capture (optional - may fail silently) */
    yo_pty_init();

    yo_name = name;
    yo_documentation_callback = documentation_callback;
    yo_prompt_callback = prompt_callback;

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
        "  IMPORTANT: If the user asks you to DO something (install, remove, configure, fix,\n"
        "  create, delete, move, change, set up, etc.), you MUST respond with a command.\n"
        "  Never respond with chat when a command is needed. If you're unsure what command\n"
        "  to run or need to investigate first, use command with pending=true to run an\n"
        "  investigative command (like grep, cat, echo, which, etc.), then continue with\n"
        "  the actual fix after seeing the output.\n"
        "\n"
        "- chat: Respond with text ONLY for pure knowledge questions where no action is\n"
        "  requested (e.g. 'what does -r do', 'explain pipes', 'how does git rebase work').\n"
        "  NEVER use chat when the user is asking you to do, change, fix, or accomplish\n"
        "  something - use command instead, even if you need to investigate first.\n"
        "\n"
        "- scrollback: Request recent terminal output when you need to see what happened\n"
        "  (errors, command results, etc.). You'll get another turn to respond after.\n"
        "  Note: scrollback captures raw terminal I/O, so it may contain duplicate or\n"
        "  garbled-looking lines from readline editing (e.g. the user pressing up/down\n"
        "  arrows to navigate history). Ignore these artifacts and focus on actual output.\n"
        "\n"
        "- docs: Request %s documentation when the user asks about %s features,\n"
        "  configuration, environment variables, or usage.\n"
        "\n"
        "Multi-step sequences: When you set pending=true on a command, you'll receive a\n"
        "[continuation] message with terminal output after the user executes it. Continue\n"
        "with the next command or use chat to wrap up. If the user edited the command\n"
        "substantially, acknowledge and wrap up with chat (don't continue the sequence).\n"
        "The last command in a sequence should NOT have pending=true.",
        system_prompt, name, name);

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
        if (yo_history[i].response)
            free(yo_history[i].response);
        if (yo_history[i].tool_use_id)
            free(yo_history[i].tool_use_id);
        if (yo_history[i].reasoning_content)
            free(yo_history[i].reasoning_content);
        if (yo_history[i].reasoning_items_json)
            free(yo_history[i].reasoning_items_json);
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
   received or max_turns is exhausted.  Updates resp in place.
   Returns 1 on success, 0 on failure (error already printed, resp zeroed). */
static int
yo_handle_requests(const char *query,
                   yo_response_t *resp, int max_turns)
{
    while (max_turns > 0)
    {
        if (resp->type == YO_RESPONSE_SCROLLBACK)
        {
            char *scrollback_data = NULL;
            char *saved_tool_id = NULL;
            char *saved_content = NULL;
            cJSON *new_tool_use;
            yo_response_t new_resp;
            int lines_requested;

            /* Save the tool_use_id and content (lines count) before freeing */
            saved_tool_id = resp->tool_use_id;
            resp->tool_use_id = NULL;
            saved_content = resp->content;
            resp->content = NULL;

            lines_requested = atoi(saved_content);
            if (lines_requested <= 0) lines_requested = 50;
            if (lines_requested > 1000) lines_requested = 1000;

            scrollback_data = rl_yo_get_scrollback(lines_requested);
            if (!scrollback_data || !*scrollback_data)
            {
                if (scrollback_data) free(scrollback_data);
                scrollback_data = strdup("(No terminal output available)");
            }

            if (resp->explanation) { free(resp->explanation); resp->explanation = NULL; }
            if (resp->raw_tool_use) { cJSON_Delete(resp->raw_tool_use); resp->raw_tool_use = NULL; }

            /* Replay this turn's reasoning items in the follow-up request, then
               release them — resp is about to be replaced by the new response. */
            new_tool_use = yo_call_api_with_scrollback(
                query, saved_content, scrollback_data, saved_tool_id,
                resp->reasoning_content, resp->reasoning_items_json);
            if (resp->reasoning_content) { free(resp->reasoning_content); resp->reasoning_content = NULL; }
            if (resp->reasoning_items_json) { free(resp->reasoning_items_json); resp->reasoning_items_json = NULL; }
            free(saved_tool_id);
            free(saved_content);
            free(scrollback_data);

            if (!new_tool_use)
            {
                memset(resp, 0, sizeof(*resp));
                return 0;
            }

            memset(&new_resp, 0, sizeof(new_resp));
            if (!yo_parse_response(new_tool_use, &new_resp))
            {
                yo_report_parse_error(new_tool_use);
                cJSON_Delete(new_tool_use);
                memset(resp, 0, sizeof(*resp));
                return 0;
            }

            new_resp.raw_tool_use = new_tool_use;
            *resp = new_resp;
            max_turns--;
        }
        else if (resp->type == YO_RESPONSE_DOCS)
        {
            char *saved_tool_id = NULL;
            cJSON *new_tool_use;
            yo_response_t new_resp;

            /* Save the tool_use_id before freeing */
            saved_tool_id = resp->tool_use_id;
            resp->tool_use_id = NULL;

            free(resp->content); resp->content = NULL;
            if (resp->explanation) { free(resp->explanation); resp->explanation = NULL; }
            if (resp->raw_tool_use) { cJSON_Delete(resp->raw_tool_use); resp->raw_tool_use = NULL; }

            /* Replay this turn's reasoning items in the follow-up request, then
               release them — resp is about to be replaced by the new response. */
            new_tool_use = yo_call_api_with_docs(query, "", saved_tool_id,
                                                 resp->reasoning_content,
                                                 resp->reasoning_items_json);
            if (resp->reasoning_content) { free(resp->reasoning_content); resp->reasoning_content = NULL; }
            if (resp->reasoning_items_json) { free(resp->reasoning_items_json); resp->reasoning_items_json = NULL; }
            free(saved_tool_id);

            if (!new_tool_use)
            {
                memset(resp, 0, sizeof(*resp));
                return 0;
            }

            memset(&new_resp, 0, sizeof(new_resp));
            if (!yo_parse_response(new_tool_use, &new_resp))
            {
                yo_report_parse_error(new_tool_use);
                cJSON_Delete(new_tool_use);
                memset(resp, 0, sizeof(*resp));
                return 0;
            }

            new_resp.raw_tool_use = new_tool_use;
            *resp = new_resp;
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
   the LLM to include it.  Updates resp in place if the retry succeeds.
   Returns 1 to continue normally, 0 if the user cancelled (caller should abort). */
static int
yo_handle_explanation_retry(const char *query,
                            yo_response_t *resp)
{
    cJSON *retry_tool_use;

    if (resp->type != YO_RESPONSE_COMMAND || (resp->explanation && *resp->explanation))
        return 1;  /* No retry needed */

    retry_tool_use = yo_retry_for_explanation(query, resp->raw_tool_use);
    if (retry_tool_use)
    {
        yo_response_t r;
        memset(&r, 0, sizeof(r));
        if (yo_parse_response(retry_tool_use, &r)
            && r.type == YO_RESPONSE_COMMAND
            && r.explanation && *r.explanation)
        {
            /* Retry succeeded — use the new response */
            free(resp->content); resp->content = r.content;
            if (resp->explanation) free(resp->explanation);
            resp->explanation = r.explanation;
            if (resp->tool_use_id) free(resp->tool_use_id);
            resp->tool_use_id = r.tool_use_id;
            resp->pending = r.pending;
            cJSON_Delete(resp->raw_tool_use);
            resp->raw_tool_use = retry_tool_use;
            /* Adopt the retry's reasoning fields too.  The retry is a distinct
               LLM response: its reasoning_content / reasoning_items belong
               with ITS function_call.  Keeping the original response's copies
               would replay foreign reasoning items immediately before the
               retry's function_call on the next request, violating the
               Responses API reasoning-items ordering/pairing rule. */
            if (resp->reasoning_content) free(resp->reasoning_content);
            resp->reasoning_content = r.reasoning_content;
            r.reasoning_content = NULL;
            if (resp->reasoning_items_json) free(resp->reasoning_items_json);
            resp->reasoning_items_json = r.reasoning_items_json;
            r.reasoning_items_json = NULL;
        }
        else
        {
            /* Retry didn't produce a valid command with explanation — use original */
            if (r.content) free(r.content);
            if (r.explanation) free(r.explanation);
            if (r.tool_use_id) free(r.tool_use_id);
            if (r.reasoning_content) free(r.reasoning_content);
            if (r.reasoning_items_json) free(r.reasoning_items_json);
            cJSON_Delete(retry_tool_use);
        }
    }
    else if (yo_cancelled)
    {
        return 0;  /* Cancelled */
    }

    return 1;
}

/* **************************************************************** */
/*                                                                  */
/*                    Unified LLM Call                              */
/*                                                                  */
/* **************************************************************** */

/* Encapsulates the full LLM call chain: API call → parse → handle requests → retry.
   Returns 1 on success (resp filled in), 0 on error/cancellation (resp zeroed).
   Flags:
     YO_LLM_RETRY_EXPLANATION            — always retry if explanation missing
     YO_LLM_RETRY_EXPLANATION_IF_PENDING  — retry only if resp->pending */
static int
yo_call_llm(const char *query, int flags, yo_response_t *resp)
{
    cJSON *tool_use;
    long window;
    int est;
    long pct;

    memset(resp, 0, sizeof(*resp));

    /* Context-usage estimate + thinking indicator.  The indicator lives here
       (not at the call sites) so it can be redrawn when history compaction
       runs.  pct = estimated request size (history + system prompt + query
       + JSON slack) in per-mille of the effective context window.

       BOTH model-info accessors are resolved BEFORE the thinking indicator
       prints: yo_get_effective_context_window() and yo_get_max_output_tokens()
       can each trigger the lazy model-info fetch (which prints its own
       "Fetching model info..." indicator, whose leading "\r\033[K" erases
       whatever line is current).  A ~/.yoconf context_window/token_budget
       override makes the context-window lookup an early return WITHOUT
       touching the cache, so the max-output-tokens lookup can still be the
       first to hit the cache miss -- and without the warm-up below it would
       fire from the request builders AFTER thinking is visible, erasing it
       for the whole LLM wait.  Warming both accessors up front guarantees
       the fetch (if any) always happens -- and is replaced -- before
       thinking prints; the builders' own lookups are then cache hits.  As a
       belt-and-braces guard, yo_print_fetching() is a no-op while the
       thinking line is visible (yo_thinking_shown). */
    est = yo_estimate_request_tokens(query);
    window = yo_get_effective_context_window();
    (void)yo_get_max_output_tokens();  /* warm the model-info cache pre-thinking */
    pct = yo_usage_permille(est, window);
    yo_print_thinking((int)pct);

    /* Compact the session history when this request would exceed half of the
       effective context window.  yo_compact_history() prints its own
       "Compacting..." indicator (only when it actually attempts the
       summarization request); on failure it is best-effort — the history is
       left alone and the original usage estimate stands.  The one exception
       is Ctrl-C: a cancelled summarizer must not be followed by the main
       request (the user would have to press Ctrl-C twice, and "Cancelled"
       would be followed by the request going through anyway). */
    if (est > window / 2)
    {
        /* Ignore cancels left over from earlier requests: only a Ctrl-C that
           lands on THIS compaction attempt may abort the operation. */
        yo_cancelled = 0;

        if (yo_compact_history())
        {
            est = yo_estimate_request_tokens(query);
            pct = yo_usage_permille(est, window);
        }
        else if (yo_cancelled)
        {
            /* The summarizer request was cancelled: the HTTP layer already
               erased the "Compacting..." indicator and printed "Cancelled".
               Abort the whole operation — no indicator redraw and no main
               request.  resp is still zeroed; the callers (rl_yo_accept_line,
               yo_continuation_hook) redisplay the prompt on a 0 return. */
            return 0;
        }
        /* Redraw the indicator: "[M.N%] Thinking..." with the post-compaction
           usage on success, the original "[N.N%] Thinking..." after a
           best-effort (non-cancel) failure. */
        yo_clear_thinking();
        yo_print_thinking((int)pct);
    }

    /* Step 1: Call LLM API */
    tool_use = yo_call_api(query);
    if (!tool_use)
        return 0;  /* Error/cancellation already printed */

    /* Step 2: Parse the response */
    if (!yo_parse_response(tool_use, resp))
    {
        yo_report_parse_error(tool_use);
        cJSON_Delete(tool_use);
        memset(resp, 0, sizeof(*resp));
        return 0;
    }
    resp->raw_tool_use = tool_use;  /* Transfer ownership */

    /* Step 3: Handle scrollback/docs requests */
    if (!yo_handle_requests(query, resp, 3))
    {
        memset(resp, 0, sizeof(*resp));
        return 0;
    }

    /* Step 4: Conditionally retry for missing explanation */
    {
        int do_retry = 0;
        if (flags & YO_LLM_RETRY_EXPLANATION)
            do_retry = 1;
        else if ((flags & YO_LLM_RETRY_EXPLANATION_IF_PENDING) && resp->pending)
            do_retry = 1;

        if (do_retry)
        {
            if (!yo_handle_explanation_retry(query, resp))
            {
                /* User cancelled during retry */
                yo_response_free(resp);
                return 0;
            }
        }
    }

    return 1;
}

/* One-shot rl_startup_hook that fires on the next readline() call after
   the user executes a pending command.  Grabs scrollback, calls the LLM
   with a synthetic [continuation] query, and prefills the next command
   (or displays a chat response). */
static int
yo_continuation_hook(void)
{
    char *scrollback = NULL;
    char *cont_query = NULL;
    yo_response_t resp;

    memset(&resp, 0, sizeof(resp));

    /* One-shot: uninstall ourselves immediately, restore previous hook */
    rl_startup_hook = yo_saved_startup_hook;
    yo_saved_startup_hook = NULL;

    /* Safety check */
    if (!yo_continuation_active)
        return 0;

    /* Load config and API key */
    if (!yo_load_config(yo_load_config_on_prompt)) 
    {
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
        yo_continuation_active = 0;
        return 0;
    }

    /* Unified LLM call: call_claude → parse → handle_requests → explanation_retry */
    if (!yo_call_llm(cont_query, YO_LLM_RETRY_EXPLANATION, &resp))
    {
        yo_continuation_active = 0;
        free(cont_query);
        return 0;
    }

    /* Clear thinking indicator */
    yo_clear_thinking();

    /* Handle the response */
    if (resp.type == YO_RESPONSE_COMMAND)
    {
        if (resp.explanation && *resp.explanation)
            yo_display_chat(resp.explanation);

        /* Add continuation exchange to session history */
        yo_history_add(cont_query, resp.type, resp.content, resp.tool_use_id, 0, resp.pending, resp.reasoning_content, resp.reasoning_items_json);

        /* Prefill the command */
        rl_replace_line(resp.content, 0);
        rl_point = rl_end;
        yo_last_was_command = 1;

        /* Continue or finish? */
        yo_continuation_active = resp.pending;
        if (resp.pending)
            yo_install_continuation_sigcleanup();
    }
    else if (resp.type == YO_RESPONSE_CHAT)
    {
        yo_display_chat(resp.content);
        yo_history_add(cont_query, resp.type, resp.content, resp.tool_use_id, 1, 0, resp.reasoning_content, resp.reasoning_items_json);
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
    yo_response_free(&resp);
    free(cont_query);

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
    char *saved_query = NULL;
    yo_response_t resp;

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

    /* Save the query for potential follow-up calls */
    saved_query = strdup(rl_line_buffer);

    /* Add the yo command itself to shell history, then reset history state
       so UP arrow finds this entry and any saved line state is cleared. */
    add_history(saved_query);
    _rl_start_using_history();

    /* Handle "yo reset" — clear LLM context without calling the API */
    if (strcmp(rl_line_buffer, "yo reset") == 0)
    {
        rl_crlf();
        rl_yo_clear_history();
        yo_scrollback_clear();
        yo_continuation_active = 0;
        yo_last_was_command = 0;
        fprintf(rl_outstream, "%s%sContext reset%s\n", yo_get_chat_prefix(), yo_get_color_prefix(), yo_get_color_reset());
        fflush(rl_outstream);
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        return 0;
    }

    fprintf(rl_outstream, "\n");

    /* Load config file fresh each time (sets provider, config_model, returns key) */
    if (!yo_load_config(yo_load_config_on_prompt))
    {
        /* Error already printed by yo_load_config */
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        return 0;
    }

    /* Unified LLM call: call_claude → parse → handle_requests → explanation_retry
       (yo_call_llm prints the "[N.N%] Thinking..." indicator itself) */
    memset(&resp, 0, sizeof(resp));
    if (!yo_call_llm(saved_query, YO_LLM_RETRY_EXPLANATION_IF_PENDING, &resp))
    {
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        free(saved_query);
        return 0;
    }

    /* Clear thinking indicator on success */
    yo_clear_thinking();

    if (resp.type == YO_RESPONSE_COMMAND)
    {
        /* Command mode: replace input with generated command */

        /* Print explanation if present */
        if (resp.explanation && *resp.explanation)
        {
            yo_display_chat(resp.explanation);
        }

        /* Add to session history (not executed yet) */
        yo_history_add(saved_query, resp.type, resp.content, resp.tool_use_id, 0, resp.pending, resp.reasoning_content, resp.reasoning_items_json);

        /* Replace line with the command */
        rl_replace_line(resp.content, 0);
        rl_point = rl_end;

        /* Mark that we just generated a command */
        yo_last_was_command = 1;

        /* Set continuation state if response is pending */
        yo_continuation_active = resp.pending;
        if (resp.pending)
            yo_install_continuation_sigcleanup();

        /* Redisplay with new content */
        rl_on_new_line();
        rl_redisplay();
    }
    else if (resp.type == YO_RESPONSE_CHAT)
    {
        /* Chat mode: display response, return to fresh prompt */
        yo_display_chat(resp.content);

        /* Add to session history */
        yo_history_add(saved_query, resp.type, resp.content, resp.tool_use_id, 1, 0, resp.reasoning_content, resp.reasoning_items_json);

        /* Clear any active continuation */
        yo_continuation_active = 0;

        /* Clear the line and show fresh prompt */
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
    }
    else if (resp.type == YO_RESPONSE_SCROLLBACK)
    {
        /* Exceeded max scrollback turns */
        yo_print_error("Too many scrollback requests");
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
    }
    else
    {
        yo_print_error("Unknown response type from LLM (full tool use response: %s)",
                       resp.raw_tool_use ? cJSON_PrintUnformatted(resp.raw_tool_use) : "(null)");
    }

    free(saved_query);
    yo_response_free(&resp);

    return 0;
}

/* **************************************************************** */
/*                                                                  */
/*                    Configuration Loading                         */
/*                                                                  */
/* **************************************************************** */

/* Trim leading and trailing whitespace in place.  Returns pointer into
   the same buffer (may be advanced past leading whitespace). */
static char *
yo_trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;

    end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';

    return s;
}

/* Parse a config value with C-string escape processing and optional quoting.
   Input: pointer to the value portion of a config line (after directive + whitespace).
   - If the value starts with " or ', everything up to the matching closing quote
     is the raw value (quotes are not included in the result).
   - If unquoted, the value extends to the first # comment or end of line,
     with leading and trailing whitespace stripped.
   - In both cases, C-style escape sequences are processed: \n, \t, \\, \0NNN (octal).
   Returns a malloc'd string, or NULL on error.
   On error, sets *error_msg to a static string describing the problem. */
static char *
yo_parse_config_string(const char *input, const char **error_msg)
{
    const char *src;
    const char *end;
    char *result;
    char *dst;
    int quoted = 0;
    char quote_char = 0;

    *error_msg = NULL;

    /* Skip leading whitespace (already done by caller, but be safe) */
    while (*input == ' ' || *input == '\t')
        input++;

    if (*input == '"' || *input == '\'')
    {
        quoted = 1;
        quote_char = *input;
        input++;  /* skip opening quote */

        /* Find closing quote */
        end = input;
        while (*end && *end != quote_char)
        {
            if (*end == '\\' && end[1])
                end++;  /* skip escaped char */
            end++;
        }

        if (*end != quote_char)
        {
            *error_msg = "unterminated quoted string";
            return NULL;
        }
        /* end points at closing quote */
    }
    else
    {
        /* Unquoted: value extends to # comment or end of line */
        end = input;
        while (*end && *end != '#' && *end != '\n' && *end != '\r')
            end++;

        /* Trim trailing whitespace */
        while (end > input && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
    }

    /* Allocate result (at most end - input bytes, escapes only shrink) */
    result = malloc((size_t)(end - input) + 1);
    dst = result;
    src = input;

    while (src < end)
    {
        if (*src == '\\' && src + 1 < end)
        {
            src++;
            switch (*src)
            {
            case 'n':  *dst++ = '\n'; src++; break;
            case 't':  *dst++ = '\t'; src++; break;
            case 'r':  *dst++ = '\r'; src++; break;
            case '\\': *dst++ = '\\'; src++; break;
            case '"':  *dst++ = '"';  src++; break;
            case '\'': *dst++ = '\''; src++; break;
            case '0':
                /* Octal: \0, \0N, \0NN, \0NNN */
                {
                    unsigned int val = 0;
                    int digits = 0;
                    src++;  /* skip the '0' */
                    while (digits < 3 && src < end && *src >= '0' && *src <= '7')
                    {
                        val = val * 8 + (unsigned int)(*src - '0');
                        src++;
                        digits++;
                    }
                    *dst++ = (char)val;
                }
                break;
            default:
                /* Unknown escape — keep the backslash and character */
                *dst++ = '\\';
                *dst++ = *src++;
                break;
            }
        }
        else
        {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return result;
}

/* Read an API key from a single-line key file (e.g. ~/.anthropickey, ~/.yoshkey).
   Checks that the file has mode 0600.
   Returns malloc'd key string on success, NULL on not-found or error.
   Sets *found_out to 1 if the file exists (even if there's an error reading it),
   0 if the file simply doesn't exist.  This lets the caller distinguish
   "not found, try next" from "found but broken, stop".
   display_name is used in error messages (e.g. "~/.anthropickey"). */
static char *
yo_read_keyfile(const char *path, const char *display_name, int *found_out)
{
    struct stat st;
    FILE *fp;
    char *key;
    size_t len;
    char *end;

    if (stat(path, &st) != 0)
    {
        *found_out = 0;
        return NULL;
    }

    *found_out = 1;

    if ((st.st_mode & 0777) != 0600)
    {
        yo_print_error_no_newline(
            "%s must have mode 0600 (current: %04o)", display_name, st.st_mode & 0777);
        return NULL;
    }

    fp = fopen(path, "r");
    if (!fp)
    {
        yo_print_error_no_newline("Cannot read %s: %s", display_name, strerror(errno));
        return NULL;
    }

    key = malloc(256);
    if (!fgets(key, 256, fp))
    {
        fclose(fp);
        free(key);
        yo_print_error_no_newline("%s is empty", display_name);
        return NULL;
    }

    fclose(fp);

    /* Trim whitespace */
    len = strlen(key);
    while (len > 0 && (key[len-1] == '\n' || key[len-1] == '\r' || key[len-1] == ' ' || key[len-1] == '\t'))
        key[--len] = '\0';

    end = key;
    while (*end == ' ' || *end == '\t')
        end++;

    if (end != key)
        memmove(key, end, strlen(end) + 1);

    if (strlen(key) == 0)
    {
        free(key);
        yo_print_error_no_newline("%s is empty", display_name);
        return NULL;
    }

    return key;
}

/* Apply final config after yo_load_config has parsed ~/.yoconf and resolved the key.
   Sets yo_api_key and resolves the model (config model > provider default). */
static void
yo_finish_config(char *parsed_key)
{
    if (yo_api_key)
        free(yo_api_key);
    yo_api_key = parsed_key;

    /* Resolve model: config file model > provider default */
    if (yo_model)
    {
        free(yo_model);
        yo_model = NULL;
    }
    if (yo_config_model)
    {
        yo_model = strdup(yo_config_model);
    }
    else if (yo_provider == YO_PROVIDER_OPENAI)
    {
        yo_model = strdup(YO_DEFAULT_OPENAI_MODEL);
    }
    else if (yo_provider == YO_PROVIDER_KIMI)
    {
        yo_model = strdup(YO_DEFAULT_KIMI_MODEL);
    }
    else if (yo_provider == YO_PROVIDER_DEEPSEEK)
    {
        yo_model = strdup(YO_DEFAULT_DEEPSEEK_MODEL);
    }
    else if (yo_provider == YO_PROVIDER_QWEN)
    {
        yo_model = strdup(YO_DEFAULT_QWEN_MODEL);
    }
    else if (yo_provider == YO_PROVIDER_ZAI)
    {
        yo_model = strdup(YO_DEFAULT_ZAI_MODEL);
    }
    else if (yo_provider == YO_PROVIDER_META)
    {
        yo_model = strdup(YO_DEFAULT_META_MODEL);
    }
    else if (yo_provider == YO_PROVIDER_OPENROUTER)
    {
        yo_model = strdup(YO_DEFAULT_OPENROUTER_MODEL);
    }
    else
    {
        yo_model = strdup(YO_DEFAULT_MODEL);
    }
}

/* Load configuration from ~/.yoconf and/or provider-specific key files.
   Sets yo_provider and yo_config_model as side effects.
   Returns malloc'd API key string on success, NULL on error (error already printed).

   Key resolution order:
   1. ~/.yoconf (all fields optional: provider, model, key)
   2. If key missing and provider known: ~/.anthropickey, ~/.openaikey, ~/.kimikey,
      ~/.deepseekkey, ~/.qwenkey, ~/.zaikey, ~/.metakey, or ~/.openrouterkey
      (matching the provider)
   3. If key missing and provider unknown: ~/.anthropickey, ~/.yoshkey, ~/.openaikey,
      ~/.kimikey, ~/.deepseekkey, ~/.qwenkey, ~/.zaikey, ~/.metakey, ~/.openrouterkey
   4. Model defaults applied by yo_finish_config. */
static bool
yo_load_config(enum yo_load_config_mode mode)
{
    char *home;
    char path[1024];
    struct stat st;
    FILE *fp;
    char *parsed_key = NULL;
    int have_provider = 0;

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
        yo_print_error_no_newline("Cannot determine home directory");
        return false;
    }

    /* Reset config state */
    if (yo_config_model)
    {
        free(yo_config_model);
        yo_config_model = NULL;
    }
    if (yo_chat_prefix)
    {
        free(yo_chat_prefix);
        yo_chat_prefix = NULL;
    }
    if (yo_color_prefix)
    {
        free(yo_color_prefix);
        yo_color_prefix = NULL;
    }
    if (yo_color_reset)
    {
        free(yo_color_reset);
        yo_color_reset = NULL;
    }
    if (yo_enable_italic)
    {
        free(yo_enable_italic);
        yo_enable_italic = NULL;
    }
    if (yo_disable_italic)
    {
        free(yo_disable_italic);
        yo_disable_italic = NULL;
    }
    if (yo_enable_bold)
    {
        free(yo_enable_bold);
        yo_enable_bold = NULL;
    }
    if (yo_disable_bold)
    {
        free(yo_disable_bold);
        yo_disable_bold = NULL;
    }
    if (yo_enable_strikethrough)
    {
        free(yo_enable_strikethrough);
        yo_enable_strikethrough = NULL;
    }
    if (yo_disable_strikethrough)
    {
        free(yo_disable_strikethrough);
        yo_disable_strikethrough = NULL;
    }
    if (yo_code_delimiter)
    {
        free(yo_code_delimiter);
        yo_code_delimiter = NULL;
    }
    if (yo_base_url)
    {
        free(yo_base_url);
        yo_base_url = NULL;
    }
    yo_config_scrollback_enabled = -1;
    yo_config_scrollback_bytes = -1;
    yo_config_scrollback_lines = -1;
    yo_history_limit = YO_DEFAULT_HISTORY_LIMIT;
    yo_token_budget = YO_DEFAULT_TOKEN_BUDGET;
    yo_token_budget_set = 0;
    yo_server_web_enabled = 1;
    yo_config_context_window = -1;
    yo_config_max_output_tokens = -1;
    yo_config_thinking = YO_THINKING_UNSET;
    yo_openrouter_api_style = YO_OPENROUTER_API_CHAT;
    yo_include_reasoning = YO_INCLUDE_REASONING_UNSET;

    /* Step 1: Try ~/.yoconf — all fields optional */
    snprintf(path, sizeof(path), "%s/.yoconf", home);

    if (stat(path, &st) == 0)
    {
        char line[1024];
        char *parsed_provider = NULL;
        char *parsed_model = NULL;
        int line_num = 0;
        int had_error = 0;
        int openrouter_api_line = 0;  /* line of the openrouter_api directive (0 = absent) */

        if ((st.st_mode & 0777) != 0600)
        {
            yo_print_error_no_newline("~/.yoconf must have mode 0600 (current: %04o)",
                                      st.st_mode & 0777);
            return false;
        }

        fp = fopen(path, "r");
        if (!fp)
        {
            yo_print_error_no_newline("Cannot read ~/.yoconf: %s", strerror(errno));
            return false;
        }

        while (fgets(line, sizeof(line), fp))
        {
            char *trimmed, *directive, *value;

            line_num++;
            trimmed = yo_trim(line);

            /* Skip empty lines and comments */
            if (*trimmed == '\0' || *trimmed == '#')
                continue;

            /* Split into directive and value at first whitespace */
            directive = trimmed;
            value = trimmed;
            while (*value && *value != ' ' && *value != '\t')
                value++;

            if (*value)
            {
                *value = '\0';
                value++;
                value = yo_trim(value);
            }

            if (strcmp(directive, "provider") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline(
                        "~/.yoconf:%d: 'provider' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                if (parsed_provider) free(parsed_provider);
                parsed_provider = strdup(value);
            }
            else if (strcmp(directive, "model") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'model' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                if (parsed_model) free(parsed_model);
                parsed_model = strdup(value);
            }
            else if (strcmp(directive, "key") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'key' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                if (parsed_key) free(parsed_key);
                parsed_key = strdup(value);
            }
            else if (strcmp(directive, "chat_prefix") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: chat_prefix: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_chat_prefix) free(yo_chat_prefix);
                yo_chat_prefix = parsed;
            }
            else if (strcmp(directive, "color_prefix") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: color_prefix: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_color_prefix) free(yo_color_prefix);
                yo_color_prefix = parsed;
            }
            else if (strcmp(directive, "color_reset") == 0 || strcmp(directive, "chat_reset") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: %s: %s", line_num, directive, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_color_reset) free(yo_color_reset);
                yo_color_reset = parsed;
            }
            else if (strcmp(directive, "enable_italic") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: enable_italic: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_enable_italic) free(yo_enable_italic);
                yo_enable_italic = parsed;
            }
            else if (strcmp(directive, "disable_italic") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: disable_italic: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_disable_italic) free(yo_disable_italic);
                yo_disable_italic = parsed;
            }
            else if (strcmp(directive, "enable_bold") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: enable_bold: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_enable_bold) free(yo_enable_bold);
                yo_enable_bold = parsed;
            }
            else if (strcmp(directive, "disable_bold") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: disable_bold: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_disable_bold) free(yo_disable_bold);
                yo_disable_bold = parsed;
            }
            else if (strcmp(directive, "enable_strikethrough") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: enable_strikethrough: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_enable_strikethrough) free(yo_enable_strikethrough);
                yo_enable_strikethrough = parsed;
            }
            else if (strcmp(directive, "disable_strikethrough") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: disable_strikethrough: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_disable_strikethrough) free(yo_disable_strikethrough);
                yo_disable_strikethrough = parsed;
            }
            else if (strcmp(directive, "code_delimiter") == 0)
            {
                const char *parse_error;
                char *parsed = yo_parse_config_string(value, &parse_error);
                if (!parsed)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: code_delimiter: %s", line_num, parse_error);
                    had_error = 1;
                    break;
                }
                if (yo_code_delimiter) free(yo_code_delimiter);
                yo_code_delimiter = parsed;
            }
            else if (strcmp(directive, "scrollback_enabled") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'scrollback_enabled' requires a value (0 or 1)", line_num);
                    had_error = 1;
                    break;
                }
                yo_config_scrollback_enabled = (*value == '0') ? 0 : 1;
            }
            else if (strcmp(directive, "scrollback_bytes") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'scrollback_bytes' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                yo_config_scrollback_bytes = atol(value);
                if (yo_config_scrollback_bytes <= 0)
                    yo_config_scrollback_bytes = YO_DEFAULT_SCROLLBACK_BYTES;
            }
            else if (strcmp(directive, "scrollback_lines") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'scrollback_lines' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                yo_config_scrollback_lines = atoi(value);
                if (yo_config_scrollback_lines <= 0)
                    yo_config_scrollback_lines = YO_DEFAULT_SCROLLBACK_LINES;
            }
            else if (strcmp(directive, "history_limit") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'history_limit' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                yo_history_limit = atoi(value);
                if (yo_history_limit < 1)
                    yo_history_limit = YO_DEFAULT_HISTORY_LIMIT;
            }
            else if (strcmp(directive, "token_budget") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'token_budget' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                yo_token_budget = atoi(value);
                if (yo_token_budget < 100)
                    yo_token_budget = YO_DEFAULT_TOKEN_BUDGET;
                /* token_budget is repurposed as an alias of the context
                   window: when set, it overrides the model's context window
                   for the usage indicator and compaction threshold (the old
                   round-robin history pruning is gone — compaction replaced
                   it).  context_window takes precedence when both are set. */
                yo_token_budget_set = 1;
            }
            else if (strcmp(directive, "server_web") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'server_web' requires a value (0 or 1)", line_num);
                    had_error = 1;
                    break;
                }
                yo_server_web_enabled = (*value == '0') ? 0 : 1;
            }
            else if (strcmp(directive, "base_url") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'base_url' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                if (yo_base_url) free(yo_base_url);
                yo_base_url = strdup(value);
            }
            else if (strcmp(directive, "context_window") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'context_window' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                yo_config_context_window = atol(value);
                if (yo_config_context_window <= 0)
                    yo_config_context_window = -1;  /* 0/negative = unset */
            }
            else if (strcmp(directive, "max_output_tokens") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'max_output_tokens' requires a value", line_num);
                    had_error = 1;
                    break;
                }
                yo_config_max_output_tokens = atol(value);
                if (yo_config_max_output_tokens <= 0)
                    yo_config_max_output_tokens = -1;  /* 0/negative = unset */
            }
            else if (strcmp(directive, "thinking") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'thinking' requires a value "
                                              "(off, none, minimal, low, medium, high, xhigh, or max)", line_num);
                    had_error = 1;
                    break;
                }
                if (strcmp(value, "off") == 0 || strcmp(value, "none") == 0)
                    yo_config_thinking = YO_THINKING_OFF;
                else if (strcmp(value, "minimal") == 0)
                    yo_config_thinking = YO_THINKING_MINIMAL;
                else if (strcmp(value, "low") == 0)
                    yo_config_thinking = YO_THINKING_LOW;
                else if (strcmp(value, "medium") == 0)
                    yo_config_thinking = YO_THINKING_MEDIUM;
                else if (strcmp(value, "high") == 0)
                    yo_config_thinking = YO_THINKING_HIGH;
                else if (strcmp(value, "xhigh") == 0)
                    yo_config_thinking = YO_THINKING_XHIGH;
                else if (strcmp(value, "max") == 0)
                    yo_config_thinking = YO_THINKING_MAX;
                else
                {
                    yo_print_error_no_newline(
                        "~/.yoconf:%d: thinking: unknown level '%s' "
                        "(expected off, none, minimal, low, medium, high, xhigh, or max)",
                        line_num, value);
                    had_error = 1;
                    break;
                }
            }
            else if (strcmp(directive, "include_reasoning") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'include_reasoning' requires a value (0 or 1)",
                                              line_num);
                    had_error = 1;
                    break;
                }
                yo_include_reasoning = (*value == '0') ? 0 : 1;
            }
            else if (strcmp(directive, "openrouter_api") == 0)
            {
                if (!*value)
                {
                    yo_print_error_no_newline("~/.yoconf:%d: 'openrouter_api' requires a value "
                                              "(chat or responses)", line_num);
                    had_error = 1;
                    break;
                }
                if (strcmp(value, "chat") == 0)
                    yo_openrouter_api_style = YO_OPENROUTER_API_CHAT;
                else if (strcmp(value, "responses") == 0)
                    yo_openrouter_api_style = YO_OPENROUTER_API_RESPONSES;
                else
                {
                    yo_print_error_no_newline(
                        "~/.yoconf:%d: openrouter_api: unknown style '%s' "
                        "(expected 'chat' or 'responses')",
                        line_num, value);
                    had_error = 1;
                    break;
                }
                openrouter_api_line = line_num;
            }
            else
            {
                yo_print_error_no_newline(
                    "~/.yoconf:%d: unknown directive '%s'", line_num, directive);
                had_error = 1;
                break;
            }
        }

        fclose(fp);

        if (had_error)
        {
            if (parsed_provider) free(parsed_provider);
            if (parsed_model) free(parsed_model);
            if (parsed_key) free(parsed_key);
            if (yo_chat_prefix) { free(yo_chat_prefix); yo_chat_prefix = NULL; }
            if (yo_color_prefix) { free(yo_color_prefix); yo_color_prefix = NULL; }
            if (yo_color_reset) { free(yo_color_reset); yo_color_reset = NULL; }
            if (yo_enable_italic) { free(yo_enable_italic); yo_enable_italic = NULL; }
            if (yo_disable_italic) { free(yo_disable_italic); yo_disable_italic = NULL; }
            if (yo_enable_bold) { free(yo_enable_bold); yo_enable_bold = NULL; }
            if (yo_disable_bold) { free(yo_disable_bold); yo_disable_bold = NULL; }
            if (yo_enable_strikethrough) { free(yo_enable_strikethrough); yo_enable_strikethrough = NULL; }
            if (yo_disable_strikethrough) { free(yo_disable_strikethrough); yo_disable_strikethrough = NULL; }
            if (yo_code_delimiter) { free(yo_code_delimiter); yo_code_delimiter = NULL; }
            if (yo_base_url) { free(yo_base_url); yo_base_url = NULL; }
            yo_config_context_window = -1;
            yo_config_max_output_tokens = -1;
            yo_token_budget = YO_DEFAULT_TOKEN_BUDGET;
            yo_token_budget_set = 0;
            yo_config_thinking = YO_THINKING_UNSET;
            yo_openrouter_api_style = YO_OPENROUTER_API_CHAT;
            yo_include_reasoning = YO_INCLUDE_REASONING_UNSET;
            return false;
        }

        /* Apply provider if specified */
        if (parsed_provider)
        {
            if (strcmp(parsed_provider, "anthropic") == 0)
            {
                yo_provider = YO_PROVIDER_ANTHROPIC;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "openai") == 0)
            {
                yo_provider = YO_PROVIDER_OPENAI;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "kimi") == 0)
            {
                yo_provider = YO_PROVIDER_KIMI;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "deepseek") == 0)
            {
                yo_provider = YO_PROVIDER_DEEPSEEK;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "qwen") == 0)
            {
                yo_provider = YO_PROVIDER_QWEN;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "z.ai") == 0)
            {
                yo_provider = YO_PROVIDER_ZAI;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "zai") == 0)
            {
                yo_provider = YO_PROVIDER_ZAI;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "meta") == 0)
            {
                yo_provider = YO_PROVIDER_META;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "muse") == 0)
            {
                yo_provider = YO_PROVIDER_META;
                have_provider = 1;
            }
            else if (strcmp(parsed_provider, "openrouter") == 0)
            {
                yo_provider = YO_PROVIDER_OPENROUTER;
                have_provider = 1;
            }
            else
            {
                yo_print_error_no_newline(
                    "~/.yoconf: unknown provider '%s' (expected 'anthropic', 'openai', 'kimi', "
                    "'deepseek', 'qwen', 'zai', 'z.ai', 'meta', 'muse', or 'openrouter')",
                    parsed_provider);
                free(parsed_provider);
                if (parsed_model) free(parsed_model);
                if (parsed_key) free(parsed_key);
                return false;
            }
            free(parsed_provider);
        }

        /* Validate openrouter_api against the provider.  The directive may
           appear before or after the provider directive, so this check runs
           after the whole file has been parsed. */
        if (openrouter_api_line && yo_provider != YO_PROVIDER_OPENROUTER)
        {
            yo_print_error_no_newline(
                "~/.yoconf:%d: openrouter_api only applies to provider 'openrouter'",
                openrouter_api_line);
            if (parsed_model) free(parsed_model);
            if (parsed_key) free(parsed_key);
            yo_config_model = NULL;
            yo_openrouter_api_style = YO_OPENROUTER_API_CHAT;
            return false;
        }

        /* Apply model from config */
        yo_config_model = parsed_model;  /* may be NULL, that's fine */

        /* If we got the key from yoconf, we're done */
        if (parsed_key)
        {
            /* Default provider to Anthropic if not specified */
            if (!have_provider)
            {
                yo_provider = YO_PROVIDER_ANTHROPIC;
                have_provider = 1;
            }
            yo_finish_config(parsed_key);
            return true;
        }
    }

    if (mode == yo_load_config_in_pty_init)
        return true;
    
    /* Step 2: Key not found in yoconf (or yoconf doesn't exist).
       Try provider-specific key files. */

    if (have_provider)
    {
        int found = 0;
        const char *key_filename;

        /* Provider was set by yoconf — check the matching key file */
        switch (yo_provider)
        {
            case YO_PROVIDER_ANTHROPIC: key_filename = ".anthropickey"; break;
            case YO_PROVIDER_KIMI:      key_filename = ".kimikey"; break;
            case YO_PROVIDER_OPENAI:    key_filename = ".openaikey"; break;
            case YO_PROVIDER_DEEPSEEK:  key_filename = ".deepseekkey"; break;
            case YO_PROVIDER_QWEN:      key_filename = ".qwenkey"; break;
            case YO_PROVIDER_ZAI:       key_filename = ".zaikey"; break;
            case YO_PROVIDER_META:      key_filename = ".metakey"; break;
            case YO_PROVIDER_OPENROUTER: key_filename = ".openrouterkey"; break;
            default:                    key_filename = ".openaikey"; break;
        }

        snprintf(path, sizeof(path), "%s/%s", home, key_filename);
        {
            char display_path[128];
            snprintf(display_path, sizeof(display_path), "~/%s", key_filename);
            parsed_key = yo_read_keyfile(path, display_path, &found);
        }

        if (parsed_key)
        {
            yo_finish_config(parsed_key);
            return true;
        }

        if (found)
            return false;  /* File existed but had an error — already printed */

        if (mode == yo_load_config_in_pty_init)
            return true;
        yo_print_error_no_newline(
            "~/.yoconf specifies provider '%s' but no key. "
            "Add 'key' to ~/.yoconf or create ~/%s (mode 0600).",
            yo_provider_to_string(yo_provider),
            key_filename);
        return false;
    }

    /* Step 3: Neither key nor provider known — try the fallback chain.
       ~/.anthropickey -> ~/.yoshkey -> ~/.openaikey -> ~/.kimikey -> ~/.deepseekkey -> ~/.qwenkey -> ~/.zaikey
       If a file exists but has an error, stop immediately. */

    {
        int found = 0;

        snprintf(path, sizeof(path), "%s/.anthropickey", home);
        parsed_key = yo_read_keyfile(path, "~/.anthropickey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_ANTHROPIC;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.yoshkey", home);
        parsed_key = yo_read_keyfile(path, "~/.yoshkey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_ANTHROPIC;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.openaikey", home);
        parsed_key = yo_read_keyfile(path, "~/.openaikey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_OPENAI;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.kimikey", home);
        parsed_key = yo_read_keyfile(path, "~/.kimikey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_KIMI;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.deepseekkey", home);
        parsed_key = yo_read_keyfile(path, "~/.deepseekkey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_DEEPSEEK;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.qwenkey", home);
        parsed_key = yo_read_keyfile(path, "~/.qwenkey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_QWEN;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.zaikey", home);
        parsed_key = yo_read_keyfile(path, "~/.zaikey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_ZAI;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.metakey", home);
        parsed_key = yo_read_keyfile(path, "~/.metakey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_META;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;

        snprintf(path, sizeof(path), "%s/.openrouterkey", home);
        parsed_key = yo_read_keyfile(path, "~/.openrouterkey", &found);
        if (parsed_key)
        {
            yo_provider = YO_PROVIDER_OPENROUTER;
            yo_finish_config(parsed_key);
            return true;
        }
        if (found)
            return false;
    }

    yo_print_error_no_newline("No API key found. Create ~/.yoconf with your API key (mode 0600), "
                              "or create ~/.anthropickey, ~/.yoshkey, ~/.openaikey, ~/.kimikey, "
                              "~/.deepseekkey, ~/.qwenkey, ~/.zaikey, ~/.metakey, or ~/.openrouterkey (mode 0600). "
                              "See 'yo how do I configure the LLM' for details.");
    return false;
}

/* **************************************************************** */
/*                                                                  */
/*                       LLM API Call                               */
/*                                                                  */
/* **************************************************************** */

/* Build the tools array for the Anthropic API request.
   Converts common tool definitions to Anthropic JSON format and appends
   Anthropic-specific server tools (web_search, web_fetch). */
static cJSON *
yo_build_tools_anthropic(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool, *schema, *json_props, *prop, *required;
    char *docs_desc;

    /* Tool: command */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "command");
    cJSON_AddStringToObject(tool, "description",
        "Generate a shell command for the user to review and execute. "
        "The command will be prefilled at the prompt for the user to edit or run.");
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "The shell command to execute");
    cJSON_AddItemToObject(json_props, "command", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description",
        "Brief explanation of what this command does, shown to user before the command");
    cJSON_AddItemToObject(json_props, "explanation", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "boolean");
    cJSON_AddStringToObject(prop, "description",
        "Set to true if this is part of a multi-step sequence and you need to see "
        "the output before providing the next command. After the user executes this "
        "command, you will automatically receive the terminal output.");
    cJSON_AddItemToObject(json_props, "pending", prop);
    cJSON_AddItemToObject(schema, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToArray(required, cJSON_CreateString("explanation"));
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: chat */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "chat");
    cJSON_AddStringToObject(tool, "description",
        "Respond with a text message for questions and explanations; use ONLY when no command is needed.");
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "Your text response to the user");
    cJSON_AddItemToObject(json_props, "response", prop);
    cJSON_AddItemToObject(schema, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("response"));
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: scrollback */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "scrollback");
    cJSON_AddStringToObject(tool, "description",
        "Request recent terminal output to see command results, error messages, or context. "
        "Use this when you need to see what happened in the terminal.");
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "integer");
    cJSON_AddStringToObject(prop, "description", "Number of recent lines to retrieve (max 1000)");
    cJSON_AddItemToObject(json_props, "lines", prop);
    cJSON_AddItemToObject(schema, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("lines"));
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: docs */
    tool = cJSON_CreateObject();
    if (asprintf(&docs_desc,
             "Request %s documentation to answer questions about %s features, configuration, "
             "environment variables, LLM provider/model or API key setup, or usage.",
             yo_name, yo_name) < 0)
        docs_desc = NULL;  /* asprintf failed: omit the description below */
    cJSON_AddStringToObject(tool, "name", "docs");
    if (docs_desc)
        cJSON_AddStringToObject(tool, "description", docs_desc);
    free(docs_desc);
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    json_props = cJSON_CreateObject();
    cJSON_AddItemToObject(schema, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "input_schema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* Anthropic-specific server tools: web_search and web_fetch */
    if (yo_server_web_enabled)
    {
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "web_search_20250305");
        cJSON_AddStringToObject(tool, "name", "web_search");
        cJSON_AddNumberToObject(tool, "max_uses", 5);
        cJSON_AddItemToArray(tools, tool);

        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "web_fetch_20250910");
        cJSON_AddStringToObject(tool, "name", "web_fetch");
        cJSON_AddNumberToObject(tool, "max_uses", 3);
        cJSON_AddItemToArray(tools, tool);
    }

    return tools;
}

/* Build tools array in Chat Completions API format.
   Similar to Responses API format but without strict mode and additionalProperties
   which Chat Completions API providers don't support.
   web_search_enabled is ignored for Chat Completions API providers (always 0). */
static cJSON *
yo_build_tools_chat_completions_api(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool, *func, *params, *json_props, *prop, *required;
    char *docs_desc;

    /* Tool: command */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    func = cJSON_CreateObject();
    cJSON_AddStringToObject(func, "name", "command");
    cJSON_AddStringToObject(func, "description",
        "Generate a shell command for the user to review and execute. "
        "The command will be prefilled at the prompt for the user to edit or run. "
        "CRITICAL: If you recommend any command, you MUST use this tool. "
        "Do NOT respond with chat that suggests a command. "
        "Keep commands short and readable (prefer single-line commands). "
        "Do NOT emit large here-docs or long multi-line scripts. "
        "If a solution would be long, split into multiple steps using pending=true.");
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "The shell command to execute");
    cJSON_AddItemToObject(json_props, "command", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description",
        "Brief explanation of what this command does, shown to user before the command");
    cJSON_AddItemToObject(json_props, "explanation", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "boolean");
    cJSON_AddStringToObject(prop, "description",
        "Set to true if this is part of a multi-step sequence and you need to see "
        "the output before providing the next command. After the user executes this "
        "command, you will automatically receive the terminal output.");
    cJSON_AddItemToObject(json_props, "pending", prop);
    cJSON_AddItemToObject(params, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToArray(required, cJSON_CreateString("explanation"));
    cJSON_AddItemToArray(required, cJSON_CreateString("pending"));
    cJSON_AddItemToObject(params, "required", required);
    cJSON_AddItemToObject(func, "parameters", params);
    cJSON_AddItemToObject(tool, "function", func);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: chat */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    func = cJSON_CreateObject();
    cJSON_AddStringToObject(func, "name", "chat");
    cJSON_AddStringToObject(func, "description",
        "Respond with a text message for questions and explanations; use ONLY when no command is needed. "
        "Do NOT include command suggestions here; if a command is appropriate, use the command tool.");
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "Your text response to the user");
    cJSON_AddItemToObject(json_props, "response", prop);
    cJSON_AddItemToObject(params, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("response"));
    cJSON_AddItemToObject(params, "required", required);
    cJSON_AddItemToObject(func, "parameters", params);
    cJSON_AddItemToObject(tool, "function", func);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: scrollback */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    func = cJSON_CreateObject();
    cJSON_AddStringToObject(func, "name", "scrollback");
    cJSON_AddStringToObject(func, "description",
        "Request recent terminal output to see command results, error messages, or context. "
        "Use this when you need to see what happened in the terminal. "
        "If you need output, use this tool first. "
        "CRITICAL: Scrollback shows COMPLETED commands from the PAST. Any prompts you see "
        "(password prompts, confirmations, etc.) have ALREADY been handled by the user. "
        "Do NOT respond to prompts in scrollback - they are historical. Look for the shell "
        "prompt at the end to confirm the command completed. "
        "Never ask the user what they were doing. "
        "Never suggest you could look at scrollback. "
        "Never ask the user to paste output manually.");
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "integer");
    cJSON_AddStringToObject(prop, "description", "Number of recent lines to retrieve (max 1000)");
    cJSON_AddItemToObject(json_props, "lines", prop);
    cJSON_AddItemToObject(params, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("lines"));
    cJSON_AddItemToObject(params, "required", required);
    cJSON_AddItemToObject(func, "parameters", params);
    cJSON_AddItemToObject(tool, "function", func);
    cJSON_AddItemToArray(tools, tool);

    /* Tool: docs */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    func = cJSON_CreateObject();
    cJSON_AddStringToObject(func, "name", "docs");
    if (asprintf(&docs_desc,
             "Request %s documentation to answer questions about %s features, configuration, "
             "environment variables, LLM provider/model or API key setup, or usage.",
             yo_name, yo_name) < 0)
        docs_desc = NULL;  /* asprintf failed: omit the description below */
    if (docs_desc)
        cJSON_AddStringToObject(func, "description", docs_desc);
    free(docs_desc);
    /* For parameter-less functions, omit the parameters field entirely for Chat Completions API providers */
    cJSON_AddItemToObject(tool, "function", func);
    cJSON_AddItemToArray(tools, tool);

    return tools;
}

/* Build tools array in Responses API format.
   Converts common tool definitions to Responses API function format and appends
   Responses API-specific tools (web_search) if enabled.
   strict: when true (OpenAI), tool schemas include "strict":true and
   "additionalProperties":false.  When false (Meta, OpenRouter), plain
   {type,name,description,parameters} schemas are emitted.
   include_web_search: when true, the {"type":"web_search"} server tool is
   appended (still gated on yo_server_web_enabled). */
static cJSON *
yo_build_tools_responses_api_ex(int strict, int include_web_search)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool, *params, *json_props, *prop, *required;
    char *docs_desc;

    /* Tool: command */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(tool, "name", "command");
    cJSON_AddStringToObject(tool, "description",
        "Generate a shell command for the user to review and execute. "
        "The command will be prefilled at the prompt for the user to edit or run. "
        "CRITICAL: If you recommend any command, you MUST use this tool. "
        "Do NOT respond with chat that suggests a command. "
        "Keep commands short and readable (prefer single-line commands). "
        "Do NOT emit large here-docs or long multi-line scripts. "
        "If a solution would be long, split into multiple steps using pending=true.");
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "The shell command to execute");
    cJSON_AddItemToObject(json_props, "command", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description",
        "Brief explanation of what this command does, shown to user before the command");
    cJSON_AddItemToObject(json_props, "explanation", prop);
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "boolean");
    cJSON_AddStringToObject(prop, "description",
        "Set to true if this is part of a multi-step sequence and you need to see "
        "the output before providing the next command. After the user executes this "
        "command, you will automatically receive the terminal output.");
    cJSON_AddItemToObject(json_props, "pending", prop);
    cJSON_AddItemToObject(params, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));
    cJSON_AddItemToArray(required, cJSON_CreateString("explanation"));
    cJSON_AddItemToArray(required, cJSON_CreateString("pending"));
    cJSON_AddItemToObject(params, "required", required);
    if (strict)
        cJSON_AddFalseToObject(params, "additionalProperties");
    cJSON_AddItemToObject(tool, "parameters", params);
    if (strict)
        cJSON_AddTrueToObject(tool, "strict");
    cJSON_AddItemToArray(tools, tool);

    /* Tool: chat */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(tool, "name", "chat");
    cJSON_AddStringToObject(tool, "description",
        "Respond with a text message for questions and explanations; use ONLY when no command is needed. "
        "Do NOT include command suggestions here; if a command is appropriate, use the command tool.");
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", "Your text response to the user");
    cJSON_AddItemToObject(json_props, "response", prop);
    cJSON_AddItemToObject(params, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("response"));
    cJSON_AddItemToObject(params, "required", required);
    if (strict)
        cJSON_AddFalseToObject(params, "additionalProperties");
    cJSON_AddItemToObject(tool, "parameters", params);
    if (strict)
        cJSON_AddTrueToObject(tool, "strict");
    cJSON_AddItemToArray(tools, tool);

    /* Tool: scrollback */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(tool, "name", "scrollback");
    cJSON_AddStringToObject(tool, "description",
        "Request recent terminal output to see command results, error messages, or context. "
        "Use this when you need to see what happened in the terminal. "
        "If you need output, use this tool first. "
        "Never ask the user what they were doing. "
        "Never suggest you could look at scrollback. "
        "Never ask the user to paste output manually.");
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    json_props = cJSON_CreateObject();
    prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "integer");
    cJSON_AddStringToObject(prop, "description", "Number of recent lines to retrieve (max 1000)");
    cJSON_AddItemToObject(json_props, "lines", prop);
    cJSON_AddItemToObject(params, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("lines"));
    cJSON_AddItemToObject(params, "required", required);
    if (strict)
        cJSON_AddFalseToObject(params, "additionalProperties");
    cJSON_AddItemToObject(tool, "parameters", params);
    if (strict)
        cJSON_AddTrueToObject(tool, "strict");
    cJSON_AddItemToArray(tools, tool);

    /* Tool: docs */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(tool, "name", "docs");
    if (asprintf(&docs_desc,
             "Request %s documentation to answer questions about %s features, configuration, "
             "environment variables, LLM provider/model or API key setup, or usage.",
             yo_name, yo_name) < 0)
        docs_desc = NULL;  /* asprintf failed: omit the description below */
    if (docs_desc)
        cJSON_AddStringToObject(tool, "description", docs_desc);
    free(docs_desc);
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "type", "object");
    json_props = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "properties", json_props);
    required = cJSON_CreateArray();
    cJSON_AddItemToObject(params, "required", required);
    if (strict)
        cJSON_AddFalseToObject(params, "additionalProperties");
    cJSON_AddItemToObject(tool, "parameters", params);
    if (strict)
        cJSON_AddTrueToObject(tool, "strict");
    cJSON_AddItemToArray(tools, tool);

    /* Responses API-specific: web_search tool */
    if (include_web_search && yo_server_web_enabled)
    {
        cJSON *ws = cJSON_CreateObject();
        cJSON_AddStringToObject(ws, "type", "web_search");
        cJSON_AddItemToArray(tools, ws);
    }

    return tools;
}

/* Build tools array in Responses API format for OpenAI (strict schemas
   enabled, web_search tool when yo_server_web_enabled). */
static cJSON *
yo_build_tools_responses_api(void)
{
    return yo_build_tools_responses_api_ex(1, yo_server_web_enabled);
}

/* Build tools array in Responses API format WITHOUT OpenAI strict-mode extras
   ("strict":true / "additionalProperties":false): plain {type, name,
   description, parameters} entries with top-level name/description/parameters
   (no nested "function" object).  Used by providers that speak the Responses
   API but reject strict tool schemas (Meta Muse, OpenRouter).
   include_web_search: append {"type":"web_search"} when yo_server_web_enabled. */
static cJSON *
yo_build_tools_responses_api_compat(int include_web_search)
{
    return yo_build_tools_responses_api_ex(0, include_web_search);
}

/* Remove ANSI escape sequences and other control characters from scrollback
   before sending it to providers that need sanitized scrollback (Anthropic already handles raw scrollback well). */
static char *
yo_sanitize_scrollback(const char *input)
{
    size_t in_len, out_len = 0;
    char *out;
    size_t i = 0;

    if (!input)
        return strdup("");

    in_len = strlen(input);
    out = malloc(in_len + 1);
    if (!out)
        return strdup(input);

    while (i < in_len)
    {
        unsigned char c = (unsigned char)input[i];

        if (c == 0x1b) /* ESC */
        {
            size_t j = i + 1;
            if (j < in_len)
            {
                unsigned char next = (unsigned char)input[j];
                if (next == '[') /* CSI */
                {
                    j++;
                    while (j < in_len)
                    {
                        unsigned char ch = (unsigned char)input[j];
                        if (ch >= 0x40 && ch <= 0x7e)
                        {
                            j++;
                            break;
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
                else if (next == ']') /* OSC */
                {
                    j++;
                    while (j < in_len)
                    {
                        unsigned char ch = (unsigned char)input[j];
                        if (ch == 0x07) /* BEL */
                        {
                            j++;
                            break;
                        }
                        if (ch == 0x1b && (j + 1) < in_len && input[j + 1] == '\\')
                        {
                            j += 2;
                            break;
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
                else if (next == 'P' || next == '^' || next == '_') /* DCS, PM, APC */
                {
                    j++;
                    while (j < in_len)
                    {
                        unsigned char ch = (unsigned char)input[j];
                        if (ch == 0x1b && (j + 1) < in_len && input[j + 1] == '\\')
                        {
                            j += 2;
                            break;
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
                else
                {
                    /* Skip simple ESC sequences (ESC + one char) */
                    i = j + 1;
                    continue;
                }
            }
        }

        /* Strip other control chars except \n and \t */
        if (c < 0x20 && c != '\n' && c != '\t')
        {
            i++;
            continue;
        }

        out[out_len++] = (char)c;
        i++;
    }

    out[out_len] = '\0';
    return out;
}

/* **************************************************************** */
/*                                                                  */
/*                  Shared HTTP Infrastructure                      */
/*                                                                  */
/* **************************************************************** */

/* Shared HTTP request core used by yo_http_post and yo_http_get.
   Performs the curl multi-handle loop with Ctrl-C (self-pipe) cancellation.
   `body` is the request body for POSTs (ignored when use_get is set);
   when use_get is set, a plain GET with no body is issued.
   When quiet is set, no error/cancellation chatter is printed and the
   thinking indicator is left alone (used for best-effort background
   fetches such as model info).
   Returns malloc'd response body on success, NULL on error/cancel.
   The caller owns the returned string and must free it.
   The headers list is freed by this function. */
static char *
yo_http_perform(const char *url, struct curl_slist *headers,
                const char *body, long timeout, int use_get, int quiet)
{
    CURL *curl;
    CURLM *multi;
    yo_response_buffer_t response_buf = {0};
    struct sigaction sa, old_sa;
    int still_running = 1;
    int cancelled = 0;

    /* Initialize self-pipe for Ctrl-C handling */
    if (yo_init_sigint_pipe() < 0)
    {
        if (!quiet)
        {
            yo_clear_thinking();
            yo_print_error_no_newline("Failed to initialize signal handling: %s", strerror(errno));
        }
        curl_slist_free_all(headers);
        return NULL;
    }

    /* Drain any stale signals and reset cancelled flag */
    yo_drain_sigint_pipe();

    curl = curl_easy_init();
    if (!curl)
    {
        if (!quiet)
        {
            yo_clear_thinking();
            yo_print_error_no_newline("Failed to initialize HTTP client (curl_easy_init returned NULL)");
        }
        curl_slist_free_all(headers);
        return NULL;
    }

    multi = curl_multi_init();
    if (!multi)
    {
        curl_easy_cleanup(curl);
        if (!quiet)
        {
            yo_clear_thinking();
            yo_print_error_no_newline("Failed to initialize HTTP client (curl_multi_init returned NULL)");
        }
        curl_slist_free_all(headers);
        return NULL;
    }

    /* Configure CURL easy handle */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (use_get)
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    else
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, yo_curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    if (timeout > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

    /* Add easy handle to multi handle */
    curl_multi_add_handle(multi, curl);

    /* Install our SIGINT handler for the duration of the request */
    sa.sa_handler = yo_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sa);

    /* Multi interface loop with curl_multi_poll() */
    {
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
            if (!quiet)
            {
                yo_clear_thinking();
                yo_print_error_no_newline("HTTP error: %s", curl_multi_strerror(mc));
            }
            goto http_error;
        }
    }

    /* Handle cancellation */
    if (cancelled)
    {
        if (!quiet)
        {
            yo_clear_thinking();
            fprintf(rl_outstream, "%s%sCancelled%s\n", yo_get_chat_prefix(), yo_get_color_prefix(), yo_get_color_reset());
            fflush(rl_outstream);
        }
        goto http_error;
    }

    {
        CURLMsg *msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(multi, &msgs_left)))
        {
            if (msg->msg == CURLMSG_DONE)
            {
                CURL *easy = msg->easy_handle;
                ZASSERT(easy == curl);
                CURLcode result = msg->data.result;

                if (result != CURLE_OK)
                {
                    if (!quiet)
                    {
                        yo_clear_thinking();
                        yo_print_error_no_newline("HTTP error: %s", curl_easy_strerror(result));
                    }
                    goto http_error;
                }

                {
                    long http_code = 0;
                    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
                    if (http_code == 200)
                        break;

                    if (!quiet)
                    {
                        yo_clear_thinking();
                        if (response_buf.data) {
                            yo_print_error_no_newline(
                                "Unexpected HTTP status code: %ld; full response: %s",
                                http_code, response_buf.data);
                        } else
                            yo_print_error_no_newline("Unexpected HTTP status code: %ld", http_code);
                    }
                    goto http_error;
                }
            }
        }
    }

    /* Clean up curl handles */
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!response_buf.data)
    {
        if (!quiet)
        {
            yo_clear_thinking();
            yo_print_error_no_newline("No response from API");
        }
        return NULL;
    }

    return response_buf.data;

http_error:
    if (response_buf.data)
        free(response_buf.data);
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return NULL;
}

/* Make an HTTP POST request with curl multi-handle and Ctrl-C cancellation.
   Returns malloc'd response body on success, NULL on error/cancel.
   The caller owns the returned string and must free it.
   On error, an error message is already printed.
   The headers list is freed by this function. */
static char *
yo_http_post(const char *url, struct curl_slist *headers,
             const char *body, long timeout)
{
    return yo_http_perform(url, headers, body, timeout, 0, 0);
}

/* Make an HTTP GET request with no body, mirroring yo_http_post.
   Uses the same curl multi-handle and Ctrl-C cancellation loop.
   Returns malloc'd response body on success, NULL on error/cancel.
   The caller owns the returned string and must free it.
   On error, an error message is already printed.
   The headers list is freed by this function. */
static char *
yo_http_get(const char *url, struct curl_slist *headers, long timeout)
{
    return yo_http_perform(url, headers, NULL, timeout, 1, 0);
}

/* Silent variant of yo_http_get for best-effort background fetches
   (e.g. model info): on error/cancel it just returns NULL without
   printing anything or touching the thinking indicator. */
static char *
yo_http_get_quiet(const char *url, struct curl_slist *headers, long timeout)
{
    return yo_http_perform(url, headers, NULL, timeout, 1, 1);
}

/* **************************************************************** */
/*                                                                  */
/*                     Model Limits & Registry                      */
/*                                                                  */
/* **************************************************************** */

/* Built-in model registry, ported from brainstorm-3
   lib/shared/model_registry.rb (MODEL_CONTEXT_WINDOWS and
   MODEL_MAX_OUTPUT_TOKENS).  Matching is case-insensitive on the model
   name prefix and the first matching entry wins, so more-specific
   prefixes must appear before less-specific ones.  Vendor-prefixed IDs
   ("vendor/model", e.g. OpenRouter's "meta/muse-spark-1.3") fall back to
   matching on the bare model name after the last '/'. */

typedef struct {
    const char *prefix;
    long value;
} yo_model_registry_entry_t;

/* Model-to-context-window lookup table. */
static const yo_model_registry_entry_t yo_model_context_windows[] = {
    /* OpenAI models — values from OpenAI API docs, mid-2025.
       More-specific prefixes must appear before less-specific ones. */
    { "gpt-5.4-mini", 400000 },      /* mini/nano variants share the smaller 400K window */
    { "gpt-5.4-nano", 400000 },
    { "gpt-5.4", 1048576 },          /* GPT-5.4 / 5.4 Pro: 1M context */
    { "gpt-5.2", 400000 },
    { "gpt-5.1", 400000 },
    { "gpt-5", 256000 },
    { "gpt-4.1-nano", 1047576 },
    { "gpt-4.1-mini", 1047576 },
    { "gpt-4.1", 1047576 },
    { "gpt-4o-mini", 128000 },
    { "gpt-4o", 128000 },
    { "gpt-4-turbo", 128000 },
    { "gpt-4-1", 1047576 },
    { "gpt-3.5-turbo", 16385 },
    { "gpt-4", 8192 },               /* plain gpt-4 (after gpt-4o/gpt-4-turbo/gpt-4-1) */

    /* OpenAI reasoning models */
    { "o1-mini", 128000 },
    { "o1-preview", 128000 },
    { "o1", 200000 },
    { "o3-mini", 200000 },
    { "o3", 200000 },
    { "o4-mini", 200000 },

    /* Claude models (kept up to date; do not reorder) */
    { "claude-opus-4-8", 1000000 },
    { "claude-opus-4-7", 1000000 },
    { "claude-opus-4-6", 1000000 },
    { "claude-opus-4", 200000 },
    { "claude-sonnet-4-6", 1000000 },
    { "claude-sonnet-4", 200000 },
    { "claude-3-5-sonnet", 200000 },
    { "claude-3-opus", 200000 },
    { "claude-3-haiku", 200000 },
    { "claude", 200000 },            /* catch-all for any Claude variant */

    /* DeepSeek — current V4 family uses 1M context; older chat/reasoner are 64K. */
    { "deepseek-v4-pro", 1000000 },
    { "deepseek-v4-flash", 1000000 },
    { "deepseek-reasoner", 64000 },  /* R1 / V3 reasoning */
    { "deepseek-chat", 64000 },      /* V3 chat */
    { "deepseek-coder", 128000 },    /* Coder V2 family */
    { "deepseek", 128000 },          /* legacy catch-all */

    /* Kimi / Moonshot (kept up to date) */
    { "kimi-k3", 1048576 },
    { "kimi-k2.7", 262144 },
    { "kimi-k2.6", 262144 },
    { "kimi-k2.5", 256000 },
    { "kimi", 128000 },
    { "moonshot", 128000 },

    /* Qwen / Alibaba */
    { "qwen3.7-max", 262144 },
    { "qwen3.5-plus", 1000000 },
    { "qwen3.5-flash", 1000000 },
    { "qwen3-235b-a22b", 128000 },
    { "qwen-plus", 1000000 },
    { "qwen-turbo", 1000000 },
    { "qwen-coder-plus", 128000 },
    { "qwen-max", 128000 },          /* uncertain; conservative */
    { "qwen", 128000 },

    /* z.ai GLM models — OpenAI-compatible API. */
    { "glm-5.3", 1000000 },
    { "glm-5.2", 1000000 },
    { "glm-5.1", 200000 },
    { "glm-5", 200000 },
    { "glm-4.5", 128000 },
    { "glm-4-plus", 128000 },
    { "glm-4-air", 128000 },
    { "glm-4-flash", 128000 },
    { "glm-4", 128000 },             /* base GLM-4 catch-all */

    /* xAI Grok models */
    { "grok-4.20-multi-agent", 1000000 },
    { "grok-4.20-0309-reasoning", 1000000 },
    { "grok-4.20-0309-non-reasoning", 1000000 },
    { "grok-4.20", 1000000 },        /* catch-all for grok-4.20 variants */
    { "grok-4.5", 500000 },
    { "grok-4.3", 1000000 },
    { "grok-build", 256000 },
    { "grok", 500000 },              /* catch-all for future Grok models */

    /* Meta Muse models */
    { "muse-spark", 1048576 },
    { "muse", 1048576 },             /* catch-all */

    { NULL, 0 }
};

/* Model-to-max-output-tokens lookup table.  This controls the max_tokens
   parameter sent to APIs — the maximum number of tokens the model can
   generate in a single response. */
static const yo_model_registry_entry_t yo_model_max_output_tokens[] = {
    /* OpenAI models */
    { "gpt-5.4-mini", 128000 },
    { "gpt-5.4-nano", 128000 },
    { "gpt-5.4", 128000 },
    { "gpt-5.2", 64000 },
    { "gpt-5.1", 32768 },
    { "gpt-5", 32768 },
    { "gpt-4.1-nano", 32768 },
    { "gpt-4.1-mini", 32768 },
    { "gpt-4.1", 32768 },
    { "gpt-4o-mini", 16384 },
    { "gpt-4o", 16384 },
    { "gpt-4-turbo", 4096 },
    { "gpt-4-1", 32768 },
    { "gpt-3.5-turbo", 4096 },
    { "gpt-4", 8192 },

    /* OpenAI reasoning models */
    { "o1-mini", 65536 },
    { "o1-preview", 32768 },
    { "o1", 100000 },
    { "o3-mini", 100000 },
    { "o3", 100000 },
    { "o4-mini", 100000 },

    /* Claude models — official max output token limits from Anthropic docs.
       More-specific prefixes first (e.g. claude-opus-4-6 before claude-opus-4). */
    { "claude-opus-4-8", 128000 },
    { "claude-opus-4-7", 128000 },
    { "claude-opus-4-6", 128000 },
    { "claude-sonnet-4-6", 64000 },
    { "claude-opus-4-5", 64000 },
    { "claude-sonnet-4-5", 64000 },
    { "claude-haiku-4-5", 64000 },
    { "claude-sonnet-4", 64000 },
    { "claude-opus-4", 64000 },
    { "claude-3-7-sonnet", 64000 },  /* 64k with extended thinking (8,192 without) */
    { "claude-3-5-sonnet", 8192 },
    { "claude-3-opus", 4096 },
    { "claude-3-haiku", 4096 },
    { "claude", 64000 },             /* safe default for unknown future Claude models */

    /* DeepSeek — V4 models support very long outputs; older models are limited. */
    { "deepseek-v4-pro", 384000 },   /* uncertain; may be lower for some endpoints */
    { "deepseek-v4-flash", 384000 }, /* uncertain; may be lower for some endpoints */
    { "deepseek-reasoner", 8192 },
    { "deepseek-chat", 8192 },
    { "deepseek-coder", 8192 },
    { "deepseek", 8192 },

    /* Kimi / Moonshot (kept up to date) */
    { "kimi-k3", 131072 },
    { "kimi-k2.7", 262000 },
    { "kimi-k2.6", 262000 },
    { "kimi-k2.5", 16384 },
    { "kimi", 8192 },
    { "moonshot", 8192 },

    /* Qwen / Alibaba — conservative where docs are ambiguous. */
    { "qwen3.7-max", 16384 },        /* uncertain; conservative */
    { "qwen3.5-plus", 65536 },
    { "qwen3.5-flash", 65536 },
    { "qwen3-235b-a22b", 8192 },
    { "qwen-plus", 32768 },
    { "qwen-turbo", 16384 },
    { "qwen-coder-plus", 8192 },
    { "qwen-max", 8192 },            /* uncertain; conservative */
    { "qwen", 8192 },

    /* z.ai GLM models — conservative defaults where official specs are unclear. */
    { "glm-5.3", 128000 },
    { "glm-5.2", 128000 },
    { "glm-5.1", 128000 },
    { "glm-5", 128000 },
    { "glm-4.5", 32768 },            /* uncertain; official may be higher */
    { "glm-4-plus", 16384 },
    { "glm-4-air", 16384 },
    { "glm-4-flash", 16384 },
    { "glm-4", 8192 },

    /* xAI Grok models — max_output_tokens includes reasoning tokens */
    { "grok-4.20-multi-agent", 128000 },
    { "grok-4.20-0309-reasoning", 128000 },
    { "grok-4.20-0309-non-reasoning", 128000 },
    { "grok-4.20", 128000 },
    { "grok-4.5", 128000 },
    { "grok-4.3", 128000 },
    { "grok-build", 64000 },
    { "grok", 128000 },

    /* Meta Muse models */
    { "muse-spark", 128000 },
    { "muse", 128000 },

    { NULL, 0 }
};

/* Case-insensitive prefix lookup in a registry table.  First match wins.
   Returns 1 and sets *value_out when the model matches an entry,
   0 when the model is unknown to the table.

   Vendor-prefixed model IDs ("vendor/model", as used by OpenRouter — e.g.
   "meta/muse-spark-1.3") would defeat a plain prefix match, so when the
   full string matches nothing the lookup retries with the substring after
   the LAST '/' (the bare model name) before giving up.  The full string is
   always tried first, so a registry entry that includes the vendor prefix
   would still win. */
static int
yo_model_registry_lookup(const yo_model_registry_entry_t *table, const char *model,
                         long *value_out)
{
    size_t i;

    if (!model || !*model)
        return 0;

    for (i = 0; table[i].prefix; i++)
    {
        if (strncasecmp(model, table[i].prefix, strlen(table[i].prefix)) == 0)
        {
            *value_out = table[i].value;
            return 1;
        }
    }

    /* No match on the full string: retry with the bare model name when the
       ID is vendor-prefixed ("vendor/model"), e.g. "meta/muse-spark-1.3"
       must resolve through the "muse-spark"/"muse" entries. */
    {
        const char *slash = strrchr(model, '/');
        if (slash && slash[1])
        {
            const char *bare = slash + 1;

            for (i = 0; table[i].prefix; i++)
            {
                if (strncasecmp(bare, table[i].prefix, strlen(table[i].prefix)) == 0)
                {
                    *value_out = table[i].value;
                    return 1;
                }
            }
        }
    }

    return 0;
}

/* Resolve a model's context window and max output tokens from the registry.
   Uses YO_REGISTRY_DEFAULT_MAX_OUTPUT_TOKENS when the model has a context
   entry but no output entry, and the YO_UNKNOWN_MODEL_* defaults when the
   model isn't in the registry at all. */
static void
yo_model_registry_limits(const char *model, long *context_window_out,
                         long *max_output_tokens_out)
{
    long context_window;
    long max_output_tokens;

    if (yo_model_registry_lookup(yo_model_context_windows, model, &context_window))
    {
        if (!yo_model_registry_lookup(yo_model_max_output_tokens, model,
                                      &max_output_tokens))
            max_output_tokens = YO_REGISTRY_DEFAULT_MAX_OUTPUT_TOKENS;
    }
    else
    {
        context_window = YO_UNKNOWN_MODEL_CONTEXT_WINDOW;
        max_output_tokens = YO_UNKNOWN_MODEL_MAX_OUTPUT_TOKENS;
    }

    *context_window_out = context_window;
    *max_output_tokens_out = max_output_tokens;
}

/* Returns the wire string for a configured thinking level, or NULL when the
   level does not map to a request parameter (unset/off/none). */
static const char *
yo_thinking_level_string(int level)
{
    switch (level)
    {
        case YO_THINKING_MINIMAL: return "minimal";
        case YO_THINKING_LOW:     return "low";
        case YO_THINKING_MEDIUM:  return "medium";
        case YO_THINKING_HIGH:    return "high";
        case YO_THINKING_XHIGH:   return "xhigh";
        case YO_THINKING_MAX:     return "max";
        default:                  return NULL;
    }
}

/* True when the user configured a thinking level other than off/none. */
static int
yo_thinking_enabled(void)
{
    return yo_config_thinking > YO_THINKING_OFF
        && yo_thinking_level_string(yo_config_thinking) != NULL;
}

/* Anthropic extended-thinking budget ("thinking":{"budget_tokens":B}) for a
   configured thinking level.  Returns 0 for unset/off (no thinking).
   Levels map: minimal→1024, low→2048, medium→4096, high→8192,
   xhigh→16384, max→32768. */
static long
yo_thinking_budget_tokens(int level)
{
    switch (level)
    {
        case YO_THINKING_MINIMAL: return 1024;
        case YO_THINKING_LOW:     return 2048;
        case YO_THINKING_MEDIUM:  return 4096;
        case YO_THINKING_HIGH:    return 8192;
        case YO_THINKING_XHIGH:   return 16384;
        case YO_THINKING_MAX:     return 32768;
        default:                  return 0;
    }
}

/* Mirror of brainstorm-3 openai_client.rb#model_supports_reasoning_effort?:
   true for GLM-5.2 and newer GLM models (glm-5.2, glm-5.3, glm-6, ...). */
static int
yo_zai_supports_reasoning_effort(const char *model)
{
    long major = 0, minor = 0;

    if (!model || strncasecmp(model, "glm-", 4) != 0)
        return 0;
    model += 4;

    while (*model >= '0' && *model <= '9')
        major = major * 10 + (*model++ - '0');

    if (*model == '.')
    {
        model++;
        while (*model >= '0' && *model <= '9')
            minor = minor * 10 + (*model++ - '0');
    }

    return major > 5 || (major == 5 && minor >= 2);
}

/* Cached model info, keyed by (provider, model, base_url).  Re-fetched only
   when the user changes provider, model, or base_url in ~/.yoconf — not on
   every LLM call.  Failures (registry fallbacks) are cached under the same
   rule.  Nothing is fetched at startup; the first fetch is lazy. */
typedef struct {
    char *key_provider;   /* provider string at fetch time */
    char *key_model;      /* model at fetch time */
    char *key_base_url;   /* base_url at fetch time ("" when unset) */
    long context_window;
    long max_output_tokens;
    int fetched;
} yo_model_info_t;

static yo_model_info_t yo_model_info_cache;

/* Fetch obj member `name` as a positive number.  Returns 1 if found. */
static int
yo_json_get_positive_number(cJSON *obj, const char *name, long *out)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (item && cJSON_IsNumber(item) && item->valuedouble > 0)
    {
        *out = (long)item->valuedouble;
        return 1;
    }
    return 0;
}

/* Best-effort sniff: search root (and, where present, its "data",
   "data"."top_provider", "top_provider", and "model" children) for a numeric
   field with any of the given names.  Returns 1 if found. */
static int
yo_json_sniff_number(cJSON *root, const char *const *names, long *out)
{
    size_t i;
    cJSON *data, *top_provider, *model;

    for (i = 0; names[i]; i++)
        if (yo_json_get_positive_number(root, names[i], out))
            return 1;

    data = cJSON_GetObjectItem(root, "data");
    if (data && cJSON_IsObject(data))
    {
        for (i = 0; names[i]; i++)
            if (yo_json_get_positive_number(data, names[i], out))
                return 1;

        top_provider = cJSON_GetObjectItem(data, "top_provider");
        if (top_provider && cJSON_IsObject(top_provider))
            for (i = 0; names[i]; i++)
                if (yo_json_get_positive_number(top_provider, names[i], out))
                    return 1;
    }

    top_provider = cJSON_GetObjectItem(root, "top_provider");
    if (top_provider && cJSON_IsObject(top_provider))
        for (i = 0; names[i]; i++)
            if (yo_json_get_positive_number(top_provider, names[i], out))
                return 1;

    model = cJSON_GetObjectItem(root, "model");
    if (model && cJSON_IsObject(model))
        for (i = 0; names[i]; i++)
            if (yo_json_get_positive_number(model, names[i], out))
                return 1;

    return 0;
}

/* Join base URL (yo_base_url when set, otherwise default_base) with path.
   Returns malloc'd URL or NULL on allocation failure. */
static char *
yo_model_info_url(const char *default_base, const char *path)
{
    const char *base = (yo_base_url && *yo_base_url) ? yo_base_url : default_base;
    size_t base_len = strlen(base);
    const char *p = path;
    char *url;

    while (*p == '/')
        p++;

    if (base_len > 0 && base[base_len - 1] == '/')
    {
        if (asprintf(&url, "%s%s", base, p) < 0)
            return NULL;
    }
    else
    {
        if (asprintf(&url, "%s/%s", base, p) < 0)
            return NULL;
    }
    return url;
}

/* Best-effort fetch of model limits from the provider's model-info API.
   Sets *context_window_out / *max_output_tokens_out only for values the API
   actually provided.  Fully silent: on any error (missing key, timeout,
   HTTP error, unparseable response) it just returns without printing, and
   the caller fills the gaps from the registry. */
static void
yo_fetch_model_info_from_api(const char *provider, const char *model,
                             long *context_window_out, long *max_output_tokens_out)
{
    char *url = NULL;
    char *path;
    struct curl_slist *headers = NULL;
    char auth_header[300];
    char *response;
    cJSON *root;

    if (!yo_api_key || !*yo_api_key || !model || !*model)
        return;

    if (strcmp(provider, "openrouter") == 0)
    {
        /* GET {base_url or https://openrouter.ai/api/v1}/model/{model}
           (note: OpenRouter uses the SINGULAR "model" path)
           Response: {"data": {"context_length": N,
                               "top_provider": {"max_completion_tokens": M, ...}}}
           When base_url is set it is expected to already include the version
           path (e.g. https://openrouter.ai/api/v1/), so we append just
           "model/{model}" to it. */
        if (asprintf(&path, "model/%s", model) < 0)
            return;
        url = yo_model_info_url("https://openrouter.ai/api/v1", path);
        free(path);
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", yo_api_key);
        headers = curl_slist_append(NULL, auth_header);
    }
    else if (strcmp(provider, "anthropic") == 0)
    {
        /* GET {base_url or https://api.anthropic.com/v1}/models/{model}
           Best effort: use limit fields if the response happens to have them.
           When base_url is set it is expected to already include the version
           path, so we append just "models/{model}" to it. */
        if (asprintf(&path, "models/%s", model) < 0)
            return;
        url = yo_model_info_url("https://api.anthropic.com/v1", path);
        free(path);
        snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", yo_api_key);
        headers = curl_slist_append(NULL, auth_header);
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    }
    else if (strcmp(provider, "openai") == 0)
    {
        /* GET {base_url or https://api.openai.com/v1}/models/{model}
           Best effort: use limit fields if the response happens to have them.
           When base_url is set it is expected to already include the version
           path, so we append just "models/{model}" to it. */
        if (asprintf(&path, "models/%s", model) < 0)
            return;
        url = yo_model_info_url("https://api.openai.com/v1", path);
        free(path);
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", yo_api_key);
        headers = curl_slist_append(NULL, auth_header);
    }
    else if (strcmp(provider, "meta") == 0)
    {
        /* GET {base_url or https://api.meta.ai/v1}/models/{model}
           Best effort: use limit fields if the response happens to have them.
           When base_url is set it is expected to already include the version
           path (e.g. https://api.meta.ai/v1/), so we append just
           "models/{model}" to it (previously this produced .../v1/v1/models/...). */
        if (asprintf(&path, "models/%s", model) < 0)
            return;
        url = yo_model_info_url("https://api.meta.ai/v1", path);
        free(path);
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", yo_api_key);
        headers = curl_slist_append(NULL, auth_header);
    }
    else
    {
        /* kimi/deepseek/qwen/zai (and anything else): no model-info API
           attempt; go straight to the registry. */
        return;
    }

    if (!url || !headers)
    {
        free(url);
        if (headers)
            curl_slist_free_all(headers);
        return;
    }

    response = yo_http_get_quiet(url, headers, YO_MODEL_INFO_TIMEOUT);
    free(url);
    if (!response)
        return;

    root = cJSON_Parse(response);
    free(response);
    if (!root)
        return;

    if (strcmp(provider, "openrouter") == 0)
    {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (data && cJSON_IsObject(data))
        {
            yo_json_get_positive_number(data, "context_length", context_window_out);

            {
                cJSON *top_provider = cJSON_GetObjectItem(data, "top_provider");
                if (top_provider && cJSON_IsObject(top_provider))
                    yo_json_get_positive_number(top_provider, "max_completion_tokens",
                                                max_output_tokens_out);
            }
        }
    }
    else
    {
        static const char *const context_names[] = { "context_window", "context_length", NULL };
        static const char *const output_names[] = { "max_output_tokens", "max_tokens", NULL };

        yo_json_sniff_number(root, context_names, context_window_out);
        yo_json_sniff_number(root, output_names, max_output_tokens_out);
    }

    cJSON_Delete(root);
}

/* Resolve the current model's limits: ask the provider API when it can tell
   us (short timeout, silent on failure) and fill whatever is missing from
   the built-in registry. */
static void
yo_resolve_model_info(long *context_window_out, long *max_output_tokens_out)
{
    long context_window = 0;
    long max_output_tokens = 0;
    const char *provider = yo_provider_to_string(yo_provider);

    yo_fetch_model_info_from_api(provider, yo_model, &context_window, &max_output_tokens);

    {
        long registry_context, registry_max;
        yo_model_registry_limits(yo_model, &registry_context, &registry_max);
        if (context_window <= 0)
            context_window = registry_context;
        if (max_output_tokens <= 0)
            max_output_tokens = registry_max;
    }

    *context_window_out = context_window;
    *max_output_tokens_out = max_output_tokens;
}

/* True when this provider has a model-info API that
   yo_fetch_model_info_from_api will actually try (registry-only providers
   return 0 — nothing is fetched for them). */
static int
yo_provider_fetches_model_info(const char *provider)
{
    return strcmp(provider, "openrouter") == 0
        || strcmp(provider, "anthropic") == 0
        || strcmp(provider, "openai") == 0
        || strcmp(provider, "meta") == 0;
}

/* Get the context window and max output tokens for the current
   (provider, model, base_url).  Values come from the provider's API when it
   reports them, otherwise from the built-in model registry.  Results are
   cached per (provider, model, base_url) so we do NOT re-request on every
   LLM call — only when the user changes provider, model, or base_url in
   ~/.yoconf.  The fetch is lazy: nothing happens until first use.
   When a real network fetch is about to happen (providers with a model-info
   API, and only on a cache miss) the "Fetching model info..." indicator is
   printed; it disappears when the thinking indicator is printed
   (yo_print_thinking calls yo_clear_fetching), and yo_print_fetching is a
   no-op while the thinking indicator is visible -- a late cache miss can
   never erase it. */
static void
yo_get_model_info(long *context_window_out, long *max_output_tokens_out)
{
    const char *provider = yo_provider_to_string(yo_provider);
    const char *base_url = yo_base_url ? yo_base_url : "";
    int cache_valid = yo_model_info_cache.fetched
        && yo_model_info_cache.key_provider
        && yo_model_info_cache.key_model
        && yo_model_info_cache.key_base_url
        && yo_model
        && strcmp(yo_model_info_cache.key_provider, provider) == 0
        && strcmp(yo_model_info_cache.key_model, yo_model) == 0
        && strcmp(yo_model_info_cache.key_base_url, base_url) == 0;

    if (!cache_valid)
    {
        long context_window, max_output_tokens;

        if (yo_provider_fetches_model_info(provider))
            yo_print_fetching();

        yo_resolve_model_info(&context_window, &max_output_tokens);

        if (yo_model_info_cache.key_provider)
            free(yo_model_info_cache.key_provider);
        if (yo_model_info_cache.key_model)
            free(yo_model_info_cache.key_model);
        if (yo_model_info_cache.key_base_url)
            free(yo_model_info_cache.key_base_url);
        yo_model_info_cache.key_provider = strdup(provider);
        yo_model_info_cache.key_model = strdup(yo_model);
        yo_model_info_cache.key_base_url = strdup(base_url);
        yo_model_info_cache.context_window = context_window;
        yo_model_info_cache.max_output_tokens = max_output_tokens;
        yo_model_info_cache.fetched = 1;
    }

    if (context_window_out)
        *context_window_out = yo_model_info_cache.context_window;
    if (max_output_tokens_out)
        *max_output_tokens_out = yo_model_info_cache.max_output_tokens;
}

/* Max output tokens to request from the LLM: ~/.yoconf max_output_tokens
   override > model/API-reported value > 16384. */
static long
yo_get_max_output_tokens(void)
{
    if (yo_config_max_output_tokens > 0)
        return yo_config_max_output_tokens;

    {
        long context_window, max_output_tokens;
        yo_get_model_info(&context_window, &max_output_tokens);
        if (max_output_tokens > 0)
            return max_output_tokens;
    }

    return YO_UNKNOWN_MODEL_MAX_OUTPUT_TOKENS;
}

/* Context window for the current model: ~/.yoconf context_window override >
   model/API-reported value > registry default.  (Used for context-usage
   display and compaction thresholds.) */
static long
yo_get_context_window(void)
{
    if (yo_config_context_window > 0)
        return yo_config_context_window;

    {
        long context_window, max_output_tokens;
        yo_get_model_info(&context_window, &max_output_tokens);
        if (context_window > 0)
            return context_window;
    }

    return YO_UNKNOWN_MODEL_CONTEXT_WINDOW;
}

/* Effective context-window budget for the usage indicator and compaction:
   ~/.yoconf context_window override > ~/.yoconf token_budget (when explicitly
   set — it is repurposed as an alias of the context window) > the model's
   context window (API/registry). */
static long
yo_get_effective_context_window(void)
{
    if (yo_config_context_window > 0)
        return yo_config_context_window;

    if (yo_token_budget_set && yo_token_budget > 0)
        return (long)yo_token_budget;

    return yo_get_context_window();
}

/* Estimated request size in per-mille (tenths of a percent) of the context
   window, clamped to [0, 999] (the indicator never reads "100.0%").  One
   decimal digit of precision: per-mille/10 and per-mille%10 form "[N.N%]". */
static long
yo_usage_permille(long estimate, long window)
{
    long permille;

    if (window <= 0)
        return 0;

    permille = estimate * 1000 / window;
    if (permille < 0)
        permille = 0;
    if (permille > 999)
        permille = 999;

    return permille;
}

/* **************************************************************** */
/*                                                                  */
/*              Anthropic: Request Building & Response Parsing       */
/*                                                                  */
/* **************************************************************** */

/* Add an Anthropic prompt-caching breakpoint — "cache_control":{"type":"ephemeral"}
   — to a JSON object (a tool definition, a system text block, or a message
   content block).  Anthropic caches only what is explicitly marked, up to 4
   breakpoints per request; anything unmarked is re-sent and re-billed as
   fresh input tokens on every call. */
static void
yo_anthropic_mark_cache_breakpoint(cJSON *obj)
{
    cJSON *cache_control;

    if (!obj || !cJSON_IsObject(obj))
        return;

    cache_control = cJSON_CreateObject();
    cJSON_AddStringToObject(cache_control, "type", "ephemeral");
    cJSON_AddItemToObject(obj, "cache_control", cache_control);
}

/* Mark the last content block of the last message in the messages array with
   an Anthropic cache_control breakpoint (see yo_build_anthropic_request).
   The last message's content may be a plain string (typical final user
   message) or an array of blocks (e.g. a tool_result turn); both are handled.
   Skips silently when the messages array is empty or malformed. */
static void
yo_anthropic_mark_last_message_breakpoint(cJSON *messages)
{
    cJSON *last_msg;
    cJSON *content;
    int msg_count;

    if (!messages || !cJSON_IsArray(messages))
        return;

    msg_count = cJSON_GetArraySize(messages);
    if (msg_count <= 0)
        return;

    last_msg = cJSON_GetArrayItem(messages, msg_count - 1);
    if (!last_msg)
        return;

    content = cJSON_GetObjectItem(last_msg, "content");
    if (content && cJSON_IsString(content))
    {
        /* Plain string content: convert to a one-element text-block array
           carrying the breakpoint. */
        cJSON *blocks = cJSON_CreateArray();
        cJSON *block = cJSON_CreateObject();

        cJSON_AddStringToObject(block, "type", "text");
        cJSON_AddStringToObject(block, "text", content->valuestring);
        yo_anthropic_mark_cache_breakpoint(block);
        cJSON_AddItemToArray(blocks, block);
        /* Replaces (and frees) the old string content item. */
        cJSON_ReplaceItemInObject(last_msg, "content", blocks);
    }
    else if (content && cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0)
    {
        yo_anthropic_mark_cache_breakpoint(
            cJSON_GetArrayItem(content, cJSON_GetArraySize(content) - 1));
    }
}

/* Build Anthropic request body, URL, and headers from provider-native messages.
   include_tools: 1 for normal requests (tools array + tool_choice "any" +
   server-side web tools); 0 for tools-less requests (the context-compaction
   summarizer) — no tools array, no tool_choice, and no web-search beta
   header.  The tools prompt-caching breakpoint is skipped when there are no
   tools; the system and final-message breakpoints still apply.
   system_override: when non-NULL, replaces the yosh shell system prompt (and
   the "You are powered by" wrapper) — used by the compaction summarizer.
   max_output_override: when > 0, overrides yo_get_max_output_tokens().
   Extended thinking (~/.yoconf "thinking") is enabled only for normal
   requests: "thinking":{"type":"enabled","budget_tokens":B} is added and
   "tool_choice" is OMITTED (Anthropic rejects forced tool choice together
   with extended thinking); tools are still sent and the model chooses.
   max_tokens is raised to B + 1024 when it would not exceed B (Anthropic
   requires max_tokens > budget_tokens).
   Returns malloc'd request body string.  Sets *url_out and *headers_out.
   Caller must free the request body and the headers (via curl_slist_free_all). */
static char *
yo_build_anthropic_request_ex(cJSON *messages,
                           const char **url_out, struct curl_slist **headers_out,
                           long *timeout_out,
                           int include_tools,
                           const char *system_override,
                           long max_output_override)
{
    cJSON *request_json;
    cJSON *tools = NULL;
    cJSON *tool_choice;
    char *request_body;
    char auth_header[300];
    struct curl_slist *headers = NULL;
    int web_enabled = yo_server_web_enabled && include_tools;
    long max_output = (max_output_override > 0) ? max_output_override
                                                : yo_get_max_output_tokens();
    int thinking_enabled = include_tools && yo_thinking_enabled();
    long thinking_budget = 0;

    if (thinking_enabled)
    {
        thinking_budget = yo_thinking_budget_tokens(yo_config_thinking);

        /* Anthropic requires max_tokens to be strictly greater than
           budget_tokens.  Bump max_tokens when the configured/registry value
           would not leave room for the thinking budget. */
        if (max_output <= thinking_budget)
            max_output = thinking_budget + 1024;
    }

    ZASSERT(yo_model);

    /* Build tools array */
    if (include_tools)
    {
        tools = yo_build_tools_anthropic();

        /* Prompt caching breakpoint #1: mark the LAST CUSTOM tool so the
           tools-array prefix gets cached.  Custom tools are plain
           {name,description,input_schema} objects without a top-level "type"
           field; server tools (web_search/web_fetch) carry "type" and come
           last, and Anthropic's acceptance of cache_control on server-tool
           definitions is unverified — so walk from the end and mark the last
           object that has no "type".  When no custom tool exists (defensive),
           the tools breakpoint is skipped entirely. */
        {
            int tool_count = cJSON_GetArraySize(tools);
            int ti;

            for (ti = tool_count - 1; ti >= 0; ti--)
            {
                cJSON *tool = cJSON_GetArrayItem(tools, ti);

                if (tool && cJSON_IsObject(tool)
                    && !cJSON_GetObjectItem(tool, "type"))
                {
                    yo_anthropic_mark_cache_breakpoint(tool);
                    break;
                }
            }
        }
    }

    /* Build request JSON */
    request_json = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "model", yo_model);
    cJSON_AddNumberToObject(request_json, "max_tokens", max_output);

    /* Extended thinking, when the user configured a thinking level (normal
       requests only — see above).  Placed right after max_tokens so the
       request shape stays stable. */
    if (thinking_enabled)
    {
        cJSON *thinking = cJSON_CreateObject();
        cJSON_AddStringToObject(thinking, "type", "enabled");
        cJSON_AddNumberToObject(thinking, "budget_tokens", thinking_budget);
        cJSON_AddItemToObject(request_json, "thinking", thinking);
    }

    {
        char *base_prompt;
        char *system_prompt;
        cJSON *system_blocks;
        cJSON *system_block;

        if (system_override)
        {
            system_prompt = strdup(system_override);
        }
        else
        {
            /* Prompt core: powered-by line + config info lines + shell system
               prompt + shell tuned prompt text (yo_build_prompt_core). */
            base_prompt = yo_build_prompt_core();

            if (web_enabled)
            {
                /* Same failure handling as yo_build_config_info_lines: on
                   asprintf failure fall back to a valid string (the core
                   without the web paragraph) so system_prompt is never an
                   uninitialized/garbage pointer. */
                if (asprintf(&system_prompt, "%s\n\n"
                    "When you need up-to-date information from the internet (current events, latest docs,\n"
                    "real-time data, etc.), you also have access to web_search and web_fetch server tools.\n"
                    "These run automatically when you use them - just search or fetch as needed before\n"
                    "choosing your final response tool (command or chat).\n"
                    "IMPORTANT: Your output is displayed in a terminal. Never use HTML tags like <cite>,\n"
                    "<source>, <ref>, etc. in your responses. Just write plain text. Do not include\n"
                    "inline citations or reference markers - the user does not need source attribution.",
                    base_prompt) < 0)
                    system_prompt = base_prompt;  /* asprintf failed: send the core un-augmented */
                else
                    free(base_prompt);
            }
            else
            {
                system_prompt = base_prompt;
            }
        }

        if (!system_prompt)
            system_prompt = strdup("");  /* always a valid C string */

        /* Prompt caching breakpoint #2: send "system" as a content-block
           array whose text block carries the cache_control breakpoint. */
        system_blocks = cJSON_CreateArray();
        system_block = cJSON_CreateObject();
        cJSON_AddStringToObject(system_block, "type", "text");
        cJSON_AddStringToObject(system_block, "text", system_prompt);
        yo_anthropic_mark_cache_breakpoint(system_block);
        cJSON_AddItemToArray(system_blocks, system_block);
        cJSON_AddItemToObject(request_json, "system", system_blocks);
        free(system_prompt);
    }

    /* Add messages array to request (takes ownership) */
    cJSON_AddItemToObject(request_json, "messages", messages);

    /* Prompt caching breakpoint #3: mark the LAST content block of the LAST
       message (the conversation tail).  Together with the tool and system
       breakpoints this forms a rolling cache: each request's prefix overlaps
       the previous request's cached prefix.  Done here, after the messages
       array is attached to the request JSON, so it applies no matter which
       builder produced the messages. */
    yo_anthropic_mark_last_message_breakpoint(
        cJSON_GetObjectItem(request_json, "messages"));

    /* Add tools array (takes ownership) and force tool use with
       tool_choice: {"type": "any"} — only for normal (tools-ful) requests.
       With extended thinking enabled, tool_choice is OMITTED: Anthropic
       rejects the combination ("may not be used with tool_choice"), so the
       model chooses freely (tools are still sent). */
    if (tools)
    {
        cJSON_AddItemToObject(request_json, "tools", tools);

        if (!thinking_enabled)
        {
            tool_choice = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_choice, "type", "any");
            cJSON_AddItemToObject(request_json, "tool_choice", tool_choice);
        }
    }

    request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);

    /* Set up headers */
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", yo_api_key);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    if (web_enabled)
        headers = curl_slist_append(headers, "anthropic-beta: web-fetch-2025-09-10");

    /* Build URL: use base_url if set, otherwise default */
    if (yo_base_url)
    {
        /* Ensure base_url ends with / */
        size_t base_len = strlen(yo_base_url);
        if (base_len > 0 && yo_base_url[base_len - 1] == '/')
        {
            if (asprintf((char **)url_out, "%smessages", yo_base_url) < 0)
            {
                *url_out = NULL;
            }
        }
        else
        {
            if (asprintf((char **)url_out, "%s/messages", yo_base_url) < 0)
            {
                *url_out = NULL;
            }
        }
    }
    else
    {
        *url_out = strdup("https://api.anthropic.com/v1/messages");
    }

    if (!*url_out)
    {
        free(request_body);
        curl_slist_free_all(headers);
        return NULL;
    }

    *headers_out = headers;
    *timeout_out = 0;

    return request_body;
}

/* Parse Anthropic API response → normalized tool_use cJSON.
   If is_retry is set and there are multiple tool_use blocks, takes the first.
   Otherwise for multiple tool_use blocks, returns NULL and sets *needs_retry=1
   with the content array duplicated into *retry_content_out.
   Caller must free the returned cJSON with cJSON_Delete. */
static cJSON *
yo_parse_anthropic_response(const char *response_data, int is_retry,
                            int *needs_retry, cJSON **retry_content_out)
{
    cJSON *response_json;
    cJSON *content_array;
    cJSON *result = NULL;
    int i, content_count;

    *needs_retry = 0;
    *retry_content_out = NULL;

    response_json = cJSON_Parse(response_data);
    if (!response_json)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to parse API response");
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
                fprintf(rl_outstream, "%s%sAPI error: %s%s\n",
                        yo_get_chat_prefix(), yo_get_color_prefix(), msg->valuestring, yo_get_color_reset());
                fflush(rl_outstream);
            }
            else
            {
                yo_print_error_no_newline("API returned an error: %s", response_data);
            }
        }
        else
        {
            yo_print_error_no_newline("Unexpected API response format: %s", response_data);
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
            cJSON *content_item = cJSON_GetArrayItem(content_array, i);
            cJSON *type_item;
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
        else if (tool_use_count == 1 || is_retry)
        {
            result = cJSON_DetachItemFromArray(content_array, first_tool_use_idx);
            cJSON_Delete(response_json);
            return result;
        }
        else
        {
            /* Multiple tool_use blocks - signal caller to retry */
            *needs_retry = 1;
            *retry_content_out = cJSON_Duplicate(content_array, 1);
            cJSON_Delete(response_json);
            return NULL;
        }
    }
}

static const char *
yo_provider_to_string(yo_provider_t provider)
{
    switch (provider)
    {
        case YO_PROVIDER_ANTHROPIC:  return "anthropic";
        case YO_PROVIDER_OPENAI:     return "openai";
        case YO_PROVIDER_KIMI:       return "kimi";
        case YO_PROVIDER_DEEPSEEK:   return "deepseek";
        case YO_PROVIDER_QWEN:       return "qwen";
        case YO_PROVIDER_ZAI:        return "zai";
        case YO_PROVIDER_META:       return "meta";
        case YO_PROVIDER_OPENROUTER: return "openrouter";
    }
    ZASSERT(!"unknown provider");
    return "unknown";
}

static int
yo_provider_uses_chat_completions_api(yo_provider_t provider)
{
    /* OpenRouter supports both API styles; the configured openrouter_api
       directive decides which one is used. */
    if (provider == YO_PROVIDER_OPENROUTER)
        return yo_openrouter_api_style == YO_OPENROUTER_API_CHAT;

    return provider == YO_PROVIDER_KIMI
        || provider == YO_PROVIDER_DEEPSEEK
        || provider == YO_PROVIDER_QWEN
        || provider == YO_PROVIDER_ZAI;
}

/* True when the provider (with its configured API style) expects OpenAI
   Responses API message shapes: flat function_call / function_call_output
   items instead of role-based messages. */
static int
yo_provider_uses_responses_api(yo_provider_t provider)
{
    if (provider == YO_PROVIDER_OPENAI || provider == YO_PROVIDER_META)
        return 1;
    if (provider == YO_PROVIDER_OPENROUTER)
        return yo_openrouter_api_style == YO_OPENROUTER_API_RESPONSES;
    return 0;
}

static const char *
yo_default_chat_completions_url(yo_provider_t provider)
{
    switch (provider)
    {
        case YO_PROVIDER_KIMI:       return "https://api.moonshot.ai/v1/chat/completions";
        case YO_PROVIDER_DEEPSEEK:   return "https://api.deepseek.com/chat/completions";
        case YO_PROVIDER_QWEN:       return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
        case YO_PROVIDER_ZAI:        return "https://api.z.ai/api/paas/v4/chat/completions";
        case YO_PROVIDER_OPENROUTER: return "https://openrouter.ai/api/v1/chat/completions";
        default:                     ZASSERT(!"not a chat completions provider"); return NULL;
    }
}

/* Additional prompt text supplied by the shell via the rl_yo_prompt_callback_t
   callback (bashline.c registers the big "You are a SHELL assistant..." tuned
   texts there).  NULL callback == always empty.  Returns a malloc'd string
   (possibly empty; caller frees). */
static char *
yo_shell_tuned_prompt(void)
{
    char *text;

    if (!yo_prompt_callback)
        return strdup("");

    text = (char *)yo_prompt_callback(yo_provider_to_string(yo_provider), yo_model);
    if (!text)
        return strdup("");

    return text;
}

/* Factual lines describing the shell's current LLM configuration, embedded in
   every normal request's prompt so the model knows the limits it is operating
   under.  Returns a malloc'd string with no trailing newline (caller frees).
   The model-info lookups here are cache-hits in practice (the builders
   already resolved max output tokens), but on a cache miss they may print the
   "Fetching model info..." indicator. */
static char *
yo_build_config_info_lines(void)
{
    const char *thinking_level = yo_thinking_level_string(yo_config_thinking);
    char *lines = NULL;
    char *tail = NULL;

    if (yo_base_url && *yo_base_url)
    {
        if (asprintf(&tail, "API base URL: %s.", yo_base_url) < 0)
            tail = NULL;
    }

    if (asprintf(&lines,
                 "Context window: %ld tokens (context is compacted automatically above 50%% usage).\n"
                 "Max output tokens per response: %ld.\n"
                 "Server-side web search: %s.\n"
                 "Thinking level: %s.\n"
                 "Prompt caching: enabled.%s%s",
                 yo_get_effective_context_window(),
                 yo_get_max_output_tokens(),
                 yo_server_web_enabled ? "enabled" : "disabled",
                 thinking_level ? thinking_level : "provider default",
                 tail ? "\n" : "",
                 tail ? tail : "") < 0)
        lines = NULL;

    free(tail);
    return lines ? lines : strdup("");
}

/* Append `para` to `prompt` as its own paragraph ("\n\n" separator).
   Consumes `prompt` (frees it) and returns the joined string; an empty or
   NULL `para` returns `prompt` unchanged. */
static char *
yo_prompt_append_paragraph(char *prompt, const char *para)
{
    char *joined;

    if (!para || !*para)
        return prompt;
    if (!prompt)
        return strdup(para);
    if (asprintf(&joined, "%s\n\n%s", prompt, para) < 0)
        return prompt;
    free(prompt);
    return joined;
}

/* Shared prompt core for all three request builders (Anthropic Messages,
   Responses API, Chat Completions), used for NORMAL requests only — the
   compaction summarizer passes its own system_override and never sees this:

       You are powered by <model> (provider: <provider>).

       <config info lines>

       <shell system prompt (tools guidance, OS info)>

       <shell tuned prompt text, when the shell's callback supplies one>

   Returns a malloc'd string; callers append their provider-specific
   paragraphs (e.g. the web-search paragraph) afterwards. */
static char *
yo_build_prompt_core(void)
{
    char *powered = NULL;
    char *config_info;
    char *extra;
    char *core;

    ZASSERT(yo_model);

    config_info = yo_build_config_info_lines();
    extra = yo_shell_tuned_prompt();

    if (asprintf(&powered, "You are powered by %s (provider: %s).",
                 yo_model, yo_provider_to_string(yo_provider)) < 0)
        powered = NULL;

    core = yo_prompt_append_paragraph(powered, config_info);
    free(config_info);
    core = yo_prompt_append_paragraph(core, yo_system_prompt);
    core = yo_prompt_append_paragraph(core, extra);
    free(extra);

    return core;
}

/* Heuristic: does this OpenAI model plausibly emit reasoning items?
   Mirrors brainstorm-3's model_supports_reasoning?: true for the o-series
   (o1/o3/o4/...) and for gpt-<n> with n >= 5 (gpt-5, gpt-5.2, gpt-6, ...).
   Case-insensitive. */
static int
yo_openai_model_supports_reasoning(const char *model)
{
    long major = 0;

    if (!model || !*model)
        return 0;

    if (*model == 'o' || *model == 'O')
        return 1;  /* o1, o3, o4-mini, ... */

    if (strncasecmp(model, "gpt-", 4) != 0)
        return 0;
    model += 4;

    while (*model >= '0' && *model <= '9')
        major = major * 10 + (*model++ - '0');

    return major >= 5;
}

/* **************************************************************** */
/*                                                                  */
/*               Responses API: Request Building & Response Parsing        */
/*                                                                  */
/* **************************************************************** */

/* Flags controlling provider-specific extras in the Responses API request.
   All Responses API providers share the base shape (model, max_output_tokens,
   reasoning, instructions, input, tools, store:false); the flags toggle the
   parts not every provider supports. */
#define YO_RESPONSES_FLAG_STRICT_TOOLS         (1 << 0)  /* "strict":true + "additionalProperties":false in tool schemas (OpenAI only) */
#define YO_RESPONSES_FLAG_TOOL_CHOICE_REQUIRED (1 << 1)  /* send "tool_choice":"required" (OpenAI, OpenRouter; Meta rejects it with HTTP 400) */
#define YO_RESPONSES_FLAG_WEB_SEARCH           (1 << 2)  /* include {"type":"web_search"} tool when yo_server_web_enabled (OpenAI, Meta) */
#define YO_RESPONSES_FLAG_WEB_SEARCH_PROMPT    (1 << 3)  /* mention web search availability in the tuned prompt (never for OpenRouter) */
#define YO_RESPONSES_FLAG_INCLUDE_REASONING    (1 << 4)  /* send "include":["reasoning.encrypted_content"] so reasoning can be replayed on later turns */
#define YO_RESPONSES_FLAG_META_CACHE_RETENTION (1 << 5)  /* send "prompt_cache_retention":"in_memory" (Meta Muse in-memory cache tier; OpenAI/OpenRouter 400 on unknown params) */
#define YO_RESPONSES_FLAG_NO_TOOLS             (1 << 6)  /* tools-less request (context-compaction summarizer): no tools array, no tool_choice, no web_search */

/* Build Responses API request body, URL, and headers from provider-native messages.
   The system prompt is prepended as top-level instructions.
   flags selects provider-specific extras (see YO_RESPONSES_FLAG_* above).
   system_override: when non-NULL, replaces the tuned yosh system prompt in
   "instructions" (used by the compaction summarizer).
   max_output_override: when > 0, overrides yo_get_max_output_tokens().
   Returns malloc'd request body string.  Sets *url_out and *headers_out.
   Caller must free the request body and the headers. */
static char *
yo_build_responses_api_request_ex(cJSON *messages,
                        const char **url_out, struct curl_slist **headers_out,
                        long *timeout_out,
                        unsigned flags,
                        const char *system_override,
                        long max_output_override)
{
    cJSON *request_json;
    cJSON *tools;
    char *request_body;
    char auth_header[300];
    struct curl_slist *headers = NULL;
    char *tuned_prompt;
    const char *default_url;
    long max_output = (max_output_override > 0) ? max_output_override
                                                : yo_get_max_output_tokens();

    ZASSERT(yo_model);

    /* Build tools array in Responses API format */
    if (flags & YO_RESPONSES_FLAG_NO_TOOLS)
        tools = NULL;
    else if (flags & YO_RESPONSES_FLAG_STRICT_TOOLS)
        tools = yo_build_tools_responses_api();
    else
        tools = yo_build_tools_responses_api_compat(
            (flags & YO_RESPONSES_FLAG_WEB_SEARCH) ? 1 : 0);

    /* Build tuned system instructions: the shared prompt core (powered-by
       line + config info lines + shell system prompt + shell tuned prompt
       text from the shell's prompt callback) plus the web-search paragraph
       when this provider actually gets a web_search tool and web search is
       enabled. */
    if (system_override)
        tuned_prompt = strdup(system_override);
    else
    {
        char *core = yo_build_prompt_core();
        int mention_web = (flags & YO_RESPONSES_FLAG_WEB_SEARCH_PROMPT) && yo_server_web_enabled;

        if (mention_web)
        {
            /* Same failure handling as yo_build_config_info_lines: on
               asprintf failure fall back to a valid string (here the core
               without the web paragraph) so tuned_prompt is never an
               uninitialized/garbage pointer. */
            if (asprintf(&tuned_prompt, "%s\n\n"
                "You have web search available. When you find the answer to the user's question "
                "via web search (weather, news, sports scores, prices, current events, etc.), "
                "relay the information directly using chat. Do NOT suggest a curl/wget command "
                "when you already have the answer from web search.\n"
                "When citing web sources, use plain text references only. Do not use HTML tags "
                "or markdown link syntax in citations.",
                core) < 0)
                tuned_prompt = core;  /* asprintf failed: send the core un-augmented */
            else
                free(core);
        }
        else
        {
            tuned_prompt = core;
        }
    }

    /* Build Responses API request JSON */
    request_json = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "model", yo_model);
    cJSON_AddNumberToObject(request_json, "max_output_tokens", max_output);

    /* Reasoning effort, when the user configured a thinking level */
    if (yo_thinking_enabled())
    {
        cJSON *reasoning = cJSON_CreateObject();
        cJSON_AddStringToObject(reasoning, "effort",
                                yo_thinking_level_string(yo_config_thinking));
        cJSON_AddItemToObject(request_json, "reasoning", reasoning);
    }

    if (!tuned_prompt)
        tuned_prompt = strdup("");  /* always a valid C string */

    /* System prompt goes in top-level "instructions" field */
    cJSON_AddStringToObject(request_json, "instructions", tuned_prompt);
    free(tuned_prompt);

    /* Add input array (conversation messages — takes ownership) */
    cJSON_AddItemToObject(request_json, "input", messages);

    /* Add tools array (takes ownership) — omitted for tools-less requests */
    if (tools)
        cJSON_AddItemToObject(request_json, "tools", tools);

    /* Force tool use (not for Meta, which only supports the default auto
       tool_choice and returns HTTP 400 for "required"; never for tools-less
       requests) */
    if ((flags & YO_RESPONSES_FLAG_TOOL_CHOICE_REQUIRED) && tools)
        cJSON_AddStringToObject(request_json, "tool_choice", "required");

    /* Ask for encrypted reasoning content so it can be replayed on later turns
       (Meta Muse Responses protocol: "Reasoning items in multi-turn input").
       Works with store:true or store:false and is never combined with
       previous_response_id (which yosh never uses). */
    if (flags & YO_RESPONSES_FLAG_INCLUDE_REASONING)
    {
        cJSON *include = cJSON_CreateArray();
        cJSON_AddItemToArray(include, cJSON_CreateString("reasoning.encrypted_content"));
        cJSON_AddItemToObject(request_json, "include", include);
    }

    /* Privacy: don't store responses */
    cJSON_AddFalseToObject(request_json, "store");

    /* Prompt caching: OpenAI/Meta/OpenRouter cache request prefixes
       automatically; "prompt_cache_key" groups our requests together so the
       vendor's cache matches them (documented for both OpenAI and Meta). */
    cJSON_AddStringToObject(request_json, "prompt_cache_key", "yosh");

    /* Meta Muse only: keep the cached prefix in memory (lowest-latency cache
       tier).  Never sent to OpenAI or OpenRouter, which reject unknown
       parameters with HTTP 400. */
    if (flags & YO_RESPONSES_FLAG_META_CACHE_RETENTION)
        cJSON_AddStringToObject(request_json, "prompt_cache_retention", "in_memory");

    request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);

    /* Set up headers */
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", yo_api_key);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (yo_provider == YO_PROVIDER_OPENROUTER)
        headers = curl_slist_append(headers, "X-Title: yosh");

    /* Build URL: use base_url if set, otherwise default */
    if (yo_provider == YO_PROVIDER_OPENROUTER)
        default_url = "https://openrouter.ai/api/v1/responses";
    else if (yo_provider == YO_PROVIDER_META)
        default_url = "https://api.meta.ai/v1/responses";
    else
        default_url = "https://api.openai.com/v1/responses";

    if (yo_base_url)
    {
        /* Ensure base_url ends with / */
        size_t base_len = strlen(yo_base_url);
        if (base_len > 0 && yo_base_url[base_len - 1] == '/')
        {
            if (asprintf((char **)url_out, "%sresponses", yo_base_url) < 0)
            {
                *url_out = NULL;
            }
        }
        else
        {
            if (asprintf((char **)url_out, "%s/responses", yo_base_url) < 0)
            {
                *url_out = NULL;
            }
        }
    }
    else
    {
        *url_out = strdup(default_url);
    }

    if (!*url_out)
    {
        free(request_body);
        curl_slist_free_all(headers);
        return NULL;
    }

    *headers_out = headers;
    *timeout_out = 0;

    return request_body;
}

/* Build a Responses API request for OpenAI (unchanged legacy behavior:
   strict tool schemas, tool_choice "required", web_search when enabled).
   Encrypted reasoning content is requested when the model plausibly
   supports reasoning, or when `include_reasoning 1` is configured. */
static char *
yo_build_responses_api_request(cJSON *messages,
                        const char **url_out, struct curl_slist **headers_out,
                        long *timeout_out)
{
    int include_reasoning;

    if (yo_include_reasoning == 0)
        include_reasoning = 0;  /* user explicitly disabled */
    else if (yo_include_reasoning == 1)
        include_reasoning = 1;  /* user explicitly enabled */
    else
        include_reasoning = yo_openai_model_supports_reasoning(yo_model);

    return yo_build_responses_api_request_ex(messages, url_out, headers_out, timeout_out,
                                             YO_RESPONSES_FLAG_STRICT_TOOLS
                                             | YO_RESPONSES_FLAG_TOOL_CHOICE_REQUIRED
                                             | YO_RESPONSES_FLAG_WEB_SEARCH
                                             | YO_RESPONSES_FLAG_WEB_SEARCH_PROMPT
                                             | (include_reasoning
                                                ? YO_RESPONSES_FLAG_INCLUDE_REASONING : 0),
                                             NULL, 0);
}

/* Build a Responses API request for Meta Muse.
   Meta speaks the OpenAI Responses API, but: no "tool_choice" (only the
   default auto is supported; sending "required" returns HTTP 400), no
   "strict"/"additionalProperties" in tool schemas, and web_search grounding
   is available (so the prompt may mention web search).
   Meta also accepts "prompt_cache_retention":"in_memory" (its in-memory
   prompt-cache tier); only Meta gets that parameter.
   Encrypted reasoning content is always requested (Muse Spark is a reasoning
   model) unless `include_reasoning 0` is configured. */
static char *
yo_build_meta_request(cJSON *messages,
                      const char **url_out, struct curl_slist **headers_out,
                      long *timeout_out)
{
    return yo_build_responses_api_request_ex(messages, url_out, headers_out, timeout_out,
                                             YO_RESPONSES_FLAG_WEB_SEARCH
                                             | YO_RESPONSES_FLAG_WEB_SEARCH_PROMPT
                                             | YO_RESPONSES_FLAG_META_CACHE_RETENTION
                                             | (yo_include_reasoning == 0
                                                ? 0 : YO_RESPONSES_FLAG_INCLUDE_REASONING),
                                             NULL, 0);
}

/* Build a Responses API request for OpenRouter (Responses API style).
   OpenRouter supports tool_choice "required" but takes plain (non-strict)
   tool schemas, and yosh sends no web_search tool through OpenRouter.
   Encrypted reasoning content is requested only when the ~/.yoconf
   `include_reasoning 1` directive is set: OpenRouter forwards encrypted
   reasoning for only some models, and sending "include" for the rest can
   return HTTP 400. */
static char *
yo_build_openrouter_responses_request(cJSON *messages,
                                      const char **url_out, struct curl_slist **headers_out,
                                      long *timeout_out)
{
    return yo_build_responses_api_request_ex(messages, url_out, headers_out, timeout_out,
                                             YO_RESPONSES_FLAG_TOOL_CHOICE_REQUIRED
                                             | (yo_include_reasoning == 1
                                                ? YO_RESPONSES_FLAG_INCLUDE_REASONING : 0),
                                             NULL, 0);
}

/* Build Chat Completions API request body, URL, and headers.
   Uses the Chat Completions API format with `messages` array.
   Unlike the Responses API, this uses role-based messages.
   include_tools: 1 for normal requests; 0 for tools-less requests (the
   context-compaction summarizer) — no tools array is sent.
   system_override: when non-NULL, replaces the Kimi-tuned system prompt.
   max_output_override: when > 0, overrides yo_get_max_output_tokens().
   Returns malloc'd request body string.  Sets *url_out and *headers_out.
   Caller must free the request body and the headers. */
static char *
yo_build_chat_completions_api_request(cJSON *messages,
                      const char **url_out, struct curl_slist **headers_out,
                      long *timeout_out,
                      int include_tools,
                      const char *system_override,
                      long max_output_override)
{
    cJSON *request_json;
    cJSON *tools;
    char *request_body;
    char auth_header[300];
    struct curl_slist *headers = NULL;
    char *system_prompt;
    long max_output = (max_output_override > 0) ? max_output_override
                                                : yo_get_max_output_tokens();

    ZASSERT(yo_model);

    /* Prompt caching: nothing is sent for Chat Completions API providers.
       Kimi, DeepSeek, Qwen, z.ai, and OpenRouter(chat) get automatic
       server-side prompt caching from their vendors (prefix cache keyed on
       the request itself); none of these vendors accepts cache-control
       request parameters, so the request shape is left untouched. */

    /* Build tools array for Chat Completions API providers (no strict mode, no additionalProperties) */
    tools = include_tools ? yo_build_tools_chat_completions_api() : NULL;

    /* Build system instructions: the shared prompt core (powered-by line +
       config info lines + shell system prompt + shell tuned prompt text from
       the shell's prompt callback).  No web-search paragraph: Chat
       Completions providers receive no server-side web tools. */
    if (system_override)
        system_prompt = strdup(system_override);
    else
        system_prompt = yo_build_prompt_core();

    /* Build request JSON for Chat Completions API */
    request_json = cJSON_CreateObject();
    cJSON_AddStringToObject(request_json, "model", yo_model);
    cJSON_AddNumberToObject(request_json, "max_tokens", max_output);

    /* Thinking/reasoning parameters, when the user configured a thinking level.
       Mirrors brainstorm-3 openai_client.rb: z.ai uses the native `thinking`
       property (with clear_thinking:false and reasoning_effort for GLM-5.2+),
       while Kimi/DeepSeek/Qwen take `reasoning_effort`. */
    if (yo_thinking_enabled())
    {
        if (yo_provider == YO_PROVIDER_ZAI)
        {
            cJSON *thinking = cJSON_CreateObject();
            cJSON_AddStringToObject(thinking, "type", "enabled");
            cJSON_AddFalseToObject(thinking, "clear_thinking");
            cJSON_AddItemToObject(request_json, "thinking", thinking);
            if (yo_zai_supports_reasoning_effort(yo_model))
                cJSON_AddStringToObject(request_json, "reasoning_effort", "max");
        }
        else
        {
            cJSON_AddStringToObject(request_json, "reasoning_effort",
                                    yo_thinking_level_string(yo_config_thinking));
        }
    }

    /* Build messages array with system prompt as first message */
    {
        cJSON *messages_array = cJSON_CreateArray();
        cJSON *system_msg = cJSON_CreateObject();
        cJSON *user_msg;
        
        /* System message */
        cJSON_AddStringToObject(system_msg, "role", "system");
        cJSON_AddStringToObject(system_msg, "content", system_prompt);
        cJSON_AddItemToArray(messages_array, system_msg);
        
        /* Add conversation messages - convert from Responses API format to Chat Completions format */
        if (messages && cJSON_IsArray(messages)) {
            cJSON *item;
            cJSON_ArrayForEach(item, messages) {
                cJSON *type = cJSON_GetObjectItem(item, "type");
                cJSON *role = cJSON_GetObjectItem(item, "role");
                cJSON *content = cJSON_GetObjectItem(item, "content");
                
                if (role && cJSON_IsString(role)) {
                    /* Already in Chat Completions format (user query messages) */
                    cJSON_AddItemToArray(messages_array, cJSON_Duplicate(item, 1));
                } else if (type && cJSON_IsString(type)) {
                    /* In Responses API format - convert to Chat Completions format */
                    const char *type_str = type->valuestring;
                    
                    if (strcmp(type_str, "function_call") == 0) {
                        /* Assistant message with tool_calls */
                        cJSON *call_id = cJSON_GetObjectItem(item, "call_id");
                        cJSON *name = cJSON_GetObjectItem(item, "name");
                        cJSON *arguments = cJSON_GetObjectItem(item, "arguments");
                        cJSON *assistant_msg = cJSON_CreateObject();
                        cJSON *tool_calls = cJSON_CreateArray();
                        cJSON *tool_call = cJSON_CreateObject();
                        cJSON *function = cJSON_CreateObject();
                        
                        cJSON_AddStringToObject(assistant_msg, "role", "assistant");
                        cJSON_AddStringToObject(assistant_msg, "content", "");
                        
                        cJSON_AddStringToObject(tool_call, "id", 
                            (call_id && cJSON_IsString(call_id)) ? call_id->valuestring : "call_1");
                        cJSON_AddStringToObject(tool_call, "type", "function");
                        cJSON_AddStringToObject(function, "name",
                            (name && cJSON_IsString(name)) ? name->valuestring : "chat");
                        if (arguments && cJSON_IsString(arguments)) {
                            cJSON_AddStringToObject(function, "arguments", arguments->valuestring);
                        } else {
                            cJSON_AddStringToObject(function, "arguments", "{}");
                        }
                        cJSON_AddItemToObject(tool_call, "function", function);
                        cJSON_AddItemToArray(tool_calls, tool_call);
                        cJSON_AddItemToObject(assistant_msg, "tool_calls", tool_calls);
                        cJSON_AddItemToArray(messages_array, assistant_msg);
                    } else if (strcmp(type_str, "function_call_output") == 0) {
                        /* Tool result message */
                        cJSON *call_id = cJSON_GetObjectItem(item, "call_id");
                        cJSON *output = cJSON_GetObjectItem(item, "output");
                        cJSON *tool_msg = cJSON_CreateObject();
                        
                        cJSON_AddStringToObject(tool_msg, "role", "tool");
                        cJSON_AddStringToObject(tool_msg, "tool_call_id",
                            (call_id && cJSON_IsString(call_id)) ? call_id->valuestring : "call_1");
                        cJSON_AddStringToObject(tool_msg, "content",
                            (output && cJSON_IsString(output)) ? output->valuestring : "");
                        cJSON_AddItemToArray(messages_array, tool_msg);
                    }
                }
            }
        }

        cJSON_Delete(messages);

        cJSON_AddItemToObject(request_json, "messages", messages_array);
    }
    
    free(system_prompt);

    /* Add tools array (takes ownership) — omitted for tools-less requests */
    if (tools)
        cJSON_AddItemToObject(request_json, "tools", tools);

    /* Note: Chat Completions API providers don't support tool_choice field, so we omit it */

    request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);

    /* Set up headers */
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", yo_api_key);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (yo_provider == YO_PROVIDER_OPENROUTER)
        headers = curl_slist_append(headers, "X-Title: yosh");

    /* Build URL: use base_url if set, otherwise default */
    if (yo_base_url)
    {
        /* Ensure base_url ends with / */
        size_t base_len = strlen(yo_base_url);
        if (base_len > 0 && yo_base_url[base_len - 1] == '/')
        {
            if (asprintf((char **)url_out, "%schat/completions", yo_base_url) < 0)
            {
                *url_out = NULL;
            }
        }
        else
        {
            if (asprintf((char **)url_out, "%s/chat/completions", yo_base_url) < 0)
            {
                *url_out = NULL;
            }
        }
    }
    else
    {
        *url_out = strdup(yo_default_chat_completions_url(yo_provider));
    }

    if (!*url_out)
    {
        free(request_body);
        curl_slist_free_all(headers);
        return NULL;
    }

    *headers_out = headers;
    *timeout_out = 0;

    return request_body;
}

/* Parse Chat Completions API response → normalized tool_use cJSON.
   Extracts tool_calls from choices[0].message.tool_calls[].
   Falls back to message.content for text responses.
   Caller must free the returned cJSON with cJSON_Delete. */
static cJSON *
yo_parse_chat_completions_api_response(const char *response_data)
{
    cJSON *response_json;
    cJSON *choices_array;
    cJSON *first_choice;
    cJSON *message;
    cJSON *tool_calls;
    cJSON *result = NULL;

    response_json = cJSON_Parse(response_data);
    if (!response_json)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to parse API response");
        return NULL;
    }

    /* Check for error response */
    {
        cJSON *error = cJSON_GetObjectItem(response_json, "error");
        if (error && !cJSON_IsNull(error))
        {
            cJSON *msg = cJSON_GetObjectItem(error, "message");
            yo_clear_thinking();
            if (msg && cJSON_IsString(msg))
            {
                fprintf(rl_outstream, "%s%sAPI error: %s%s\n",
                        yo_get_chat_prefix(), yo_get_color_prefix(), msg->valuestring, yo_get_color_reset());
                fflush(rl_outstream);
            }
            else
            {
                yo_print_error_no_newline("API returned an error: %s", response_data);
            }
            cJSON_Delete(response_json);
            return NULL;
        }
    }

    /* Extract choices[] array */
    choices_array = cJSON_GetObjectItem(response_json, "choices");
    if (!choices_array || !cJSON_IsArray(choices_array) || cJSON_GetArraySize(choices_array) == 0)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Unexpected API response format: %s",
                                  cJSON_PrintUnformatted(response_json));
        cJSON_Delete(response_json);
        return NULL;
    }

    /* Get first choice */
    first_choice = cJSON_GetArrayItem(choices_array, 0);
    if (!first_choice)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Empty choices array in response");
        cJSON_Delete(response_json);
        return NULL;
    }

    /* Get message object */
    message = cJSON_GetObjectItem(first_choice, "message");
    if (!message)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("No message in response choice");
        cJSON_Delete(response_json);
        return NULL;
    }

    /* Extract reasoning_content if present (for thinking mode) */
    {
        cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
        if (reasoning && cJSON_IsString(reasoning) && reasoning->valuestring && *reasoning->valuestring)
        {
            /* Store reasoning_content for later use when building messages */
            /* We'll add it to the result object as a special field */
        }
    }

    /* Look for tool_calls array */
    tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0)
    {
        cJSON *tool_call = cJSON_GetArrayItem(tool_calls, 0);
        cJSON *id = cJSON_GetObjectItem(tool_call, "id");
        cJSON *type = cJSON_GetObjectItem(tool_call, "type");
        cJSON *function = cJSON_GetObjectItem(tool_call, "function");
        
        if (function && cJSON_IsObject(function))
        {
            cJSON *name = cJSON_GetObjectItem(function, "name");
            cJSON *arguments = cJSON_GetObjectItem(function, "arguments");
            cJSON *input;
            cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");

            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "type", "tool_use");
            cJSON_AddStringToObject(result, "id",
                (id && cJSON_IsString(id)) ? id->valuestring : "chat_call");
            cJSON_AddStringToObject(result, "name",
                (name && cJSON_IsString(name)) ? name->valuestring : "chat");

            /* Parse arguments JSON string into an object */
            input = (arguments && cJSON_IsString(arguments))
                ? cJSON_Parse(arguments->valuestring) : NULL;
            if (!input)
                input = cJSON_CreateObject();
            cJSON_AddItemToObject(result, "input", input);

            /* Store reasoning_content if present */
            if (reasoning && cJSON_IsString(reasoning) && reasoning->valuestring)
            {
                cJSON_AddStringToObject(result, "reasoning_content", reasoning->valuestring);
            }
        }
    }

    if (!result)
    {
        /* No tool_calls found — look for content */
        cJSON *content = cJSON_GetObjectItem(message, "content");
        cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
        char *text_content = NULL;
        
        if (content && cJSON_IsString(content))
        {
            text_content = content->valuestring;
        }

        /* Wrap text as synthetic chat tool_use */
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

        /* Store reasoning_content if present */
        if (reasoning && cJSON_IsString(reasoning) && reasoning->valuestring)
        {
            cJSON_AddStringToObject(result, "reasoning_content", reasoning->valuestring);
        }
    }

    cJSON_Delete(response_json);
    return result;
}

/* Collect replayable reasoning items from a Responses API output[] array.
   Per the Meta Muse Responses protocol ("Reasoning items in multi-turn
   input"), output[] can contain {"type":"reasoning","id":"rs_...",
   "summary":[...],"encrypted_content":"..."} items.  Items without a
   non-empty encrypted_content cannot be replayed and are dropped.
   Returns a new cJSON array of shallow-cleaned items
   {"type":"reasoning","id":<string or omitted>,"summary":<array, [] when
   absent>,"encrypted_content":<string>} — the items are never rewritten
   beyond this shallow cleaning (replaying is opaque).  Returns NULL when no
   replayable items were found.  Caller must cJSON_Delete the result. */
static cJSON *
yo_collect_reasoning_items(cJSON *output_array)
{
    cJSON *item;
    cJSON *result = NULL;

    cJSON_ArrayForEach(item, output_array)
    {
        cJSON *type_item = cJSON_GetObjectItem(item, "type");
        cJSON *encrypted_item;
        cJSON *clean;

        if (!type_item || !cJSON_IsString(type_item)
            || strcmp(type_item->valuestring, "reasoning") != 0)
            continue;

        encrypted_item = cJSON_GetObjectItem(item, "encrypted_content");
        if (!encrypted_item || !cJSON_IsString(encrypted_item)
            || !encrypted_item->valuestring || !*encrypted_item->valuestring)
            continue;  /* cannot be replayed — always droppable */

        clean = cJSON_CreateObject();
        cJSON_AddStringToObject(clean, "type", "reasoning");

        {
            cJSON *id_item = cJSON_GetObjectItem(item, "id");
            if (id_item && cJSON_IsString(id_item) && id_item->valuestring)
                cJSON_AddStringToObject(clean, "id", id_item->valuestring);
            /* id is optional — omit when the provider did not send one */
        }

        {
            cJSON *summary = cJSON_GetObjectItem(item, "summary");
            if (summary && cJSON_IsArray(summary))
                cJSON_AddItemToObject(clean, "summary", cJSON_Duplicate(summary, 1));
            else
                cJSON_AddItemToObject(clean, "summary", cJSON_CreateArray());
            /* summary is required in replayed items; may be empty */
        }

        cJSON_AddStringToObject(clean, "encrypted_content", encrypted_item->valuestring);

        if (!result)
            result = cJSON_CreateArray();
        cJSON_AddItemToArray(result, clean);
    }

    return result;
}

/* Parse Responses API output → normalized tool_use cJSON.
   Extracts function_call items from the output[] array.
   Falls back to message items for text content.
   Reasoning items with encrypted_content are collected onto the normalized
   tool_use under "reasoning_items" (a JSON array) so they can be replayed on
   later turns.
   Caller must free the returned cJSON with cJSON_Delete. */
static cJSON *
yo_parse_responses_api_response(const char *response_data)
{
    cJSON *response_json;
    cJSON *output_array;
    cJSON *reasoning_items = NULL;
    cJSON *result = NULL;

    response_json = cJSON_Parse(response_data);
    if (!response_json)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to parse API response");
        return NULL;
    }

    /* Check for error response */
    {
        cJSON *error = cJSON_GetObjectItem(response_json, "error");
        if (error && !cJSON_IsNull(error))
        {
            cJSON *msg = cJSON_GetObjectItem(error, "message");
            yo_clear_thinking();
            if (msg && cJSON_IsString(msg))
            {
                fprintf(rl_outstream, "%s%sAPI error: %s%s\n",
                        yo_get_chat_prefix(), yo_get_color_prefix(), msg->valuestring, yo_get_color_reset());
                fflush(rl_outstream);
            }
            else
            {
                yo_print_error_no_newline("API returned an error: %s", response_data);
            }
            cJSON_Delete(response_json);
            return NULL;
        }
    }

    /* Extract output[] array */
    output_array = cJSON_GetObjectItem(response_json, "output");
    if (!output_array || !cJSON_IsArray(output_array) || cJSON_GetArraySize(output_array) == 0)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Unexpected API response format: %s",
                                  cJSON_PrintUnformatted(response_json));
        cJSON_Delete(response_json);
        return NULL;
    }

    /* Collect ALL replayable reasoning items from output[] */
    reasoning_items = yo_collect_reasoning_items(output_array);

    /* Find first function_call item in the output array */
    {
        cJSON *item;
        cJSON_ArrayForEach(item, output_array)
        {
            cJSON *type_item = cJSON_GetObjectItem(item, "type");
            if (type_item && cJSON_IsString(type_item)
                && strcmp(type_item->valuestring, "function_call") == 0)
            {
                cJSON *call_id = cJSON_GetObjectItem(item, "call_id");
                cJSON *name = cJSON_GetObjectItem(item, "name");
                cJSON *arguments = cJSON_GetObjectItem(item, "arguments");
                cJSON *input;

                result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "type", "tool_use");
                cJSON_AddStringToObject(result, "id",
                    (call_id && cJSON_IsString(call_id)) ? call_id->valuestring : "responses_call");
                cJSON_AddStringToObject(result, "name",
                    (name && cJSON_IsString(name)) ? name->valuestring : "chat");

                /* Parse arguments JSON string into an object */
                input = (arguments && cJSON_IsString(arguments))
                    ? cJSON_Parse(arguments->valuestring) : NULL;
                if (!input)
                    input = cJSON_CreateObject();
                cJSON_AddItemToObject(result, "input", input);
                break;
            }
        }
    }

    if (!result)
    {
        /* No function_call found — look for message items with text content */
        cJSON *item;
        char *text_content = NULL;

        cJSON_ArrayForEach(item, output_array)
        {
            cJSON *type_item = cJSON_GetObjectItem(item, "type");
            if (type_item && cJSON_IsString(type_item)
                && strcmp(type_item->valuestring, "message") == 0)
            {
                cJSON *content_array = cJSON_GetObjectItem(item, "content");
                if (content_array && cJSON_IsArray(content_array))
                {
                    cJSON *content_item;
                    cJSON_ArrayForEach(content_item, content_array)
                    {
                        cJSON *ct = cJSON_GetObjectItem(content_item, "type");
                        cJSON *text = cJSON_GetObjectItem(content_item, "text");
                        if (ct && cJSON_IsString(ct)
                            && strcmp(ct->valuestring, "output_text") == 0
                            && text && cJSON_IsString(text))
                        {
                            text_content = text->valuestring;
                            break;
                        }
                    }
                }
                if (text_content)
                    break;
            }
        }

        /* Wrap text as synthetic chat tool_use */
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
    }

    /* Attach replayable reasoning items (if any) to the normalized tool_use */
    if (result && reasoning_items && cJSON_GetArraySize(reasoning_items) > 0)
        cJSON_AddItemToObject(result, "reasoning_items", reasoning_items);
    else if (reasoning_items)
        cJSON_Delete(reasoning_items);

    cJSON_Delete(response_json);
    return result;
}

/* **************************************************************** */
/*                                                                  */
/*                   LLM API Call (Provider Dispatch)               */
/*                                                                  */
/* **************************************************************** */

/* Forward declaration for retry logic */
static cJSON *yo_call_api_with_messages_internal(cJSON *messages, int is_retry);
static cJSON *yo_call_api_with_messages_internal_ex(cJSON *messages, int is_retry, int no_tools);

/* Internal function to call LLM API with pre-built messages array.
   Messages must be in the current provider's native format.
   The messages array is consumed (freed internally).
   Returns a normalized tool_use cJSON object (caller must free with cJSON_Delete). */
static cJSON *
yo_call_api_with_messages(cJSON *messages)
{
    return yo_call_api_with_messages_internal(messages, 0);
}

static cJSON *
yo_call_api_with_messages_internal(cJSON *messages, int is_retry)
{
    return yo_call_api_with_messages_internal_ex(messages, is_retry, 0);
}

/* Compaction summarizer call: like yo_call_api_with_messages_internal but the
   request is built WITHOUT tools/tool_choice, with a minimal summarizer
   system prompt, and with the max output tokens capped at
   YO_SUMMARY_MAX_OUTPUT_TOKENS (a 300-word summary needs only a few hundred
   tokens, and a giant configured max_output_tokens must not let a runaway
   summary eat the window space compaction just freed — the cap is
   min(2048, the configured/resolved max)).  The reply is parsed with the
   normal provider parsers, which wrap plain text as a synthetic chat
   tool_use.  Messages are consumed. */
static cJSON *
yo_call_api_summarize(cJSON *messages)
{
    return yo_call_api_with_messages_internal_ex(messages, 0, 1);
}

static cJSON *
yo_call_api_with_messages_internal_ex(cJSON *messages, int is_retry, int no_tools)
{
    char *url;
    struct curl_slist *headers = NULL;
    long timeout;
    char *request_body;
    char *response_data;
    cJSON *result;
    long max_output_override = 0;

    if (no_tools)
    {
        max_output_override = yo_get_max_output_tokens();
        if (max_output_override <= 0 || max_output_override > YO_SUMMARY_MAX_OUTPUT_TOKENS)
            max_output_override = YO_SUMMARY_MAX_OUTPUT_TOKENS;
    }

    /* Provider-specific request building */
    if (no_tools)
    {
        /* Context-compaction summarizer: no tools, no tool_choice, minimal
           system prompt, capped output.  Keeps the provider's usual URL,
           headers, and cache-request shape (prompt_cache_key, store:false,
           Anthropic system/final-message cache breakpoints). */
        if (yo_provider_uses_responses_api(yo_provider))
        {
            /* OpenAI, Meta, and OpenRouter with `openrouter_api responses`.
               Meta's in-memory cache retention flag stays off: the
               summarizer is a one-off request. */
            request_body = yo_build_responses_api_request_ex(messages,
                (const char **)&url, &headers, &timeout,
                YO_RESPONSES_FLAG_NO_TOOLS,
                YO_SUMMARIZER_SYSTEM_PROMPT, max_output_override);
        }
        else if (yo_provider_uses_chat_completions_api(yo_provider))
        {
            /* Kimi, DeepSeek, Qwen, z.ai, and OpenRouter with `openrouter_api chat` */
            request_body = yo_build_chat_completions_api_request(messages,
                (const char **)&url, &headers, &timeout,
                0, YO_SUMMARIZER_SYSTEM_PROMPT, max_output_override);
        }
        else
        {
            /* Anthropic Messages API style */
            request_body = yo_build_anthropic_request_ex(messages,
                (const char **)&url, &headers, &timeout,
                0, YO_SUMMARIZER_SYSTEM_PROMPT, max_output_override);
        }
    }
    else if (yo_provider == YO_PROVIDER_OPENAI)
    {
        request_body = yo_build_responses_api_request(messages, (const char **)&url, &headers, &timeout);
    }
    else if (yo_provider == YO_PROVIDER_META)
    {
        request_body = yo_build_meta_request(messages, (const char **)&url, &headers, &timeout);
    }
    else if (yo_provider_uses_chat_completions_api(yo_provider))
    {
        /* Kimi, DeepSeek, Qwen, z.ai, and OpenRouter with `openrouter_api chat` */
        request_body = yo_build_chat_completions_api_request(messages, (const char **)&url, &headers, &timeout,
                                                             1, NULL, 0);
    }
    else if (yo_provider == YO_PROVIDER_OPENROUTER)
    {
        /* OpenRouter with `openrouter_api responses` */
        request_body = yo_build_openrouter_responses_request(messages, (const char **)&url, &headers, &timeout);
    }
    else
    {
        request_body = yo_build_anthropic_request_ex(messages, (const char **)&url, &headers, &timeout,
                                                     1, NULL, 0);
    }
    /* Note: messages is now owned by the request JSON and freed with it */

    if (!request_body)
    {
        yo_clear_thinking();
        yo_print_error_no_newline("Failed to build API request");
        return NULL;
    }

    /* Shared HTTP call */
    response_data = yo_http_post(url, headers, request_body, timeout);

    /* Log request/response if YO_LOG_CALLS is set */
    {
        const char *log_path = getenv("YO_LOG_CALLS");
        if (log_path && *log_path)
        {
            FILE *logfp = fopen(log_path, "a");
            if (logfp)
            {
                fprintf(logfp, "=== REQUEST ===\n%s\n", request_body);
                fprintf(logfp, "=== RESPONSE ===\n%s\n\n", response_data ? response_data : "(null)");
                fclose(logfp);
            }
        }
    }

    free(request_body);

    if (!response_data)
    {
        free(url);
        return NULL;  /* Error/cancellation already printed by yo_http_post */
    }

    /* Provider-specific response parsing */
    if (yo_provider_uses_chat_completions_api(yo_provider))
    {
        /* Kimi, DeepSeek, Qwen, z.ai, and OpenRouter with `openrouter_api chat` */
        result = yo_parse_chat_completions_api_response(response_data);
        free(response_data);
        free(url);
        return result;
    }
    else if (yo_provider == YO_PROVIDER_OPENAI
             || yo_provider == YO_PROVIDER_META
             || yo_provider == YO_PROVIDER_OPENROUTER)
    {
        /* OpenAI, Meta, and OpenRouter with `openrouter_api responses` */
        result = yo_parse_responses_api_response(response_data);
        free(response_data);
        free(url);
        return result;
    }
    else
    {
        int needs_retry = 0;
        cJSON *retry_content = NULL;

        result = yo_parse_anthropic_response(response_data, is_retry,
                                              &needs_retry, &retry_content);
        free(response_data);

        if (needs_retry && retry_content)
        {
            /* Multiple tool_use blocks — ask the LLM to pick exactly one */
            cJSON *retry_messages = cJSON_CreateArray();
            cJSON *assistant_msg = cJSON_CreateObject();
            cJSON *user_msg = cJSON_CreateObject();

            cJSON_AddStringToObject(assistant_msg, "role", "assistant");
            cJSON_AddItemToObject(assistant_msg, "content", retry_content);
            cJSON_AddItemToArray(retry_messages, assistant_msg);

            cJSON_AddStringToObject(user_msg, "role", "user");
            cJSON_AddStringToObject(user_msg, "content",
                "You provided multiple tool calls. Please respond with exactly one tool call - "
                "the most appropriate one for the user's request.");
            cJSON_AddItemToArray(retry_messages, user_msg);

            free(url);
            return yo_call_api_with_messages_internal(retry_messages, 1);
        }

        free(url);
        return result;
    }
}

/* Main API call function - builds messages from query and calls API */
static cJSON *
yo_call_api(const char *query)
{
    cJSON *messages = yo_build_messages(query);
    return yo_call_api_with_messages(messages);
}

/* API call with scrollback context - for follow-up after scrollback request */
static cJSON *
yo_call_api_with_scrollback(const char *query,
                            const char *scrollback_request, const char *scrollback_data,
                            const char *scrollback_tool_id,
                            const char *reasoning_content,
                            const char *reasoning_items_json)
{
    cJSON *messages = yo_build_messages_with_scrollback(query, scrollback_request,
                                                        scrollback_data, scrollback_tool_id,
                                                        reasoning_content,
                                                        reasoning_items_json);
    return yo_call_api_with_messages(messages);
}

/* **************************************************************** */
/*                                                                  */
/*                  Explanation Retry Logic                         */
/*                                                                  */
/* **************************************************************** */

/* When the LLM returns a command response without the required explanation
   field, re-prompt it once with the original tool_use as context and a
   request to include the explanation.  Returns the new tool_use cJSON
   (caller must free with cJSON_Delete), or NULL on failure/cancellation.
   The original_tool_use is in normalized (internal) format; the retry
   messages are built in the current provider's native format. */
static cJSON *
yo_retry_for_explanation(const char *query, cJSON *original_tool_use)
{
    cJSON *messages;
    cJSON *id_item;
    cJSON *name_item;
    cJSON *input_item;
    const char *tool_use_id;
    const char *tool_name;

    /* Get the tool_use id and name */
    id_item = cJSON_GetObjectItem(original_tool_use, "id");
    if (!id_item || !cJSON_IsString(id_item))
        return NULL;
    tool_use_id = id_item->valuestring;

    name_item = cJSON_GetObjectItem(original_tool_use, "name");
    tool_name = (name_item && cJSON_IsString(name_item)) ? name_item->valuestring : "command";

    input_item = cJSON_GetObjectItem(original_tool_use, "input");

    /* Build the normal message history including the current query */
    messages = yo_build_messages(query);

    /* Append the assistant's original tool_use using provider-native format */
    {
        cJSON *input_copy = input_item ? cJSON_Duplicate(input_item, 1) : cJSON_CreateObject();
        cJSON *reasoning_item = cJSON_GetObjectItem(original_tool_use, "reasoning_content");
        const char *reasoning = (reasoning_item && cJSON_IsString(reasoning_item)) ? reasoning_item->valuestring : NULL;
        cJSON *reasoning_items_item = cJSON_GetObjectItem(original_tool_use, "reasoning_items");
        char *reasoning_items_json = NULL;

        if (reasoning_items_item && cJSON_IsArray(reasoning_items_item)
            && cJSON_GetArraySize(reasoning_items_item) > 0)
            reasoning_items_json = cJSON_PrintUnformatted(reasoning_items_item);

        yo_msg_add_tool_use(messages, tool_use_id, tool_name, input_copy, reasoning,
                            reasoning_items_json);
        if (reasoning_items_json)
            free(reasoning_items_json);
        cJSON_Delete(input_copy);
    }

    /* Append a tool result requesting the explanation */
    yo_msg_add_tool_result(messages, tool_use_id,
        "Your command response is missing the required \"explanation\" field. "
        "Please respond again with the same command but include a brief explanation. "
        "The explanation is shown to the user before the command and is essential "
        "for them to understand what the command does.");

    return yo_call_api_with_messages(messages);
}

/* **************************************************************** */
/*                                                                  */
/*                  Response Type Helpers                            */
/*                                                                  */
/* **************************************************************** */

static const char *
yo_response_type_to_string(yo_response_type_t type)
{
    switch (type)
    {
    case YO_RESPONSE_COMMAND:    return "command";
    case YO_RESPONSE_CHAT:       return "chat";
    case YO_RESPONSE_SCROLLBACK: return "scrollback";
    case YO_RESPONSE_DOCS:       return "docs";
    case YO_RESPONSE_ERROR:      return "error";
    }
    return "error";
}

static yo_response_type_t
yo_response_type_from_string(const char *str)
{
    if (!str) return YO_RESPONSE_ERROR;
    if (strcmp(str, "command") == 0)    return YO_RESPONSE_COMMAND;
    if (strcmp(str, "chat") == 0)       return YO_RESPONSE_CHAT;
    if (strcmp(str, "scrollback") == 0) return YO_RESPONSE_SCROLLBACK;
    if (strcmp(str, "docs") == 0)       return YO_RESPONSE_DOCS;
    return YO_RESPONSE_ERROR;
}

static void
yo_response_free(yo_response_t *resp)
{
    if (resp->content)      { free(resp->content);      resp->content = NULL; }
    if (resp->explanation)  { free(resp->explanation);  resp->explanation = NULL; }
    if (resp->tool_use_id)  { free(resp->tool_use_id);  resp->tool_use_id = NULL; }
    if (resp->raw_tool_use) { cJSON_Delete(resp->raw_tool_use); resp->raw_tool_use = NULL; }
    if (resp->reasoning_content) { free(resp->reasoning_content); resp->reasoning_content = NULL; }
    if (resp->reasoning_items_json) { free(resp->reasoning_items_json); resp->reasoning_items_json = NULL; }
    resp->type = YO_RESPONSE_ERROR;
    resp->pending = 0;
}

/* **************************************************************** */
/*                                                                  */
/*                    Response Parsing                              */
/*                                                                  */
/* **************************************************************** */

/* Parse a tool_use cJSON object (from API response).
   Extracts the tool name as type, the tool_use id, and fields from input.
   The tool_use object is NOT freed - caller retains ownership.
   Does NOT set resp->raw_tool_use (caller does that). */
static int
yo_parse_response(cJSON *tool_use, yo_response_t *resp)
{
    cJSON *name_item;
    cJSON *id_item;
    cJSON *input;
    cJSON *content_item;
    cJSON *explanation_item;
    cJSON *lines_item;
    cJSON *pending_item;

    resp->type = YO_RESPONSE_ERROR;
    resp->content = NULL;
    resp->explanation = NULL;
    resp->tool_use_id = NULL;
    resp->pending = 0;
    resp->reasoning_content = NULL;
    resp->reasoning_items_json = NULL;
    /* Note: resp->raw_tool_use is NOT touched here - caller manages it */

    if (!tool_use)
        return 0;

    /* Extract tool name as type */
    name_item = cJSON_GetObjectItem(tool_use, "name");
    if (!name_item || !cJSON_IsString(name_item))
        return 0;

    resp->type = yo_response_type_from_string(name_item->valuestring);

    /* Extract tool_use id */
    id_item = cJSON_GetObjectItem(tool_use, "id");
    if (id_item && cJSON_IsString(id_item))
        resp->tool_use_id = strdup(id_item->valuestring);

    /* Extract reasoning_content if present (Kimi thinking mode) */
    {
        cJSON *reasoning_item = cJSON_GetObjectItem(tool_use, "reasoning_content");
        if (reasoning_item && cJSON_IsString(reasoning_item) && reasoning_item->valuestring)
            resp->reasoning_content = strdup(reasoning_item->valuestring);
    }

    /* Extract replayable reasoning items if present (Responses API reasoning
       replay).  The normalized tool_use carries them as a JSON array under
       "reasoning_items"; serialize for storage/threading. */
    {
        cJSON *reasoning_items = cJSON_GetObjectItem(tool_use, "reasoning_items");
        if (reasoning_items && cJSON_IsArray(reasoning_items)
            && cJSON_GetArraySize(reasoning_items) > 0)
            resp->reasoning_items_json = cJSON_PrintUnformatted(reasoning_items);
    }

    /* Extract input object */
    input = cJSON_GetObjectItem(tool_use, "input");
    if (!input)
    {
        /* docs tool may have empty input */
        if (resp->type == YO_RESPONSE_DOCS)
        {
            resp->content = strdup("");
            return 1;
        }
        resp->type = YO_RESPONSE_ERROR;
        if (resp->tool_use_id) { free(resp->tool_use_id); resp->tool_use_id = NULL; }
        return 0;
    }

    if (resp->type == YO_RESPONSE_COMMAND)
    {
        content_item = cJSON_GetObjectItem(input, "command");
        if (content_item && cJSON_IsString(content_item))
            resp->content = strdup(content_item->valuestring);

        explanation_item = cJSON_GetObjectItem(input, "explanation");
        if (explanation_item && cJSON_IsString(explanation_item))
            resp->explanation = strdup(explanation_item->valuestring);

        pending_item = cJSON_GetObjectItem(input, "pending");
        if (pending_item && cJSON_IsTrue(pending_item))
            resp->pending = 1;
    }
    else if (resp->type == YO_RESPONSE_CHAT)
    {
        content_item = cJSON_GetObjectItem(input, "response");
        if (content_item && cJSON_IsString(content_item))
            resp->content = strdup(content_item->valuestring);
    }
    else if (resp->type == YO_RESPONSE_SCROLLBACK)
    {
        lines_item = cJSON_GetObjectItem(input, "lines");
        if (lines_item && cJSON_IsNumber(lines_item))
        {
            char lines_str[32];
            snprintf(lines_str, sizeof(lines_str), "%d", (int)lines_item->valuedouble);
            resp->content = strdup(lines_str);
        }
        else
        {
            resp->content = strdup("50");
        }
    }
    else if (resp->type == YO_RESPONSE_DOCS)
    {
        resp->content = strdup("");
    }

    if (!resp->content)
    {
        resp->type = YO_RESPONSE_ERROR;
        if (resp->tool_use_id) { free(resp->tool_use_id); resp->tool_use_id = NULL; }
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
yo_get_chat_prefix(void)
{
    return yo_chat_prefix ? yo_chat_prefix : "";
}

static const char *
yo_get_color_prefix(void)
{
    return yo_color_prefix ? yo_color_prefix : YO_DEFAULT_COLOR_PREFIX;
}

static const char *
yo_get_color_reset(void)
{
    return yo_color_reset ? yo_color_reset : YO_DEFAULT_COLOR_RESET;
}

static const char *
yo_get_enable_italic(void)
{
    return yo_enable_italic ? yo_enable_italic : YO_DEFAULT_ENABLE_ITALIC;
}

static const char *
yo_get_disable_italic(void)
{
    return yo_disable_italic ? yo_disable_italic : YO_DEFAULT_DISABLE_ITALIC;
}

static const char *
yo_get_enable_bold(void)
{
    return yo_enable_bold ? yo_enable_bold : YO_DEFAULT_ENABLE_BOLD;
}

static const char *
yo_get_disable_bold(void)
{
    return yo_disable_bold ? yo_disable_bold : YO_DEFAULT_DISABLE_BOLD;
}

static const char *
yo_get_enable_strikethrough(void)
{
    return yo_enable_strikethrough ? yo_enable_strikethrough : YO_DEFAULT_ENABLE_STRIKETHROUGH;
}

static const char *
yo_get_disable_strikethrough(void)
{
    return yo_disable_strikethrough ? yo_disable_strikethrough : YO_DEFAULT_DISABLE_STRIKETHROUGH;
}

static const char *
yo_get_code_delimiter(void)
{
    return yo_code_delimiter ? yo_code_delimiter : YO_DEFAULT_CODE_DELIMITER;
}

/* Markdown rendering state for yo_display_chat */
typedef struct {
    FILE *out;
    const char *color_prefix;
    const char *color_reset;
    const char *enable_italic;
    const char *disable_italic;
    const char *enable_bold;
    const char *disable_bold;
    const char *enable_strikethrough;
    const char *disable_strikethrough;
    const char *code_delimiter;
    int in_italic;          /* inside *...* */
    int in_bold;            /* inside **...** */
    int in_strikethrough;   /* inside ~~...~~ */
    int in_code_span;       /* inside `...` */
    int in_code_block;      /* inside ``` or indented code block */
} yo_md_state_t;

/* Re-apply current bold/italic/strikethrough state from scratch.
   Resets all attributes first, then emits color_prefix + any active toggles. */
static void
yo_md_reapply_style(yo_md_state_t *st)
{
    fputs(st->color_reset, st->out);
    fputs(st->color_prefix, st->out);
    if (st->in_italic)
        fputs(st->enable_italic, st->out);
    if (st->in_bold)
        fputs(st->enable_bold, st->out);
    if (st->in_strikethrough)
        fputs(st->enable_strikethrough, st->out);
}

/* Render a single line of markdown (without the trailing newline).
   Handles inline: **bold**, *italic*, ~~strikethrough~~, `code`, and their nesting. */
static void
yo_md_render_line(yo_md_state_t *st, const char *line, int len)
{
    int i = 0;

    while (i < len)
    {
        /* Backtick: inline code */
        if (line[i] == '`' && !st->in_code_span)
        {
            /* Enter inline code — reset to terminal default */
            st->in_code_span = 1;
            fputs(st->color_reset, st->out);
            i++;
            continue;
        }
        if (line[i] == '`' && st->in_code_span)
        {
            /* Exit inline code — restore current style (color_prefix + toggles) */
            st->in_code_span = 0;
            yo_md_reapply_style(st);
            i++;
            continue;
        }

        /* Inside code span: emit literally */
        if (st->in_code_span)
        {
            fputc(line[i], st->out);
            i++;
            continue;
        }

        /* Bold: ** */
        if (i + 1 < len && line[i] == '*' && line[i + 1] == '*')
        {
            if (st->in_bold)
            {
                st->in_bold = 0;
                fputs(st->disable_bold, st->out);
            }
            else
            {
                st->in_bold = 1;
                fputs(st->enable_bold, st->out);
            }
            i += 2;
            continue;
        }

        /* Strikethrough: ~~ */
        if (i + 1 < len && line[i] == '~' && line[i + 1] == '~')
        {
            if (st->in_strikethrough)
            {
                st->in_strikethrough = 0;
                fputs(st->disable_strikethrough, st->out);
            }
            else
            {
                st->in_strikethrough = 1;
                fputs(st->enable_strikethrough, st->out);
            }
            i += 2;
            continue;
        }

        /* Italic: * (single) */
        if (line[i] == '*')
        {
            if (st->in_italic)
            {
                st->in_italic = 0;
                fputs(st->disable_italic, st->out);
            }
            else
            {
                st->in_italic = 1;
                fputs(st->enable_italic, st->out);
            }
            i++;
            continue;
        }

        /* Normal character */
        fputc(line[i], st->out);
        i++;
    }
}

/* Check if a line is a list item: optional whitespace then a marker (-, +, *)
   followed by space, or digit(s) followed by . and space. */
static int
yo_md_is_list_item(const char *line, int len)
{
    int i = 0;

    /* Skip leading whitespace */
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= len)
        return 0;

    /* -, +, * followed by space */
    if ((line[i] == '-' || line[i] == '+' || line[i] == '*')
        && i + 1 < len && line[i + 1] == ' ')
        return 1;

    /* digit(s) followed by . and space */
    if (line[i] >= '0' && line[i] <= '9')
    {
        while (i < len && line[i] >= '0' && line[i] <= '9')
            i++;
        if (i < len && line[i] == '.' && i + 1 < len && line[i + 1] == ' ')
            return 1;
    }

    return 0;
}

/* Check if a line is indented (4+ spaces or starts with tab) */
static int
yo_md_is_indented(const char *line, int len)
{
    if (len >= 4 && line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' ')
        return 1;
    if (len >= 1 && line[0] == '\t')
        return 1;
    return 0;
}

static void
yo_display_chat(const char *response)
{
    yo_md_state_t st;
    const char *p, *line_start;
    int in_fenced_block = 0;
    int in_list = 0;          /* inside a list context */
    int prev_blank = 1;       /* previous line was blank (or start of response) */
    int in_indented_code = 0; /* inside a run of indented code block lines */

    st.out = rl_outstream;
    st.color_prefix = yo_get_color_prefix();
    st.color_reset = yo_get_color_reset();
    st.enable_italic = yo_get_enable_italic();
    st.disable_italic = yo_get_disable_italic();
    st.enable_bold = yo_get_enable_bold();
    st.disable_bold = yo_get_disable_bold();
    st.enable_strikethrough = yo_get_enable_strikethrough();
    st.disable_strikethrough = yo_get_disable_strikethrough();
    st.code_delimiter = yo_get_code_delimiter();
    st.in_italic = 0;
    st.in_bold = 0;
    st.in_strikethrough = 0;
    st.in_code_span = 0;
    st.in_code_block = 0;

    /* Print chat_prefix (user-defined text prefix, empty by default) */
    fputs(yo_get_chat_prefix(), st.out);

    /* Start with default chat color */
    fputs(st.color_prefix, st.out);

    p = response;
    while (*p)
    {
        /* Find end of current line */
        line_start = p;
        while (*p && *p != '\n')
            p++;

        int line_len = (int)(p - line_start);
        int is_blank = (line_len == 0);
        int is_indented = yo_md_is_indented(line_start, line_len);
        int is_list_item = yo_md_is_list_item(line_start, line_len);
        int is_fenced_fence = 0;

        /* Check for fenced code block toggle: ``` possibly with leading whitespace */
        {
            const char *fence_check = line_start;
            int fence_remaining = line_len;
            while (fence_remaining > 0 && (*fence_check == ' ' || *fence_check == '\t'))
            {
                fence_check++;
                fence_remaining--;
            }
            is_fenced_fence = (fence_remaining >= 3
                               && fence_check[0] == '`' && fence_check[1] == '`' && fence_check[2] == '`');
        }
        if (is_fenced_fence)
        {
            if (!in_fenced_block)
            {
                in_fenced_block = 1;
                st.in_code_block = 1;
                /* Reset all inline state — don't let stray * or ~~ leak into code */
                st.in_italic = 0;
                st.in_bold = 0;
                st.in_strikethrough = 0;
                st.in_code_span = 0;
                /* Print opening ``` in code_delimiter style */
                fputs(st.code_delimiter, st.out);
                fwrite(line_start, 1, line_len, st.out);
                /* Switch to reset for code block content */
                fputs(st.color_reset, st.out);
            }
            else
            {
                /* Print closing ``` in code_delimiter style */
                fputs(st.code_delimiter, st.out);
                fwrite(line_start, 1, line_len, st.out);
                in_fenced_block = 0;
                st.in_code_block = 0;
                /* Reset inline state coming out of code block */
                st.in_italic = 0;
                st.in_bold = 0;
                st.in_strikethrough = 0;
                st.in_code_span = 0;
                /* Restore base style */
                yo_md_reapply_style(&st);
            }

            in_indented_code = 0;
            prev_blank = 0;
            /* Fenced blocks at left margin end list context */
            if (!is_indented)
                in_list = 0;

            if (*p == '\n')
            {
                fputc('\n', st.out);
                p++;
            }
            continue;
        }

        /* Inside fenced code block: emit raw */
        if (in_fenced_block)
        {
            fwrite(line_start, 1, line_len, st.out);
            if (*p == '\n')
            {
                fputc('\n', st.out);
                p++;
            }
            continue;
        }

        /* Track list context.
           - A list item (at any indent) enters/stays in list context.
           - A blank line preserves list context (loose lists).
           - A non-blank, non-indented, non-list line ends list context. */
        if (is_list_item)
            in_list = 1;
        else if (!is_blank && !is_indented)
            in_list = 0;

        /* Indented code block: only when NOT in a list, and either preceded
           by a blank line or continuing an existing indented code run.
           Per Markdown spec, indented code cannot interrupt a paragraph or list. */
        if (is_indented && !in_list && (prev_blank || in_indented_code))
        {
            in_indented_code = 1;
            fputs(st.color_reset, st.out);
            fwrite(line_start, 1, line_len, st.out);
            yo_md_reapply_style(&st);
            prev_blank = 0;
            if (*p == '\n')
            {
                fputc('\n', st.out);
                p++;
            }
            continue;
        }

        in_indented_code = 0;

        /* Blank line */
        if (is_blank)
        {
            prev_blank = 1;
            if (*p == '\n')
            {
                fputc('\n', st.out);
                p++;
            }
            continue;
        }

        prev_blank = 0;

        /* Check for headings: # at start of line */
        if (line_len >= 2 && line_start[0] == '#')
        {
            int heading_level = 0;
            int hi = 0;
            while (hi < line_len && line_start[hi] == '#')
            {
                heading_level++;
                hi++;
            }
            /* Must be followed by space (or end of line) to be a heading */
            if (hi < line_len && line_start[hi] == ' ')
            {
                if (heading_level == 1)
                {
                    /* # Heading: bold + enable_italic (bold non-italic — stands out) */
                    fputs(st.enable_bold, st.out);
                    fputs(st.enable_italic, st.out);
                }
                else if (heading_level == 2)
                {
                    /* ## Heading: bold (keeps base italic) */
                    fputs(st.enable_bold, st.out);
                }
                else
                {
                    /* ### and deeper: enable_italic (non-italic — lighter emphasis) */
                    fputs(st.enable_italic, st.out);
                }

                fwrite(line_start, 1, line_len, st.out);

                /* Restore normal style */
                if (heading_level == 1)
                {
                    fputs(st.disable_bold, st.out);
                    fputs(st.disable_italic, st.out);
                }
                else if (heading_level == 2)
                {
                    fputs(st.disable_bold, st.out);
                }
                else
                {
                    fputs(st.disable_italic, st.out);
                }

                if (*p == '\n')
                {
                    fputc('\n', st.out);
                    p++;
                }
                continue;
            }
        }

        /* Normal line: render inline markdown */
        /* Reset inline state at line start (code spans don't cross lines) */
        st.in_code_span = 0;

        yo_md_render_line(&st, line_start, line_len);

        if (*p == '\n')
        {
            fputc('\n', st.out);
            p++;
        }
    }

    /* Reset terminal */
    fputs(st.color_reset, st.out);
    fputc('\n', st.out);
    fflush(st.out);
}

static void
yo_print_error_no_newlinev(const char *msg, va_list args)
{
    fprintf(rl_outstream, "%s%sError: ", yo_get_chat_prefix(), yo_get_color_prefix());
    vfprintf(rl_outstream, msg, args);
    fprintf(rl_outstream, "%s\n", yo_get_color_reset());
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

/* Print the thinking indicator: "[<pct/10>.<pct%10>%] Thinking..." -- pct is
   the estimated request size in per-mille (tenths of a percent) of the
   effective context window, so the indicator shows one decimal digit
   ("[5.2%] Thinking...").
   Same chat_prefix/color styling as chat output; no newline (the line is
   erased by yo_clear_thinking).  Replaces any visible "Fetching model
   info..." message first, then marks the thinking line visible
   (yo_thinking_shown) so yo_print_fetching can never erase it. */
static void
yo_print_thinking(int pct)
{
    yo_clear_fetching();
    yo_thinking_shown = 1;
    fprintf(rl_outstream, "%s%s[%d.%d%%] Thinking...%s",
            yo_get_chat_prefix(), yo_get_color_prefix(),
            pct / 10, pct % 10, yo_get_color_reset());
    fflush(rl_outstream);
}

static void
yo_clear_thinking(void)
{
    int saved_errno = errno;
    /* Move cursor back and clear the line */
    fprintf(rl_outstream, "\r\033[K");
    fflush(rl_outstream);
    /* The line is gone either way; drop the indicator state so it can never
       linger across requests. */
    yo_thinking_shown = 0;
    yo_fetching_shown = 0;
    errno = saved_errno;
}

/* Print the "Fetching model info..." indicator (shown when yo_get_model_info
   is about to do a real network fetch of the model's context window / max
   output tokens).  Same chat_prefix/color styling as the thinking indicator;
   no newline; the leading "\r\033[K" safely replaces whatever is on the
   current line (e.g. when the fetch is triggered mid-request-build by
   yo_get_max_output_tokens).  NO-OP while the "[N.N%] Thinking..." indicator
   is visible (yo_thinking_shown): erasing the thinking line here would leave
   nothing on screen for the whole LLM wait.  In the normal pre-thinking case
   the message is printed and disappears when the thinking indicator replaces
   it (yo_print_thinking calls yo_clear_fetching first). */
static void
yo_print_fetching(void)
{
    if (yo_thinking_shown)
        return;
    if (yo_fetching_shown)
        return;
    yo_fetching_shown = 1;
    fprintf(rl_outstream, "\r\033[K%s%sFetching model info...%s",
            yo_get_chat_prefix(), yo_get_color_prefix(), yo_get_color_reset());
    fflush(rl_outstream);
}

static void
yo_clear_fetching(void)
{
    if (!yo_fetching_shown)
        return;
    yo_fetching_shown = 0;
    fprintf(rl_outstream, "\r\033[K");
    fflush(rl_outstream);
}

static void
yo_report_parse_error(cJSON *tool_use)
{
    yo_clear_thinking();
    yo_print_error_no_newline("Failed to parse tool_use from LLM: %s",
                              tool_use ? cJSON_PrintUnformatted(tool_use) : "(null)");
}

/* **************************************************************** */
/*                                                                  */
/*                   Session Memory Functions                       */
/*                                                                  */
/* **************************************************************** */

static void
yo_history_add(const char *query, yo_response_type_t type, const char *response, const char *tool_use_id, int executed, int pending, const char *reasoning_content, const char *reasoning_items_json)
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
    yo_history[yo_history_count].response_type = type;
    yo_history[yo_history_count].response = strdup(response);
    yo_history[yo_history_count].tool_use_id = tool_use_id ? strdup(tool_use_id) : NULL;
    yo_history[yo_history_count].executed = executed;
    yo_history[yo_history_count].pending = pending;
    yo_history[yo_history_count].reasoning_content = reasoning_content ? strdup(reasoning_content) : NULL;
    yo_history[yo_history_count].reasoning_items_json = reasoning_items_json ? strdup(reasoning_items_json) : NULL;
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
        if (yo_history[0].response)
            free(yo_history[0].response);
        if (yo_history[0].tool_use_id)
            free(yo_history[0].tool_use_id);
        if (yo_history[0].reasoning_content)
            free(yo_history[0].reasoning_content);
        if (yo_history[0].reasoning_items_json)
            free(yo_history[0].reasoning_items_json);

        memmove(&yo_history[0], &yo_history[1], (yo_history_count - 1) * sizeof(yo_exchange_t));
        yo_history_count--;
    }

    /* Note: the old token-budget round-robin prune was removed — history
       size is now managed by context compaction (yo_compact_history), which
       summarizes the old part of the conversation instead of discarding it. */
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

/* Rough estimate (4 chars per token, same as yo_estimate_tokens) of the size
   of a full request for `query`: the session history plus the system prompt,
   the query itself, and a fixed slack of 64 tokens for the JSON structure
   (roles, tool_use wrappers, tool results, etc.). */
static int
yo_estimate_request_tokens(const char *query)
{
    int total = yo_estimate_tokens();

    if (yo_system_prompt)
        total += (int)(strlen(yo_system_prompt) / 4);
    if (query)
        total += (int)(strlen(query) / 4);

    return total + 64;
}

/* Per-entry token estimate, consistent with yo_estimate_tokens (4 chars per
   token over the query and response strings). */
static int
yo_history_entry_tokens(int idx)
{
    int total = 0;

    if (yo_history[idx].query)
        total += (int)strlen(yo_history[idx].query);
    if (yo_history[idx].response)
        total += (int)strlen(yo_history[idx].response);

    return total / 4;
}

/* Add an assistant message with a single tool use to the messages array.
   For Anthropic: content array with tool_use block.
   For Responses API providers (OpenAI, Meta, OpenRouter): replayed reasoning
   items (when reasoning_items_json is set) followed by a flat function_call
   item, no role wrapper.  Per the Meta Muse Responses protocol ("Reasoning
   items in multi-turn input"), every reasoning input item must be followed by
   an assistant message or a function_call item before the next
   user/system/developer message — the function_call added here satisfies
   that.  Reasoning items are opaque: they are replayed verbatim (whole item
   or dropped), never rewritten.
   For Chat Completions API providers (Kimi, DeepSeek, Qwen, z.ai, OpenRouter):
   assistant message with tool_calls array; reasoning_items_json is ignored
   (Kimi's reasoning_content string handling is separate).
   The input object is NOT consumed (it is duplicated). */
static void
yo_msg_add_tool_use(cJSON *messages, const char *tool_use_id,
                    const char *tool_name, cJSON *input,
                    const char *reasoning_content,
                    const char *reasoning_items_json)
{
    if (yo_provider_uses_responses_api(yo_provider))
    {
        /* Responses API: replay the assistant's reasoning items before the
           function_call item */
        if (reasoning_items_json && *reasoning_items_json)
        {
            cJSON *items = cJSON_Parse(reasoning_items_json);
            if (items && cJSON_IsArray(items))
            {
                cJSON *item;
                cJSON_ArrayForEach(item, items)
                    cJSON_AddItemToArray(messages, cJSON_Duplicate(item, 1));
            }
            if (items)
                cJSON_Delete(items);
        }

        /* Responses API: flat function_call item, no role wrapper */
        cJSON *msg = cJSON_CreateObject();
        char *args;

        cJSON_AddStringToObject(msg, "type", "function_call");
        cJSON_AddStringToObject(msg, "call_id", tool_use_id);
        cJSON_AddStringToObject(msg, "name", tool_name);
        args = cJSON_PrintUnformatted(input);
        cJSON_AddStringToObject(msg, "arguments", args);
        free(args);
        cJSON_AddItemToArray(messages, msg);
    }
    else if (yo_provider_uses_chat_completions_api(yo_provider))
    {
        /* Chat Completions API: assistant message with tool_calls */
        cJSON *msg = cJSON_CreateObject();
        cJSON *tool_calls = cJSON_CreateArray();
        cJSON *tool_call = cJSON_CreateObject();
        cJSON *function = cJSON_CreateObject();
        char *args;

        cJSON_AddStringToObject(msg, "role", "assistant");
        cJSON_AddStringToObject(msg, "content", "");

        /* Add reasoning_content if present (required for thinking mode) */
        if (reasoning_content && *reasoning_content)
        {
            cJSON_AddStringToObject(msg, "reasoning_content", reasoning_content);
        }

        cJSON_AddStringToObject(tool_call, "id", tool_use_id);
        cJSON_AddStringToObject(tool_call, "type", "function");
        cJSON_AddStringToObject(function, "name", tool_name);
        args = cJSON_PrintUnformatted(input);
        cJSON_AddStringToObject(function, "arguments", args);
        free(args);

        cJSON_AddItemToObject(tool_call, "function", function);
        cJSON_AddItemToArray(tool_calls, tool_call);
        cJSON_AddItemToObject(msg, "tool_calls", tool_calls);
        cJSON_AddItemToArray(messages, msg);
    }
    else
    {
        /* Anthropic: content array with tool_use block */
        cJSON *msg = cJSON_CreateObject();
        cJSON *content_array = cJSON_CreateArray();
        cJSON *tool_use = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "assistant");
        cJSON_AddStringToObject(tool_use, "type", "tool_use");
        cJSON_AddStringToObject(tool_use, "id", tool_use_id);
        cJSON_AddStringToObject(tool_use, "name", tool_name);
        cJSON_AddItemToObject(tool_use, "input", cJSON_Duplicate(input, 1));
        cJSON_AddItemToArray(content_array, tool_use);
        cJSON_AddItemToObject(msg, "content", content_array);
        cJSON_AddItemToArray(messages, msg);
    }
}

/* Add a tool result message to the messages array.
   For Anthropic: user message with tool_result content block.
   For Responses API providers: function_call_output item.
   For Chat Completions API providers: tool message with tool_call_id. */
static void
yo_msg_add_tool_result(cJSON *messages, const char *tool_use_id,
                       const char *result_content)
{
    cJSON *msg = cJSON_CreateObject();

    if (yo_provider_uses_responses_api(yo_provider))
    {
        /* Responses API: function_call_output item */
        cJSON_AddStringToObject(msg, "type", "function_call_output");
        cJSON_AddStringToObject(msg, "call_id", tool_use_id);
        cJSON_AddStringToObject(msg, "output", result_content);
    }
    else if (yo_provider_uses_chat_completions_api(yo_provider))
    {
        /* Chat Completions API: tool message with tool_call_id */
        cJSON_AddStringToObject(msg, "role", "tool");
        cJSON_AddStringToObject(msg, "tool_call_id", tool_use_id);
        cJSON_AddStringToObject(msg, "content", result_content);
    }
    else
    {
        /* Anthropic: user message with tool_result content block */
        cJSON *content_array = cJSON_CreateArray();
        cJSON *tool_result = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(tool_result, "type", "tool_result");
        cJSON_AddStringToObject(tool_result, "tool_use_id", tool_use_id);
        cJSON_AddStringToObject(tool_result, "content", result_content);
        cJSON_AddItemToArray(content_array, tool_result);
        cJSON_AddItemToObject(msg, "content", content_array);
    }

    cJSON_AddItemToArray(messages, msg);
}

/* Helper: build the tool input JSON for a history entry's tool_use */
static cJSON *
yo_build_history_tool_input(int idx)
{
    cJSON *input = cJSON_CreateObject();

    if (yo_history[idx].response_type == YO_RESPONSE_COMMAND)
    {
        cJSON_AddStringToObject(input, "command", yo_history[idx].response);
        cJSON_AddStringToObject(input, "explanation", "(from history)");
        if (yo_history[idx].pending)
            cJSON_AddTrueToObject(input, "pending");
    }
    else if (yo_history[idx].response_type == YO_RESPONSE_CHAT)
    {
        cJSON_AddStringToObject(input, "response", yo_history[idx].response);
    }

    return input;
}

/* Shared helper: add session history entries [from, to) to a messages array.
   Uses yo_msg_add_tool_use / yo_msg_add_tool_result, which produce
   provider-native format based on yo_provider.  (yo_add_history_to_messages
   adds the whole history.  Context compaction does NOT use this helper: the
   summarizer request is tools-less, so its history is flattened to a plain
   text transcript instead — see yo_build_summary_transcript.) */
static void
yo_add_history_range_to_messages(cJSON *messages, int from, int to)
{
    int i;

    if (to > yo_history_count)
        to = yo_history_count;

    for (i = from; i < to; i++)
    {
        cJSON *msg;
        cJSON *input;

        /* User message with query */
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", yo_history[i].query);
        cJSON_AddItemToArray(messages, msg);

        /* Assistant message with tool_use (provider-native) */
        input = yo_build_history_tool_input(i);
        yo_msg_add_tool_use(messages, yo_history[i].tool_use_id,
                            yo_response_type_to_string(yo_history[i].response_type),
                            input, yo_history[i].reasoning_content,
                            yo_history[i].reasoning_items_json);
        cJSON_Delete(input);

        /* Tool result (provider-native) */
        if (yo_history[i].response_type == YO_RESPONSE_COMMAND)
        {
            yo_msg_add_tool_result(messages, yo_history[i].tool_use_id,
                yo_history[i].executed ? "User executed the command" : "User did not execute the command");
        }
        else
        {
            yo_msg_add_tool_result(messages, yo_history[i].tool_use_id, "Acknowledged");
        }
    }
}

static void
yo_add_history_to_messages(cJSON *messages)
{
    yo_add_history_range_to_messages(messages, 0, yo_history_count);
}

/* Render one history entry's assistant text for the summary transcript:
   "Suggested command: <cmd> (the user <executed|did not execute> it)" for
   command responses, the raw response text otherwise (reasoning
   content/items are deliberately left out — they are not needed for a
   summary).  Returns a malloc'd string, or NULL on allocation failure. */
static char *
yo_summary_assistant_text(int idx)
{
    const char *resp = yo_history[idx].response ? yo_history[idx].response : "";

    if (yo_history[idx].response_type == YO_RESPONSE_COMMAND)
    {
        char *out = NULL;

        if (asprintf(&out, "Suggested command: %s (the user %s it)",
                     resp,
                     yo_history[idx].executed ? "executed" : "did not execute") < 0)
            return NULL;
        return out;
    }

    return strdup(resp);
}

/* Build a plain-text transcript of the session history entries [from, to) for
   the compaction summarizer request.  The summarizer is sent WITHOUT a tools
   array, so the old conversation cannot be replayed as native tool_use /
   tool_result blocks (Anthropic rejects tool blocks that reference tools the
   request does not define, and Responses-style servers can reject unpaired or
   unregistered function calls).  Following the brainstorm-3 reference
   implementation (format_messages_for_summary), each entry is flattened to
   two plain text lines, all joined by blank lines:

     [USER] <query>

     [ASSISTANT] <assistant text>

   Returns a malloc'd string ("" when the range is empty), or NULL on
   allocation failure. */
static char *
yo_build_summary_transcript(int from, int to)
{
    char *transcript = NULL;
    size_t len = 0;
    int i;

    if (to > yo_history_count)
        to = yo_history_count;

    for (i = from; i < to; i++)
    {
        const char *query = yo_history[i].query ? yo_history[i].query : "";
        char *assistant = yo_summary_assistant_text(i);
        char *user_line = NULL;
        char *asst_line = NULL;
        size_t ulen, alen, sep;
        char *grown;

        if (!assistant)
        {
            free(transcript);
            return NULL;
        }

        if (asprintf(&user_line, "[USER] %s", query) < 0)
        {
            free(assistant);
            free(transcript);
            return NULL;
        }
        if (asprintf(&asst_line, "[ASSISTANT] %s", assistant) < 0)
        {
            free(user_line);
            free(assistant);
            free(transcript);
            return NULL;
        }
        free(assistant);

        /* Append both lines, separated from any previous content by a blank
           line (the transcript is one flat list of "[ROLE] text" lines). */
        ulen = strlen(user_line);
        alen = strlen(asst_line);
        sep = transcript ? 2 : 0;
        grown = realloc(transcript, len + sep + 2 + ulen + 2 + alen + 1);
        if (!grown)
        {
            free(user_line);
            free(asst_line);
            free(transcript);
            return NULL;
        }
        transcript = grown;
        if (sep)
        {
            memcpy(transcript + len, "\n\n", 2);
            len += 2;
        }
        memcpy(transcript + len, user_line, ulen);
        len += ulen;
        memcpy(transcript + len, "\n\n", 2);
        len += 2;
        memcpy(transcript + len, asst_line, alen);
        len += alen;
        transcript[len] = 0;

        free(user_line);
        free(asst_line);
    }

    if (!transcript)
        transcript = strdup("");

    return transcript;
}

/* **************************************************************** */
/*                                                                  */
/*                     Context Compaction                           */
/*                                                                  */
/* **************************************************************** */

/* Replace the old part of the session history with a single LLM-generated
   summary exchange (context compaction).

   Trigger: yo_call_llm calls this when the estimated request size exceeds
   half of the effective context window (see yo_get_effective_context_window).

   Protocol:
     - Requires at least 2 history entries totaling at least
       YO_COMPACTION_MIN_TOKENS estimated tokens; otherwise nothing happens.
     - Split point: the first YO_COMPACTION_OLD_PART_PCT% (75%) of the
       estimated history tokens is the "old" part; the remaining ~25% (at
       least one entry) is kept verbatim.
     - The old part is flattened to a plain-text transcript
       (yo_build_summary_transcript: "[USER] <query>" / "[ASSISTANT]
       <content>" lines) and sent as ONE user message (the YO_COMPACTION_PROMPT
       instruction followed by the transcript) to a tools-less summarization
       request (yo_call_api_summarize: no tools array, no tool_choice, minimal
       system prompt, output capped at YO_SUMMARY_MAX_OUTPUT_TOKENS).  The
       transcript must be plain text because the request defines no tools:
       replaying native tool_use/tool_result blocks would reference undefined
       tools and be rejected by the API.
     - The plain-text summary is extracted from the synthetic chat tool_use
       the provider parsers produce for text replies.
     - The history is rebuilt as ONE summary exchange (query
       YO_COMPACTION_QUERY, YO_RESPONSE_CHAT with the summary text,
       tool_use_id YO_COMPACTION_TOOL_USE_ID, executed=1) followed by the
       kept entries.  Later requests replay it as: user "[context
       compacted]..." → assistant chat tool_use → tool_result "Acknowledged".

   Display: prints "Compacting..." (chat_prefix/color styling, no newline)
   when it actually attempts the summarization; the caller redraws the
   "[N.N%] Thinking..." usage indicator afterwards.

   Returns 1 if the history was compacted, 0 if not (too little history, or
   the summarization call failed/was cancelled — errors are printed by the
   HTTP layer; the history is left untouched and the caller proceeds
   best-effort without compaction). */
static int
yo_compact_history(void)
{
    int total, split, i, keep, new_count;
    long acc, target;
    cJSON *messages, *msg, *result, *input, *response_item;
    yo_exchange_t *new_history;
    char *summary;

    /* Need something worth summarizing. */
    if (yo_history_count < 2)
        return 0;

    total = yo_estimate_tokens();
    if (total < YO_COMPACTION_MIN_TOKENS)
        return 0;

    yo_clear_thinking();
    fprintf(rl_outstream, "%s%sCompacting...%s",
            yo_get_chat_prefix(), yo_get_color_prefix(), yo_get_color_reset());
    fflush(rl_outstream);

    /* Split point: walk the entries accumulating per-entry tokens until the
       accumulated amount exceeds 75% of the total; the entries before that
       point are the old (summarized) part.  If the 75% point never arrives
       (the last entry alone holds the excess), everything but the last
       entry is summarized.  Clamp to [1, count - 1] so both parts are
       non-empty. */
    target = (long)total * YO_COMPACTION_OLD_PART_PCT / 100;
    acc = 0;
    split = yo_history_count - 1;
    for (i = 0; i < yo_history_count; i++)
    {
        if (acc > target)
        {
            split = i;
            break;
        }
        acc += yo_history_entry_tokens(i);
    }
    if (split < 1)
        split = 1;
    if (split > yo_history_count - 1)
        split = yo_history_count - 1;

    /* Build the summarization request: ONE user message holding the
       [compaction] instruction followed by the flattened transcript of the
       old part of the history.  No tools exist in this request, so the old
       conversation is flattened to plain text instead of being replayed as
       native tool_use/tool_result blocks (the request builder still marks the
       Anthropic system/final-message cache breakpoints on this message). */
    {
        char *transcript = yo_build_summary_transcript(0, split);
        char *content = NULL;

        if (!transcript)
            return 0;

        if (asprintf(&content, "%s\n\n%s", YO_COMPACTION_PROMPT, transcript) < 0)
        {
            free(transcript);
            return 0;
        }
        free(transcript);

        messages = cJSON_CreateArray();
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", content);
        cJSON_AddItemToArray(messages, msg);
        free(content);
    }

    result = yo_call_api_summarize(messages);  /* consumes messages */
    if (!result)
        return 0;  /* HTTP error/cancel already printed; caller redraws indicator */

    /* Extract the summary text from the normalized tool_use.  Text replies
       arrive as a synthetic chat tool_use with input.response set; anything
       else (e.g. a stray real tool_use) is not usable as a summary. */
    summary = NULL;
    input = cJSON_GetObjectItem(result, "input");
    response_item = input ? cJSON_GetObjectItem(input, "response") : NULL;
    if (response_item && cJSON_IsString(response_item)
        && response_item->valuestring && *response_item->valuestring)
        summary = strdup(response_item->valuestring);
    cJSON_Delete(result);

    if (!summary)
        return 0;

    /* Rebuild the history: one summary exchange followed by the kept
       entries [split, count).  The kept entries' strings are transferred
       (shallow struct copy) into the new array; the old-part entries are
       freed along with the old array. */
    keep = yo_history_count - split;
    new_count = 1 + keep;
    new_history = malloc((size_t)new_count * sizeof(yo_exchange_t));
    if (!new_history)
    {
        free(summary);
        return 0;
    }

    new_history[0].query = strdup(YO_COMPACTION_QUERY);
    new_history[0].response_type = YO_RESPONSE_CHAT;
    new_history[0].response = summary;
    new_history[0].tool_use_id = strdup(YO_COMPACTION_TOOL_USE_ID);
    new_history[0].executed = 1;
    new_history[0].pending = 0;
    new_history[0].reasoning_content = NULL;
    new_history[0].reasoning_items_json = NULL;

    if (!new_history[0].query || !new_history[0].tool_use_id)
    {
        if (new_history[0].query)
            free(new_history[0].query);
        free(summary);
        if (new_history[0].tool_use_id)
            free(new_history[0].tool_use_id);
        free(new_history);
        return 0;
    }

    for (i = 0; i < keep; i++)
        new_history[1 + i] = yo_history[split + i];

    for (i = 0; i < split; i++)
    {
        if (yo_history[i].query)
            free(yo_history[i].query);
        if (yo_history[i].response)
            free(yo_history[i].response);
        if (yo_history[i].tool_use_id)
            free(yo_history[i].tool_use_id);
        if (yo_history[i].reasoning_content)
            free(yo_history[i].reasoning_content);
        if (yo_history[i].reasoning_items_json)
            free(yo_history[i].reasoning_items_json);
    }
    free(yo_history);

    yo_history = new_history;
    yo_history_count = new_count;
    yo_history_capacity = new_count;

    return 1;
}

static cJSON *
yo_build_messages(const char *current_query)
{
    cJSON *messages = cJSON_CreateArray();
    cJSON *msg;

    yo_add_history_to_messages(messages);

    /* Add current query */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", current_query);
    cJSON_AddItemToArray(messages, msg);

    return messages;
}

/* Build messages array for a follow-up call after a scrollback request. */
static cJSON *
yo_build_messages_with_scrollback(const char *current_query, const char *scrollback_request,
                                  const char *scrollback_data, const char *scrollback_tool_id,
                                  const char *reasoning_content,
                                  const char *reasoning_items_json)
{
    cJSON *messages = cJSON_CreateArray();
    cJSON *msg;
    char *scrollback_msg = NULL;
    int lines_requested;

    lines_requested = atoi(scrollback_request);
    if (lines_requested <= 0)
        lines_requested = 50;

    yo_add_history_to_messages(messages);

    /* Add current query */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", current_query);
    cJSON_AddItemToArray(messages, msg);

    /* Add assistant's scrollback tool_use (provider-native; Responses API
       providers also replay this turn's reasoning items) */
    {
        cJSON *input = cJSON_CreateObject();
        cJSON_AddNumberToObject(input, "lines", lines_requested);
        yo_msg_add_tool_use(messages, scrollback_tool_id, "scrollback", input,
                            reasoning_content, reasoning_items_json);
        cJSON_Delete(input);
    }

    /* Add tool_result with scrollback data (provider-native) */
    if (yo_provider_uses_chat_completions_api(yo_provider))
    {
        /* Chat Completions API providers need the temporality reminder - they have shown issues with responding to old prompts */
        char *clean = yo_sanitize_scrollback(scrollback_data);
        if (asprintf(&scrollback_msg,
                 "Here is the recent terminal output you requested (ANSI escapes stripped). "
                 "REMEMBER: This shows COMPLETED commands from the PAST. Any prompts in this "
                 "output have ALREADY been handled. Do NOT respond to prompts you see here.\n```\n%s\n```",
                 clean) < 0)
            scrollback_msg = NULL;  /* asprintf failed: use the fallback below */
        free(clean);
    }
    else if (yo_provider_uses_responses_api(yo_provider))
    {
        /* Responses API providers get sanitized scrollback without the temporality reminder */
        char *clean = yo_sanitize_scrollback(scrollback_data);
        if (asprintf(&scrollback_msg,
                 "Here is the recent terminal output you requested (ANSI escapes stripped):\n```\n%s\n```",
                 clean) < 0)
            scrollback_msg = NULL;  /* asprintf failed: use the fallback below */
        free(clean);
    }
    else
    {
        /* Anthropic gets raw scrollback without the temporality reminder */
        if (asprintf(&scrollback_msg,
                 "Here is the recent terminal output you requested:\n```\n%s\n```",
                 scrollback_data ? scrollback_data : "") < 0)
            scrollback_msg = NULL;  /* asprintf failed: use the fallback below */
    }
    yo_msg_add_tool_result(messages, scrollback_tool_id,
                           scrollback_msg ? scrollback_msg : "(scrollback unavailable)");
    free(scrollback_msg);

    return messages;
}

/* Build messages array for a follow-up call after a docs request. */
static cJSON *
yo_build_messages_with_docs(const char *current_query, const char *docs_request,
                            const char *docs_tool_id,
                            const char *reasoning_content,
                            const char *reasoning_items_json)
{
    cJSON *messages = cJSON_CreateArray();
    cJSON *msg;
    char *docs_msg = NULL;
    char *documentation = NULL;
    const char *provider_name = yo_provider_to_string(yo_provider);

    (void)docs_request;

    ZASSERT(yo_model);

    /* Get documentation via callback if available, otherwise use empty docs.
       The callback returns newly allocated memory that we must free. */
    if (yo_documentation_callback)
        documentation = (char *)yo_documentation_callback(provider_name, yo_model);
    if (!documentation)
        documentation = strdup("No documentation available.");

    yo_add_history_to_messages(messages);

    /* Add current query */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", current_query);
    cJSON_AddItemToArray(messages, msg);

    /* Add assistant's docs tool_use (provider-native; Responses API providers
       also replay this turn's reasoning items) */
    {
        cJSON *input = cJSON_CreateObject();
        yo_msg_add_tool_use(messages, docs_tool_id, "docs", input,
                            reasoning_content, reasoning_items_json);
        cJSON_Delete(input);
    }

    /* Add tool_result with documentation (provider-native) */
    if (asprintf(&docs_msg, "Here is the documentation:\n\n%s\n\n"
             "Now please answer the user's original question based on this documentation.",
             documentation) < 0)
        docs_msg = NULL;  /* asprintf failed: use the fallback below */
    yo_msg_add_tool_result(messages, docs_tool_id,
                           docs_msg ? docs_msg : "(documentation unavailable)");
    free(docs_msg);
    free(documentation);  /* Free the documentation returned by callback */

    return messages;
}

/* API call with docs context - for follow-up after docs request */
static cJSON *
yo_call_api_with_docs(const char *query, const char *docs_request,
                      const char *docs_tool_id,
                      const char *reasoning_content,
                      const char *reasoning_items_json)
{
    cJSON *messages = yo_build_messages_with_docs(query, docs_request, docs_tool_id,
                                                  reasoning_content,
                                                  reasoning_items_json);
    return yo_call_api_with_messages(messages);
}
