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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pwd.h>
#include <curl/curl.h>

#include "readline.h"
#include "rlprivate.h"
#include "xmalloc.h"
#include "cJSON.h"
#include "yo.h"

/* **************************************************************** */
/*                                                                  */
/*                        Configuration                             */
/*                                                                  */
/* **************************************************************** */

#define YO_DEFAULT_MODEL "claude-sonnet-4-20250514"
#define YO_DEFAULT_HISTORY_LIMIT 10
#define YO_DEFAULT_TOKEN_BUDGET 4096
#define YO_API_TIMEOUT 30L
#define YO_MAX_TOKENS 1024

/* Default color: red italics */
#define YO_DEFAULT_CHAT_COLOR "\033[3;36m"
#define YO_COLOR_RESET "\033[0m"

/* **************************************************************** */
/*                                                                  */
/*                     Session Memory Types                         */
/*                                                                  */
/* **************************************************************** */

typedef struct {
    char *query;           /* "yo find python files" */
    char *response_type;   /* "command" or "chat" */
    char *response;        /* the command or chat text */
    int executed;          /* 1 if user ran it, 0 if not */
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

/* Track if last command from yo was executed */
static int yo_last_was_command = 0;
static int yo_last_command_executed = 0;

/* **************************************************************** */
/*                                                                  */
/*                    Forward Declarations                          */
/*                                                                  */
/* **************************************************************** */

static void yo_reload_config(void);
static char *yo_load_api_key(void);
static char *yo_call_claude(const char *api_key, const char *query);
static int yo_parse_response(const char *response, char **type, char **content, char **explanation);
static void yo_display_chat(const char *response);
static void yo_history_add(const char *query, const char *type, const char *response, int executed);
static void yo_history_prune(void);
static int yo_estimate_tokens(void);
static cJSON *yo_build_messages(const char *current_query);
static void yo_print_error(const char *msg);
static void yo_print_thinking(void);
static void yo_clear_thinking(void);
static const char *yo_get_chat_color(void);

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
    if (flags >= 0)
        fcntl(yo_sigint_pipe[1], F_SETFL, flags | O_NONBLOCK);

    /* Also make read end non-blocking for drain operation */
    flags = fcntl(yo_sigint_pipe[0], F_GETFL);
    if (flags >= 0)
        fcntl(yo_sigint_pipe[0], F_SETFL, flags | O_NONBLOCK);

    return 0;
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
    if (!ptr)
    {
        return 0;
    }

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
/*                    Public API Functions                          */
/*                                                                  */
/* **************************************************************** */

void
rl_yo_enable(const char *system_prompt)
{
    if (yo_is_enabled)
        return;

    /* Store system prompt from caller */
    asprintf(
        &yo_system_prompt,
        "%s\n"
        "Analyze the user's input:\n\n"
        "1. If it describes a command or task, respond with ONLY:\n"
        "   {\"type\":\"command\",\"command\":\"<command>\",\"explanation\":\"<brief explanation>\"}\n\n"
        "2. If it's a general question, respond with ONLY:\n"
        "   {\"type\":\"chat\",\"response\":\"<your response>\"}\n\n"
        "Respond with valid JSON only.",
        system_prompt);

    /* Bind Enter key to our yo-aware accept-line */
    rl_bind_key('\n', rl_yo_accept_line);
    rl_bind_key('\r', rl_yo_accept_line);

    yo_is_enabled = 1;
}

void
rl_yo_disable(void)
{
    if (!yo_is_enabled)
        return;

    /* Restore normal Enter behavior */
    rl_bind_key('\n', rl_newline);
    rl_bind_key('\r', rl_newline);

    /* Clean up */
    rl_yo_clear_history();

    if (yo_model)
    {
        free(yo_model);
        yo_model = NULL;
    }

    free(yo_system_prompt);
    yo_system_prompt = NULL;

    /* Close signal pipe */
    if (yo_sigint_pipe[0] >= 0)
    {
        close(yo_sigint_pipe[0]);
        yo_sigint_pipe[0] = -1;
    }
    if (yo_sigint_pipe[1] >= 0)
    {
        close(yo_sigint_pipe[1]);
        yo_sigint_pipe[1] = -1;
    }

    yo_is_enabled = 0;
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
/*                   Main Accept Line Handler                       */
/*                                                                  */
/* **************************************************************** */

int
rl_yo_accept_line(int count, int key)
{
    char *api_key;
    char *response;
    char *type = NULL;
    char *content = NULL;
    char *explanation = NULL;

    /* Track if previous yo command was executed */
    if (yo_last_was_command)
    {
        /* Check if we're executing the command (line wasn't modified to start with "yo ") */
        if (rl_line_buffer && strncmp(rl_line_buffer, "yo ", 3) != 0)
        {
            /* User is executing the command or something else */
            if (yo_history_count > 0)
            {
                yo_history[yo_history_count - 1].executed = 1;
            }
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

    /* Reload config from environment (allows mid-session changes) */
    yo_reload_config();

    /* Load API key fresh each time */
    api_key = yo_load_api_key();
    if (!api_key)
    {
        /* Error already printed by yo_load_api_key */
        rl_crlf();
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        return 0;
    }

    /* Show thinking indicator */
    yo_print_thinking();

    /* Call Claude API - handles its own error/cancellation messages */
    response = yo_call_claude(api_key, rl_line_buffer);
    free(api_key);

    if (!response)
    {
        /* Error already printed by yo_call_claude */
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
        return 0;
    }

    /* Clear thinking indicator on success */
    yo_clear_thinking();

    /* Parse the response */
    if (!yo_parse_response(response, &type, &content, &explanation))
    {
        yo_print_error("Failed to parse response from Claude");
        free(response);
        return 0;
    }

    free(response);

    if (strcmp(type, "command") == 0)
    {
        /* Command mode: replace input with generated command */

        /* Print explanation if present */
        if (explanation && *explanation)
        {
            yo_display_chat(explanation);
        }

        /* Add to session history (not executed yet) */
        yo_history_add(rl_line_buffer, type, content, 0);

        /* Replace line with the command */
        rl_replace_line(content, 0);
        rl_point = rl_end;

        /* Mark that we just generated a command */
        yo_last_was_command = 1;

        /* Redisplay with new content */
        rl_on_new_line();
        rl_redisplay();
    }
    else if (strcmp(type, "chat") == 0)
    {
        /* Chat mode: display response, return to fresh prompt */
        yo_display_chat(content);

        /* Add to session history */
        yo_history_add(rl_line_buffer, type, content, 1);

        /* Clear the line and show fresh prompt */
        rl_replace_line("", 0);
        rl_on_new_line();
        rl_redisplay();
    }
    else
    {
        yo_print_error("Unknown response type from Claude");
    }

    if (type)
        free(type);
    if (content)
        free(content);
    if (explanation)
        free(explanation);

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
        fprintf(rl_outstream, "\n[yo] Create ~/.yoshkey with your API key (mode 0600)\n");
        return NULL;
    }

    /* Check permissions - must be 0600 */
    if ((st.st_mode & 0777) != 0600)
    {
        fprintf(rl_outstream, "\n[yo] ~/.yoshkey must have mode 0600 (current: %04o)\n", st.st_mode & 0777);
        return NULL;
    }

    /* Read the key */
    fp = fopen(path, "r");
    if (!fp)
    {
        yo_print_error("Cannot read ~/.yoshkey");
        return NULL;
    }

    key = malloc(256);
    if (!key)
    {
        fclose(fp);
        return NULL;
    }

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

static char *
yo_call_claude(const char *api_key, const char *query)
{
    CURL *curl;
    CURLM *multi;
    struct curl_slist *headers = NULL;
    yo_response_buffer_t response_buf = {0};
    cJSON *request_json = NULL;
    cJSON *messages = NULL;
    char *request_body = NULL;
    char auth_header[300];
    char *result = NULL;
    cJSON *response_json = NULL;
    cJSON *content_array = NULL;
    cJSON *first_content = NULL;
    cJSON *text_item = NULL;
    struct sigaction sa, old_sa;
    int still_running = 1;
    int cancelled = 0;

    /* Initialize self-pipe for Ctrl-C handling */
    if (yo_init_sigint_pipe() < 0)
    {
        yo_clear_thinking();
        yo_print_error("Failed to initialize signal handling");
        return NULL;
    }

    /* Drain any stale signals and reset cancelled flag */
    yo_drain_sigint_pipe();

    curl = curl_easy_init();
    if (!curl)
    {
        yo_clear_thinking();
        yo_print_error("Failed to initialize HTTP client");
        return NULL;
    }

    multi = curl_multi_init();
    if (!multi)
    {
        curl_easy_cleanup(curl);
        yo_clear_thinking();
        yo_print_error("Failed to initialize HTTP client");
        return NULL;
    }

    /* Build request JSON */
    request_json = cJSON_CreateObject();
    if (!request_json)
    {
        curl_multi_cleanup(multi);
        curl_easy_cleanup(curl);
        yo_clear_thinking();
        yo_print_error("Failed to build request");
        return NULL;
    }

    cJSON_AddStringToObject(request_json, "model", yo_model ? yo_model : YO_DEFAULT_MODEL);
    cJSON_AddNumberToObject(request_json, "max_tokens", YO_MAX_TOKENS);
    cJSON_AddStringToObject(request_json, "system", yo_system_prompt);

    /* Build messages array with history */
    messages = yo_build_messages(query);
    if (!messages)
    {
        cJSON_Delete(request_json);
        curl_multi_cleanup(multi);
        curl_easy_cleanup(curl);
        yo_clear_thinking();
        yo_print_error("Failed to build request");
        return NULL;
    }
    cJSON_AddItemToObject(request_json, "messages", messages);

    request_body = cJSON_PrintUnformatted(request_json);
    cJSON_Delete(request_json);

    if (!request_body)
    {
        curl_multi_cleanup(multi);
        curl_easy_cleanup(curl);
        yo_clear_thinking();
        yo_print_error("Failed to build request");
        return NULL;
    }

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
    while (still_running && !cancelled)
    {
        CURLMcode mc;
        int numfds;
        struct curl_waitfd extra_fd;

        /* Let curl do any immediate work */
        mc = curl_multi_perform(multi, &still_running);
        if (mc != CURLM_OK)
            break;

        if (!still_running)
            break;

        /* Set up our signal pipe as an extra fd to poll */
        extra_fd.fd = yo_sigint_pipe[0];
        extra_fd.events = CURL_WAIT_POLLIN;
        extra_fd.revents = 0;

        /* Wait for activity on curl sockets or our signal pipe */
        mc = curl_multi_poll(multi, &extra_fd, 1, 1000, &numfds);
        if (mc != CURLM_OK)
            break;

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

    /* Clean up curl handles */
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    free(request_body);
    curl_easy_cleanup(curl);

    /* Handle cancellation */
    if (cancelled)
    {
        if (response_buf.data)
            free(response_buf.data);
        yo_clear_thinking();
        fprintf(rl_outstream, "%s[yo] Cancelled%s\n", yo_get_chat_color(), YO_COLOR_RESET);
        fflush(rl_outstream);
        return NULL;
    }

    if (!response_buf.data)
    {
        yo_clear_thinking();
        yo_print_error("No response from API");
        return NULL;
    }

    /* Parse API response to extract the text content */
    response_json = cJSON_Parse(response_buf.data);
    free(response_buf.data);

    if (!response_json)
    {
        yo_clear_thinking();
        yo_print_error("Failed to parse API response");
        return NULL;
    }

    /* Extract content[0].text from response */
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
                fprintf(rl_outstream, "%s[yo] API error: %s%s\n",
                        yo_get_chat_color(), msg->valuestring, YO_COLOR_RESET);
                fflush(rl_outstream);
            }
            else
            {
                yo_print_error("API returned an error");
            }
        }
        else
        {
            yo_print_error("Unexpected API response format");
        }
        cJSON_Delete(response_json);
        return NULL;
    }

    first_content = cJSON_GetArrayItem(content_array, 0);
    if (!first_content)
    {
        cJSON_Delete(response_json);
        yo_clear_thinking();
        yo_print_error("Empty response from API");
        return NULL;
    }

    text_item = cJSON_GetObjectItem(first_content, "text");
    if (!text_item || !cJSON_IsString(text_item))
    {
        cJSON_Delete(response_json);
        yo_clear_thinking();
        yo_print_error("Unexpected API response format");
        return NULL;
    }

    result = strdup(text_item->valuestring);
    cJSON_Delete(response_json);

    return result;
}

/* **************************************************************** */
/*                                                                  */
/*                    Response Parsing                              */
/*                                                                  */
/* **************************************************************** */

static int
yo_parse_response(const char *response, char **type, char **content, char **explanation)
{
    cJSON *json;
    cJSON *type_item;
    cJSON *content_item;
    cJSON *explanation_item;

    *type = NULL;
    *content = NULL;
    *explanation = NULL;

    json = cJSON_Parse(response);
    if (!json)
    {
        return 0;
    }

    type_item = cJSON_GetObjectItem(json, "type");
    if (!type_item || !cJSON_IsString(type_item))
    {
        cJSON_Delete(json);
        return 0;
    }

    *type = strdup(type_item->valuestring);

    if (strcmp(*type, "command") == 0)
    {
        content_item = cJSON_GetObjectItem(json, "command");
        if (content_item && cJSON_IsString(content_item))
        {
            *content = strdup(content_item->valuestring);
        }

        explanation_item = cJSON_GetObjectItem(json, "explanation");
        if (explanation_item && cJSON_IsString(explanation_item))
        {
            *explanation = strdup(explanation_item->valuestring);
        }
    }
    else if (strcmp(*type, "chat") == 0)
    {
        content_item = cJSON_GetObjectItem(json, "response");
        if (content_item && cJSON_IsString(content_item))
        {
            *content = strdup(content_item->valuestring);
        }
    }

    cJSON_Delete(json);

    if (!*content)
    {
        if (*type)
            free(*type);
        *type = NULL;
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
yo_print_error(const char *msg)
{
    fprintf(rl_outstream, "\n%s[yo] Error: %s%s\n", yo_get_chat_color(), msg, YO_COLOR_RESET);
    fflush(rl_outstream);
}

static void
yo_print_thinking(void)
{
    fprintf(rl_outstream, "\n%s[yo] Thinking...%s", yo_get_chat_color(), YO_COLOR_RESET);
    fflush(rl_outstream);
}

static void
yo_clear_thinking(void)
{
    /* Move cursor back and clear the line */
    fprintf(rl_outstream, "\r\033[K");
    fflush(rl_outstream);
}

/* **************************************************************** */
/*                                                                  */
/*                   Session Memory Functions                       */
/*                                                                  */
/* **************************************************************** */

static void
yo_history_add(const char *query, const char *type, const char *response, int executed)
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
    yo_history[yo_history_count].executed = executed;
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

static cJSON *
yo_build_messages(const char *current_query)
{
    cJSON *messages;
    cJSON *msg;
    int i;
    char user_content[4096];

    messages = cJSON_CreateArray();
    if (!messages)
        return NULL;

    /* Add history entries */
    for (i = 0; i < yo_history_count; i++)
    {
        /* User message */
        msg = cJSON_CreateObject();
        if (!msg)
        {
            cJSON_Delete(messages);
            return NULL;
        }
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", yo_history[i].query);
        cJSON_AddItemToArray(messages, msg);

        /* Assistant response - format as JSON */
        msg = cJSON_CreateObject();
        if (!msg)
        {
            cJSON_Delete(messages);
            return NULL;
        }
        cJSON_AddStringToObject(msg, "role", "assistant");

        /* Build the JSON response string that matches our expected format */
        if (strcmp(yo_history[i].response_type, "command") == 0)
        {
            cJSON *resp_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(resp_obj, "type", "command");
            cJSON_AddStringToObject(resp_obj, "command", yo_history[i].response);
            char *resp_str = cJSON_PrintUnformatted(resp_obj);
            cJSON_AddStringToObject(msg, "content", resp_str);
            free(resp_str);
            cJSON_Delete(resp_obj);
        }
        else
        {
            cJSON *resp_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(resp_obj, "type", "chat");
            cJSON_AddStringToObject(resp_obj, "response", yo_history[i].response);
            char *resp_str = cJSON_PrintUnformatted(resp_obj);
            cJSON_AddStringToObject(msg, "content", resp_str);
            free(resp_str);
            cJSON_Delete(resp_obj);
        }
        cJSON_AddItemToArray(messages, msg);
    }

    /* Add current query with execution status of previous if applicable */
    msg = cJSON_CreateObject();
    if (!msg)
    {
        cJSON_Delete(messages);
        return NULL;
    }
    cJSON_AddStringToObject(msg, "role", "user");

    if (yo_history_count > 0 && strcmp(yo_history[yo_history_count-1].response_type, "command") == 0)
    {
        /* Include execution status */
        snprintf(user_content, sizeof(user_content), "[%s]\n%s",
                 yo_history[yo_history_count-1].executed ? "executed" : "not executed",
                 current_query);
        cJSON_AddStringToObject(msg, "content", user_content);
    }
    else
    {
        cJSON_AddStringToObject(msg, "content", current_query);
    }
    cJSON_AddItemToArray(messages, msg);

    return messages;
}
