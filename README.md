# Yosh 0.1.2

Yosh is an LLM-enabled shell. It's a custom build of GNU Bash 5.2.32 with GNU Readline 8.2.13, featuring built-in LLM integration for natural language command generation and assistance. It supports **Anthropic Claude**, **OpenAI**, **Kimi**, **DeepSeek**, **Qwen**, **z.ai**, **Meta Muse**, and **OpenRouter** as providers.

The key feature is the **yo** command: type `yo <natural language>` at the prompt and the shell calls an LLM to either generate a shell command or answer a question directly.

## Features

- **Natural language to shell commands**: Type `yo list all python files modified today` and get an executable command prefilled at your prompt
- **Interactive Q&A**: Ask questions like `yo what does the -exec flag in find do?` and get answers inline
- **Multi-provider**: Supports Anthropic Claude, OpenAI, Kimi, DeepSeek, Qwen, z.ai, Meta Muse, and OpenRouter models, configurable via `~/.yoconf`
- **Web search**: The LLM can search the web to answer questions about current events, weather, news, etc.
- **Session memory**: The shell remembers your conversation within a session for context-aware assistance
- **Terminal awareness**: The LLM can read your recent terminal output to understand what you're working on
- **Multi-step tasks**: Complex tasks can be broken into multiple commands that the LLM guides you through sequentially

## Installing from Binary

If you have a pre-built `yosh` binary:

1. **Copy the binary to your path**:
   ```bash
   sudo cp yosh /usr/local/bin/
   sudo chmod 755 /usr/local/bin/yosh
   ```

2. **Add yosh to your available shells**:
   ```bash
   echo '/usr/local/bin/yosh' | sudo tee -a /etc/shells
   ```

3. **Set yosh as your default shell** (optional):
   ```bash
   chsh -s /usr/local/bin/yosh
   ```

4. **Configure your API key** (pick one method):

   **Option A: Key file (simplest)**
   ```bash
   # For Anthropic:
   echo 'your-anthropic-api-key' > ~/.anthropickey && chmod 600 ~/.anthropickey

   # For OpenAI:
   echo 'your-openai-api-key' > ~/.openaikey && chmod 600 ~/.openaikey

   # For Kimi:
   echo 'your-kimi-api-key' > ~/.kimikey && chmod 600 ~/.kimikey

   # For DeepSeek:
   echo 'your-deepseek-api-key' > ~/.deepseekkey && chmod 600 ~/.deepseekkey

   # For Qwen:
   echo 'your-qwen-api-key' > ~/.qwenkey && chmod 600 ~/.qwenkey

   # For z.ai:
   echo 'your-zai-api-key' > ~/.zaikey && chmod 600 ~/.zaikey

   # For Meta Muse:
   echo 'your-meta-api-key' > ~/.metakey && chmod 600 ~/.metakey

   # For OpenRouter:
   echo 'your-openrouter-api-key' > ~/.openrouterkey && chmod 600 ~/.openrouterkey
   ```

   **Option B: Config file (more control)**

   ```bash
   cat > ~/.yoconf << 'EOF'
   # Provider: "anthropic" (default), "openai", "kimi", "deepseek", "qwen",
   # "zai" (or "z.ai"), "meta" (or "muse"), or "openrouter"
   provider anthropic

   # Model (optional, uses provider default if omitted)
   # model claude-sonnet-4-20250514

   # API key (optional here if using a key file)
   key your-api-key-here
   EOF
   chmod 600 ~/.yoconf
   ```

## Building from Source

### Prerequisites

Yosh is built using the [Fil-C](https://fil-c.org/) memory-safe compiler toolchain. You'll need Fil-C installed at `/opt/fil`.

### Getting the Source

```bash
git clone git@github.com:pizlonator/yosh.git
cd yosh
```

### Building

**Full build** (configures and builds readline + bash from scratch):
```bash
./build.sh
```

**Incremental build** (faster rebuilds when making code changes):
```bash
./build_incremental.sh
```

The built binary will be at `./prefix/bin/yosh`.

## Configuration

### Config File (`~/.yoconf`)

Optionally create `~/.yoconf` to configure your LLM provider, model, and/or API key. All directives are optional:

```bash
# Provider: "anthropic" (default), "openai", "kimi", "deepseek", "qwen",
# "zai" (or "z.ai"), "meta" (or "muse"), or "openrouter"
provider anthropic

# Model name (provider-specific, optional)
# Anthropic default: claude-sonnet-4-20250514
# OpenAI default: gpt-4o-mini
# Meta default: muse-spark-1.3
# OpenRouter default: meta/muse-spark-1.3
model claude-sonnet-4-20250514

# API key (optional if using a key file instead)
key sk-ant-api03-...

# Chat display color/reset (optional, supports C-style escapes)
# color_prefix \033[3;36m
# color_reset \033[0m
```

Directives that accept escape sequences (such as `chat_prefix`, `color_prefix`, `color_reset`, and the markdown rendering directives below) support C-style escape sequences (`\033`, `\n`, `\t`, `\\`) and optional quoting with `"` or `'` to preserve whitespace.

### API Key Files

If `~/.yoconf` doesn't contain a `key` directive (or doesn't exist), yosh looks for the API key in a standalone key file (mode 0600, single line with the key):

- If `provider` is set in `~/.yoconf`: checks `~/.anthropickey`, `~/.openaikey`, `~/.kimikey`, `~/.deepseekkey`, `~/.qwenkey`, `~/.zaikey`, `~/.metakey`, or `~/.openrouterkey` (matching the provider).
- If no provider is set: checks `~/.anthropickey`, then `~/.yoshkey` (legacy), then `~/.openaikey`, then `~/.kimikey`, then `~/.deepseekkey`, then `~/.qwenkey`, then `~/.zaikey`, then `~/.metakey`, then `~/.openrouterkey`. The provider is set automatically based on which file is found.
- If no provider is determined from any source, it defaults to Anthropic.

### Additional Directives

All settings are configured in `~/.yoconf`. The file is re-read on each `yo` command, so most changes take effect immediately. Scrollback settings are read once at startup.

| Directive | Default | Description |
|-----------|---------|-------------|
| `history_limit` | `10` | Max conversation exchanges to remember |
| `token_budget` | (unset) | Alias of `context_window`: when set, it overrides the context budget used for the compaction threshold (see below) |
| `scrollback_enabled` | `1` | Set to `0` to disable terminal scrollback capture (startup only) |
| `scrollback_bytes` | `1048576` | Max scrollback buffer size in bytes (startup only) |
| `scrollback_lines` | `1000` | Max lines to return to the LLM (startup only) |
| `server_web` | `1` | Set to `0` to disable server-side web search |
| `context_window` | model-dependent | Override the model's context window (in tokens) used for usage display/compaction; `0` unsets |
| `max_output_tokens` | model-dependent | Override the max output tokens requested from the model; `0` unsets |
| `openrouter_api` | `chat` | API style for the `openrouter` provider: `chat` (OpenAI Chat Completions) or `responses` (OpenAI Responses). Only valid when `provider openrouter` is set |
| `include_reasoning` | provider default | Ask Responses API providers for encrypted reasoning content and replay it on later turns. Defaults: enabled for `openai` reasoning models (o-series, gpt-5+) and always for `meta`; disabled for `openrouter`. Ignored by non-Responses providers |
| `thinking` | off | Thinking/reasoning level: `off`, `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, or `max` |
| `chat_prefix` | `""` (empty) | Text string printed before chat output (supports C escapes) |
| `color_prefix` | `\033[3;36m` | ANSI escape applied at the start of chat output (cyan italic) |
| `chat_reset` / `color_reset` | `\033[0m` | ANSI escape applied after chat output (reset) |

#### Markdown Rendering

These directives control the ANSI escape sequences used for rendering markdown formatting in chat output. Since the default base style is italic, markdown `*italic*` toggles italic OFF to create visual contrast, then toggles it back ON when the italic span ends. All values support C-style escapes.

| Directive | Default | Description |
|-----------|---------|-------------|
| `enable_italic` | `\033[23m` | Escape for markdown `*italic*` start (disables terminal italic since base is already italic) |
| `disable_italic` | `\033[3m` | Escape for markdown `*italic*` end (re-enables terminal italic to return to base style) |
| `enable_bold` | `\033[1m` | Escape for markdown `**bold**` start |
| `disable_bold` | `\033[22m` | Escape for markdown `**bold**` end |
| `enable_strikethrough` | `\033[9m` | Escape for markdown `~~strikethrough~~` start |
| `disable_strikethrough` | `\033[29m` | Escape for markdown `~~strikethrough~~` end |
| `code_delimiter` | `\033[0;3;38;5;23m` | Escape for fenced code block delimiter lines (reset + italic + dark cyan) |

### Context Compaction

While the LLM is working, yosh prints `Thinking...`.

HTTP requests are retried automatically with exponential backoff (the same
policy as t800's HTTP client): after a failed attempt yosh waits 1s, then 2s,
4s, 8s, ... capped at 60s, and gives up after 10 attempts. During a retry the
indicator becomes `[attempt N/10] Waiting...` while yosh waits and
`[attempt N/10] Thinking...` while the next attempt runs. Retried failures are
connection/resolve/timeout/other transport errors and HTTP 408, 429, and 5xx
responses; other HTTP errors (400, 401, 403, 404, 422, ...) fail immediately
because they cannot succeed on retry. Pressing Ctrl-C at any point — during a
request or during a backoff wait — cancels instantly with `Cancelled` and no
further attempts. The last HTTP response body received from any endpoint
(success or failure) is kept and can be shown with `yo show last response`.

When the estimate crosses 50% of the context window, yosh compacts the session
history before sending the request:

1. The first ~75% of the history (by estimated tokens) is summarized by the
   LLM using a tools-less summarization request with a minimal system prompt
   (its output is capped at 2048 tokens).
2. The summary replaces those exchanges as a single `[context compacted]`
   exchange; the most recent ~25% of the history is kept verbatim.
3. The terminal shows the sequence `Thinking...` → `Compacting...` →
   `Thinking...`.

Compaction is best-effort on failure: if the summarization request fails (for
example an HTTP error), the original request proceeds with the uncompacted
history. Pressing Ctrl-C during compaction aborts the whole operation with
"Cancelled" instead. Compaction only runs again when the estimate crosses 50%
again. The context window comes from
the model registry (or the provider's model-info API); the `context_window`
directive — or the legacy `token_budget` directive — overrides it.

### Model Info Fetching

The context window and max output tokens are resolved from the provider's
model-info API (when it reports them) or a built-in model registry, and cached
per (provider, model, base_url). When yosh performs a network (re)fetch — the
first use, or after you change `provider`/`model`/`base_url` in `~/.yoconf` —
the terminal shows `Fetching model info...` until the request's
`Thinking...` indicator replaces it. Registry-only providers (kimi,
deepseek, qwen, z.ai) never fetch and never show it.

### Prompt Caching

Yosh enables prompt caching on every provider that supports it, using whatever
mechanism each one requires. Cached prefixes make repeat requests cheaper and
faster; the more of the conversation prefix that hits the cache, the bigger the
win, so yosh's stable multi-turn history is exactly the shape caches like.

| Provider | Mechanism |
|----------|-----------|
| `anthropic` | Explicit `"cache_control": {"type": "ephemeral"}` breakpoints on the last custom tool definition, the system prompt block, and the last message block (Anthropic caches only what is marked) |
| `openai` | Automatic; `"prompt_cache_key": "yosh"` groups yosh requests so the prefix cache matches them |
| `meta` | Automatic; `"prompt_cache_key": "yosh"` plus `"prompt_cache_retention": "in_memory"` (its lowest-latency cache tier) |
| `kimi`, `deepseek`, `qwen`, `z.ai` | Automatic (implicit prefix caching; no request fields needed) |
| `openrouter` → `anthropic/*` models | Automatic via one top-level `"cache_control": {"type": "ephemeral"}`; OpenRouter applies the breakpoint to the last cacheable block and advances it as the conversation grows (both the Chat Completions and Responses API styles) |
| `openrouter` → `qwen/*` models | Explicit Anthropic-style cache markers on the first (system) and last USER message, since Alibaba requires explicit cache breakpoints (Chat Completions style); `role:"tool"` results are never marked (part-array content on tool messages is unverified upstream, and a skipped marker only shrinks the cached prefix), and the exact id `deepseek/deepseek-v3.2` is treated the same way per OpenRouter's docs |
| `openrouter` → `google/*` models | Implicit (Gemini 2.5+); yosh deliberately sends NO `cache_control` markers — implicit caching needs no fields and has no cache write or storage costs, while explicit breakpoints would additionally incur cache-write charges (per OpenRouter's prompt-caching docs) |
| `openrouter` → other models | Automatic (no caching fields sent) |

OpenRouter also uses `"prompt_cache_key"` as a sticky-routing hint, so
consecutive yosh requests land on the same backend and cache hits actually hit.

### What the LLM Is Told

Every yo request's system prompt opens with a "You are powered by `<model>`
(provider: `<provider>`)." line followed by a short factual block describing
the shell's LLM configuration: the context window (with a note that context is
compacted automatically above 50% usage), the max output tokens per response,
whether server-side web search is enabled, the configured thinking level, and
that prompt caching is enabled (plus the API base URL, when one is set). The
shell then appends its own tuning text for the provider/model (see the
`rl_yo_enable` prompt callback in `bash-5.2.32/bashline.c`). The compaction
summarizer request is exempt: it uses a minimal system prompt only.

## Usage

Once yosh is running, use the `yo` command:

```bash
# Generate a command
yo find all files larger than 100MB

# Ask a question
yo how do I undo the last git commit?

# Context-aware help (the LLM can see your terminal)
yo why did that command fail?

# Web search
yo what is the weather in mammoth?
```

When the LLM generates a command, it appears prefilled at your prompt. Press Enter to execute it, or edit it first. Press Ctrl-C or enter an empty line to cancel.

Two more commands are parsed directly by the shell (no LLM call, like `yo reset`):

- `yo show last response` — prints the most recent HTTP response body received
  from any API endpoint (LLM calls and model-info lookups alike, success or
  failure), verbatim and un-rendered. Handy for seeing exactly what an API
  error said. Before anything has been received it prints `No response received yet.`
- `yo show documentation` — prints the shell's documentation for the current
  provider and model (the same text the LLM's `docs` tool returns), rendered
  through the markdown renderer (no LLM).

## Source Code

Source code is available at:
```
git@github.com:pizlonator/yosh.git
```

## License

Yosh is based on GNU Bash and GNU Readline, both of which are licensed under the **GNU General Public License version 3 (GPLv3)**.

This means yosh is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Yosh also includes [cJSON](https://github.com/DaveGamble/cJSON) for JSON parsing, which is licensed under the MIT License.

See the `LICENSE.txt` file for the full license text.
