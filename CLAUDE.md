# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Yosh** is an LLM-enabled shell - a custom build of GNU Bash 5.2.32 with GNU Readline 8.2.13, compiled using the Fil-C memory-safe compiler toolchain. The entire stack (bash, readline, libcurl, openssl, zlib, libc) is compiled with Fil-C for memory safety.

The key feature is the "yo" command: type `yo <natural language>` and the shell calls Claude to either generate a shell command or answer a question.

## Build System

### Building the Project

- **First-time build or after build system changes**: Run `./build.sh`
  - Configures and builds both readline and bash from scratch
  - Uses Fil-C compilers: `/opt/fil/bin/filcc` and `/opt/fil/bin/fil++`
  - Installs to `./prefix/` directory
  - Requires the /opt/fil distribution of Fil-C, which includes libcurl.

- **Incremental builds**: Run `./build_incremental.sh`
  - Faster rebuilds when making code changes
  - Skips configuration steps

### Build Configuration

The build uses specific flags:
- Readline: Built with `--with-curses --disable-shared`. Building a static library obviates the need to set the LD_LIBRARY_PATH when running yosh.
- Bash: Built with `--without-bash-malloc --with-installed-readline`
- Both link with `-lcurl -lm` for LLM features

## Project Structure

```
yosh/
├── readline-8.2.13/    # GNU Readline with "yo" LLM integration
│   ├── yo.c            # LLM implementation (API, session memory, PTY proxy, scrollback)
│   ├── yo.h            # Public API for yo feature
│   └── cJSON.[ch]      # Embedded JSON parser (MIT licensed)
├── bash-5.2.32/        # Yosh shell (bash fork)
│   ├── shell.c         # Main init (readline before job control - critical!)
│   ├── bashline.c      # Calls rl_yo_enable() with system prompt
│   └── version.c       # "Fil's yosh" branding
├── prefix/             # Installation directory
│   └── bin/yosh        # The compiled shell
├── build.sh            # Full build script
└── build_incremental.sh
```

## The "yo" Feature

### Architecture

The yo feature is an **opt-in readline library** (like history):
- Readline provides: `yo.c` with Claude API calls, JSON parsing, session memory
- Bash provides: the system prompt via `rl_yo_enable(prompt)`

```
User types "yo list files by size" + Enter
    ↓
rl_yo_accept_line() detects "yo " prefix
    ↓
Calls Claude API with session history
    ↓
Claude returns JSON: {"type":"command","command":"ls -lS",...}
    ↓
command mode → replaces input line, user can edit/execute
chat mode → prints response, returns to fresh prompt
```

### Configuration

- **API key**: `~/.yoshkey` file (must be mode 0600, read fresh each call)
- **YO_MODEL**: Environment variable, defaults to `claude-sonnet-4-20250514`
- **YO_HISTORY_LIMIT**: Max conversation exchanges to remember (default 10)
- **YO_TOKEN_BUDGET**: Max tokens for history context (default 4096)

### Session Memory

Yosh maintains conversation context within a shell session:
```bash
$ yo find python files
find . -name "*.py"
$ yo now only today's files
find . -name "*.py" -mtime 0
$ yo count them instead
find . -name "*.py" -mtime 0 | wc -l
```

Each exchange stores: query, response type, response content, whether executed.

### Ctrl-C Cancellation

Users can press Ctrl-C to immediately cancel an in-progress API request. Implementation uses:
- Self-pipe trick: signal handler writes to a pipe for immediate wakeup
- `curl_multi_poll()` watches both curl sockets and the signal pipe
- Cancellation is near-instantaneous, not polling-based

### Terminal Scrollback Capture

The yo agent can access recent terminal output to understand command results, error messages, etc. This enables queries like "yo what was that error?" or "yo parse that output".

**Architecture**: A transparent PTY proxy captures all terminal I/O:

```
Before yo_pty_init():
  bash <---> real terminal

After yo_pty_init():
  bash <---> PTY slave
                  |
              PTY master <---> pump process <---> real terminal
                                    |
                              scrollback buffer
                              (mmap shared memory)
```

**Fork-based design**: When `rl_yo_enable()` calls `yo_pty_init()`:
1. Parent process becomes the I/O pump (never returns)
2. Child process calls `setsid()` + `TIOCSCTTY` to establish PTY as controlling terminal
3. Child continues as the shell with stdin/stdout/stderr on the PTY slave
4. Pump forwards I/O bidirectionally and captures output to scrollback buffer
5. Pump forwards signals (SIGINT, SIGWINCH, etc.) to child
6. Pump exits when child exits, preserving exit status

**Why fork-based?** Thread-based approaches fail because bash's job control uses `/dev/tty` directly. After `setsid()` + `TIOCSCTTY`, `/dev/tty` refers to the PTY slave, so job control works correctly. This requires being a session leader, which only works via fork.

**Transparency**: Programs like vim, emacs, mg, tmux, and ssh work normally because:
- The PTY slave is the actual controlling terminal
- Terminal attributes, window size, and signals pass through correctly
- The pump is invisible to the shell and its children

**LLM access**: Claude can request scrollback via a tool response:
```json
{"type":"scrollback","lines":50}
```
yo.c detects this, fetches from the shared buffer, and makes a follow-up API call with the terminal output included.

**Configuration**:
- `YO_SCROLLBACK_ENABLED=0`: Disable scrollback capture entirely
- `YO_SCROLLBACK_BYTES`: Max buffer size (default 1MB)
- `YO_SCROLLBACK_LINES`: Max lines to return (default 1000)

**Critical initialization order**: In `shell.c`, `initialize_readline()` must be called BEFORE `initialize_job_control()`. This ensures the PTY is set up before bash opens `/dev/tty` for job control. Swapping this order causes job control to use the wrong terminal.

### Key Files

| File | Purpose |
|------|---------|
| `readline-8.2.13/yo.c` | LLM implementation: API calls, session memory, PTY proxy, scrollback |
| `readline-8.2.13/yo.h` | Public API: `rl_yo_enable()`, `rl_yo_accept_line()`, `rl_yo_get_scrollback()` |
| `readline-8.2.13/funmap.c` | Registers `yo-accept-line` function |
| `bash-5.2.32/bashline.c` | Calls `rl_yo_enable()` with yosh's system prompt |
| `bash-5.2.32/shell.c` | Main shell init; readline must init before job control |
| `bash-5.2.32/version.c` | Version string and copyright display |

## Key Modifications

### Fil-C Compatibility
In `bash-5.2.32/unwind_prot.c`:
- Changed `char desired_setting[1]` to `void *desired_setting[1]` in SAVED_VAR struct
- Ensures proper memory alignment for Fil-C's safety mechanisms

### Branding
- Binary is `yosh` (not `bash`)
- Version format: `0.1-5.2.32(N)-release` (yosh version + bash version)
- Copyright: Epic Games, Inc. (2026) + Free Software Foundation

## Development Workflow

1. Make code changes in `bash-5.2.32/` or `readline-8.2.13/`
2. Run `./build_incremental.sh` to rebuild
3. Test at `./prefix/bin/yosh`

### Testing yo Feature

```bash
# Without API key - should show helpful error
./prefix/bin/yosh
yo hello

# With API key
echo "sk-ant-..." > ~/.yoshkey
chmod 600 ~/.yoshkey
./prefix/bin/yosh
yo list files by size     # Command mode - shows: ls -lS
yo what is a symlink      # Chat mode - prints explanation
```

## Important Notes

- Requires /opt/fil Fil-C distribution (compiler at `/opt/fil/bin/`, libraries in `/opt/fil/lib`)
- Static linking for libreadline so that you don't need LD_LIBRARY_PATH when running
- The bash malloc is disabled in favor of system malloc (which is a standard way to build bash)
- API key file must have 0600 permissions for security
