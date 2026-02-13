# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Yosh** is an LLM-enabled shell - a custom build of GNU Bash 5.2.32 with GNU Readline 8.2.13, compiled using the Fil-C memory-safe compiler toolchain. The entire stack (bash, readline, libcurl, openssl, zlib, libc) is compiled with Fil-C for memory safety.

The key feature is the "yo" command: type `yo <natural language>` and the shell calls an LLM (Anthropic Claude or OpenAI) to either generate a shell command or answer a question.

## Build System

- **Full build**: `./build.sh` — configures and builds readline + bash from scratch. Requires Fil-C at `/opt/fil`.
- **Incremental build**: `./build_incremental.sh` — faster rebuilds when making code changes.
- **Output**: The resulting shell binary ends up at `../fil-c-2/pizfix/bin/yosh`

## Project Structure

```
yosh/
├── readline-8.2.13/    # GNU Readline with "yo" LLM integration
│   ├── yo.c            # All LLM code: API, session memory, PTY proxy, scrollback, continuation
│   ├── yo.h            # Public API for yo feature
│   └── cJSON.[ch]      # Embedded JSON parser (MIT licensed)
├── bash-5.2.32/        # Yosh shell (bash fork)
│   ├── shell.c         # Main init (readline before job control - critical!)
│   ├── bashline.c      # Calls rl_yo_enable() with system prompt
│   └── version.c       # "Fil's yosh" branding
├── build.sh            # Full build script
└── build_incremental.sh
```

## The "yo" Feature

### Architecture

The yo feature is an **opt-in readline extension** (like history). Readline provides `yo.c` with all LLM logic; bash provides the system prompt via `rl_yo_enable(prompt)`.

**Multi-provider support**: yo supports both Anthropic (Claude) and OpenAI APIs. The provider is selected via `~/.yoconf`. The architecture keeps provider-specific code separated:
- Message building uses provider-aware helpers (`yo_msg_add_tool_use`, `yo_msg_add_tool_result`) that produce native JSON for each provider from C parameters
- HTTP infrastructure is shared (`yo_http_post`) with curl multi-handle and Ctrl-C cancellation
- Request building is per-provider (`yo_build_anthropic_request`, `yo_build_openai_request`)
- Response parsing is per-provider (`yo_parse_anthropic_response`, `yo_parse_openai_response`), both producing a normalized internal tool_use format

**API details**:
- Anthropic uses the Messages API (`/v1/messages`) with server-side tools (`web_search_20250305`, `web_fetch_20250910`)
- OpenAI uses the Responses API (`/v1/responses`, NOT Chat Completions) with `{"type":"web_search"}` tool for web search
- The Responses API always returns `"error": null` on success — error checking must use `cJSON_IsNull()` to avoid false positives
- OpenAI Responses API uses flat items in `input[]` (`{"type":"function_call",...}`, `{"type":"function_call_output",...}`) rather than role-based messages for tool interactions
- OpenAI Responses API uses `"instructions"` for system prompt (not a system message in the input array), `"input"` instead of `"messages"`, `"max_output_tokens"` instead of `"max_completion_tokens"`, and `"output[]"` instead of `"choices[].message"`

Response types from the LLM:
- **command** — `{"type":"command","command":"...","explanation":"..."}` — prefills the command in the prompt for the user to review/edit/execute.
- **command with pending** — same but with `"pending":true` — triggers multi-step continuation (see below).
- **chat** — `{"type":"chat","response":"..."}` — prints the response, returns to fresh prompt.
- **scrollback** — `{"type":"scrollback","lines":N}` — yo fetches terminal scrollback and makes a follow-up API call.

### Configuration

**Config file (`~/.yoconf`)** — Read fresh on each yo request. Supports `#` comments and three directives:
```
# Provider: "anthropic" (default) or "openai"
provider anthropic

# Model name (provider-specific)
model claude-sonnet-4-20250514

# API key
key sk-ant-api03-...
```

**Legacy fallback**: If `~/.yoconf` doesn't exist but `~/.yoshkey` does, the key is read from `~/.yoshkey` and provider defaults to `anthropic`. `~/.yoshkey` is deprecated.

**Provider defaults**:
- Anthropic: model defaults to `claude-sonnet-4-20250514`
- OpenAI: model defaults to `gpt-4o-mini`

**Environment variables**:
- **YO_MODEL**: Overrides the model from `~/.yoconf` or the provider default
- **YO_HISTORY_LIMIT**: Max conversation exchanges to remember (default 10)
- **YO_TOKEN_BUDGET**: Max tokens for history context (default 4096)
- **YO_SCROLLBACK_ENABLED**: Set to `0` to disable PTY proxy / scrollback capture
- **YO_SCROLLBACK_BYTES**: Max scrollback buffer size (default 1MB)
- **YO_SCROLLBACK_LINES**: Max lines to return to LLM (default 1000)
- **YO_SERVER_WEB**: Set to `0` to disable server-side web search and fetch (both providers, default: enabled)
- **Distro detection**: `rl_yo_enable()` reads `/etc/os-release` and appends it to the system prompt.

### Session Memory

Yosh maintains conversation context within a shell session. Each exchange stores: query, response type, response content, whether executed, and whether it was a pending (multi-step) response.

### Multi-Step Continuation

For tasks requiring multiple commands (e.g., "set up keyboard shortcuts"), the LLM can return `"pending":true` on a command response. This triggers an automatic continuation loop:

1. LLM returns `{"type":"command","command":"...","explanation":"...","pending":true}`
2. User sees the explanation and prefilled command, presses Enter to execute
3. On the next prompt, `yo_continuation_hook()` fires via `rl_startup_hook`
4. The hook grabs 200 lines of scrollback, sends a `[continuation]` message to the LLM with the terminal output
5. The LLM responds with the next command (possibly also pending) or a chat/done response
6. Repeat until a response without `"pending":true`

**Cancellation**: Empty line cancels continuation. New `yo ` query cancels continuation. Ctrl-C during API call cancels.

**Implementation details**:
- `yo_continuation_active` flag tracks whether we're mid-sequence
- `yo_continuation_hook()` is a one-shot `rl_startup_hook` — installs itself via `rl_yo_accept_line` when the user executes a pending command, uninstalls at the top of the hook
- `yo_saved_startup_hook` saves/restores bash's own startup hook to avoid conflicts
- The hook prints the thinking indicator without a leading `\n` (unlike `yo_print_thinking()`) because it fires at a fresh prompt start
- History entries include a `pending` field so reconstructed messages preserve `"pending":true` for LLM context

### Terminal Scrollback Capture

A transparent PTY proxy captures all terminal I/O into a shared-memory circular buffer:

```
bash <---> PTY slave | PTY master <---> pump process <---> real terminal
                                             |
                                       scrollback buffer (mmap)
```

`yo_pty_init()` forks: parent becomes the I/O pump (never returns), child becomes the shell with `setsid()` + `TIOCSCTTY` so `/dev/tty` refers to the PTY slave. This is required for bash's job control to work correctly.

**Critical initialization order**: In `shell.c`, `initialize_readline()` must be called BEFORE `initialize_job_control()`. Swapping this order causes job control to use the wrong terminal.

### Ctrl-C Cancellation

Self-pipe trick: SIGINT handler writes to a pipe, `curl_multi_poll()` watches both curl sockets and the signal pipe for near-instantaneous cancellation during API calls.

### Key Files

| File | Purpose |
|------|---------|
| `readline-8.2.13/yo.c` | All LLM code: multi-provider API calls (Anthropic + OpenAI), session memory, PTY proxy, scrollback, continuation |
| `readline-8.2.13/yo.h` | Public API: `rl_yo_enable()`, `rl_yo_accept_line()`, `rl_yo_get_scrollback()` |
| `bash-5.2.32/bashline.c` | Calls `rl_yo_enable()` with yosh's system prompt |
| `bash-5.2.32/shell.c` | Main shell init; readline must init before job control |

## Development Workflow

1. Make code changes in `bash-5.2.32/` or `readline-8.2.13/`
2. Run `./build_incremental.sh` to rebuild
3. Test at `../fil-c-2/pizfix/bin/yosh`
