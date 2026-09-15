# AGENTS.md

This file provides guidance to coding agents when working with code in this repository.

## Project Overview

**Yosh** is an LLM-enabled shell - a custom build of GNU Bash 5.2.32 with GNU Readline 8.2.13, compiled using the Fil-C memory-safe compiler toolchain. The entire stack (bash, readline, libcurl, openssl, zlib, libc) is compiled with Fil-C for memory safety.

The key feature is the "yo" command: type `yo <natural language>` and the shell calls an LLM (Anthropic Claude, OpenAI, Kimi, DeepSeek, Qwen, z.ai, Meta Muse, or OpenRouter) to either generate a shell command or answer a question.

## Build System

- **Full build**: `./build.sh` — configures and builds readline + bash from scratch. Requires Fil-C at `/opt/fil`.
- **Incremental build**: `./build_incremental.sh` — faster rebuilds when making code changes.
- **Output**: The resulting shell binary ends up at `../fil-c-5/pizfix/bin/yosh`

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

The yo feature is an **opt-in readline extension** (like history). Readline provides `yo.c` with all LLM logic; bash provides the system prompt via `rl_yo_enable(prompt)` plus two callbacks: a docs callback (`rl_yo_docs_callback_t`) and a tuned-prompt callback (`rl_yo_prompt_callback_t`) that returns the shell-specific "You are a SHELL assistant..." prompt text (implemented in `bash-5.2.32/bashline.c`; see "Request Prompt Composition" below).

**Multi-provider support**: yo supports Anthropic (Claude), OpenAI, Kimi, DeepSeek, Qwen, z.ai, Meta (Muse), and OpenRouter APIs. The provider is selected via `~/.yoconf`. Anthropic, OpenAI, and Kimi each have their own API style; DeepSeek, Qwen, and z.ai use the Chat Completions API style; Meta and OpenRouter speak the OpenAI Responses API style (OpenRouter can also use Chat Completions — see `openrouter_api` below). The tuned prompt a request carries is selected MODEL-PREFIX-based, not API-style-based — see "Request Prompt Composition" step 4. The architecture keeps provider-specific code separated:
- Message building uses provider-aware helpers (`yo_msg_add_tool_use`, `yo_msg_add_tool_result`) that produce native JSON for each provider from C parameters
- HTTP infrastructure is shared (`yo_http_post`) with curl multi-handle and Ctrl-C cancellation
- Request building is per-API-style (`yo_build_anthropic_request`, `yo_build_responses_api_request`, `yo_build_chat_completions_api_request`) with thin per-provider wrappers (`yo_build_meta_request`, `yo_build_openrouter_responses_request`)
- Response parsing is per-API-style (`yo_parse_anthropic_response`, `yo_parse_responses_api_response`, `yo_parse_chat_completions_api_response`), all producing a normalized internal tool_use format

**API details**:
- Anthropic uses the Messages API (`/v1/messages`) with server-side tools (`web_search_20250305`, `web_fetch_20250910`) and `tool_choice {"type":"any"}`
- OpenAI uses the Responses API (`/v1/responses`, NOT Chat Completions) with `{"type":"web_search"}` tool for web search
- Kimi uses the Chat Completions API (`/v1/chat/completions`)
- DeepSeek uses the Chat Completions API (`/chat/completions`)
- Qwen uses the Chat Completions API (`/v1/chat/completions`)
- z.ai uses the Chat Completions API (`/api/paas/v4/chat/completions`)
- Meta uses the OpenAI Responses API (`/v1/responses`, default model `muse-spark-1.3`): no `tool_choice` (only the default auto; sending `"required"` returns HTTP 400), non-strict (compat) tool schemas, web_search grounding, `prompt_cache_retention: "in_memory"`, and `include: ["reasoning.encrypted_content"]` (Muse Spark is a reasoning model)
- OpenRouter uses `https://openrouter.ai/api/v1/` and sends an `X-Title: yosh` header. Default style is Chat Completions (`/chat/completions`, no tool_choice); `openrouter_api responses` switches to the Responses API (`/responses`, `tool_choice: "required"`, non-strict tools, no web_search tool). The tuned prompt follows the MODEL, not the API style: the default model `meta/muse-spark-1.3` prefix-matches `muse`, so OpenRouter **chat** carries the OPENAI-tuned text, while `anthropic/claude-*` models carry the KIMI-tuned text in either style (see "Request Prompt Composition" step 4). No server-side web tools and no web-search prompt paragraph are sent through OpenRouter
- The Responses API always returns `"error": null` on success — error checking must use `cJSON_IsNull()` to avoid false positives
- OpenAI Responses API uses flat items in `input[]` (`{"type":"function_call",...}`, `{"type":"function_call_output",...}`) rather than role-based messages for tool interactions
- OpenAI Responses API uses `"instructions"` for system prompt (not a system message in the input array), `"input"` instead of `"messages"`, `"max_output_tokens"` instead of `"max_completion_tokens"`, and `"output[]"` instead of `"choices[].message"`
- Tool definitions are built separately per provider. OpenAI tools are stricter (e.g., `command.pending` is required and descriptions strongly bias toward command/tool use); Meta, OpenRouter, and Chat Completions providers get the non-strict "compat" schemas
- OpenAI scrollback is sanitized (ANSI/escape sequences stripped) before sending it to the model; Anthropic receives raw scrollback. The "SCROLLBACK TEMPORALITY" reminder (the output shows completed commands from the past) and the "EXAMPLES FORMAT" reminder ride ONLY in the KIMI-tuned prompt text: models selected for the OPENAI-tuned text (openai/meta providers and any gpt/o1/o3/o4/muse-prefixed model — including `meta/muse-spark-1.3` on OpenRouter chat) intentionally do NOT get them. This is a deliberate consequence of the model-prefix-based tuned-prompt selection (see "Request Prompt Composition" step 4)

Response types from the LLM:
- **command** — `{"type":"command","command":"...","explanation":"..."}` — prefills the command in the prompt for the user to review/edit/execute.
- **command with pending** — same but with `"pending":true` — triggers multi-step continuation (see below).
- **chat** — `{"type":"chat","response":"..."}` — prints the response, returns to fresh prompt.
- **scrollback** — `{"type":"scrollback","lines":N}` — yo fetches terminal scrollback and makes a follow-up API call.

### Configuration

**Config file (`~/.yoconf`)** — Read fresh on each yo request. Supports `#` comments. All directives are optional:
```
# Provider: "anthropic", "openai", "kimi", "deepseek", "qwen", "zai" (or
# "z.ai"), "meta" (or "muse"), or "openrouter"
# The provider determines the API style used. When using base_url, the provider
# field selects which API format to use:
#   - anthropic = Anthropic Messages API style
#   - openai = OpenAI Responses API style
#   - meta/muse = OpenAI Responses API style (Meta Muse dialect)
#   - openrouter = OpenAI Chat Completions style by default; the
#     openrouter_api directive switches it to the Responses API style
#   - kimi = OpenAI Chat Completions API style
#   - deepseek = Chat Completions API style
#   - qwen = Chat Completions API style
#   - zai = Chat Completions API style
provider anthropic

# Model name (provider-specific)
model claude-sonnet-4-5-20250929

# API key (optional if using a key file instead)
key sk-ant-api03-...

# Base URL for API requests (optional)
# When set, overrides the default API URL. The provider field determines the
# API style. The endpoint path is appended automatically:
#   - anthropic: /messages
#   - openai/meta: /responses
#   - openrouter: /chat/completions (or /responses with openrouter_api responses)
#   - kimi: /chat/completions
# Example: base_url https://api.moonshot.ai/v1/
# base_url https://your-custom-endpoint.com/v1/

# Extended thinking / reasoning effort (optional)
# Levels: off, none, minimal, low, medium, high, xhigh, max
# thinking high

# API style for OpenRouter only: "chat" (default) or "responses"
# openrouter_api responses

# Chat display prefix/reset (C-style escapes supported, optional quoting with " or ')
# chat_prefix \033[3;36m
# chat_reset \033[0m
```

**API key files** — If `~/.yoconf` doesn't contain a `key` directive (or doesn't exist), yosh looks for the key in standalone files (mode 0600, single line):
- If provider is set in `~/.yoconf`: checks `~/.anthropickey`, `~/.openaikey`, `~/.kimikey`, `~/.deepseekkey`, `~/.qwenkey`, `~/.zaikey`, `~/.metakey`, or `~/.openrouterkey` (matching the provider).
- If no provider is set: checks `~/.anthropickey` → `~/.yoshkey` (legacy) → `~/.openaikey` → `~/.kimikey` → `~/.deepseekkey` → `~/.qwenkey` → `~/.zaikey` → `~/.metakey` → `~/.openrouterkey`. Provider is set automatically based on which file is found.
- If no provider is determined from any source, defaults to Anthropic.

**Provider defaults**:
- Anthropic: model defaults to `claude-sonnet-4-5-20250929`
- OpenAI: model defaults to `gpt-5.2`
- Kimi: model defaults to `kimi-k2.5`
- DeepSeek: model defaults to `deepseek-v4-flash`
- Qwen: model defaults to `qwen-plus`
- z.ai: model defaults to `glm-5.2`
- Meta: model defaults to `muse-spark-1.3`
- OpenRouter: model defaults to `meta/muse-spark-1.3`

**Additional config directives** (all in `~/.yoconf`, re-read on each yo command unless noted):
- **history_limit**: Max conversation exchanges to remember (default 10)
- **token_budget**: REPURPOSED as an alias of the context window: when set, it overrides the model's context window for the usage indicator and the compaction threshold (the old round-robin history pruning by token budget is gone — compaction replaced it). `context_window` takes precedence when both are set.
- **context_window**: Override the model's context window (tokens). Used for the `[N.N%] Thinking...` usage indicator and the compaction trigger. Default: API/registry value for the model (unknown model: 131072).
- **max_output_tokens**: Override the max tokens requested from the LLM (`max_tokens` / `max_output_tokens`). Default: API/registry value for the model (unknown model: 16384).
- **thinking**: Extended thinking / reasoning effort: `off`, `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, or `max`. Unset = disabled. Wired per provider (see "Thinking / Extended Reasoning" below).
- **include_reasoning**: `1`/`0` — force on/off the Responses API `"include": ["reasoning.encrypted_content"]` request. Unset = per-provider default (OpenAI: yes when the model plausibly supports reasoning; Meta: always; OpenRouter: no — sending `include` for models that do not forward encrypted reasoning can 400).
- **openrouter_api**: `chat` (default) or `responses` — selects OpenRouter's API style. Only valid when provider is `openrouter` (any other provider errors out).
- **server_web**: Set to `0` to disable server-side web search and fetch (default: enabled)
- **scrollback_enabled**: Set to `0` to disable PTY proxy / scrollback capture (startup only)
- **scrollback_bytes**: Max scrollback buffer size (default 1MB, startup only)
- **scrollback_lines**: Max lines to return to LLM (default 1000, startup only)
- **chat_prefix**: String printed before chat output (default: `\033[3;36m`, supports C escapes)
- **chat_reset** (alias **color_reset**): String printed after chat output (default: `\033[0m`, supports C escapes)
- **color_prefix**: Styling override for the chat prefix color (default: italic cyan)
- **enable_italic** / **disable_italic** / **enable_bold** / **disable_bold** / **enable_strikethrough** / **disable_strikethrough** / **code_delimiter**: Markdown styling SGR overrides
- **base_url**: Override the default API base URL. The `provider` field determines which API style (and endpoint path) is used. See config example above.
- **Distro detection**: `rl_yo_enable()` reads `/etc/os-release` and appends it to the system prompt.

### Thinking / Extended Reasoning

The `thinking` directive maps to different request parameters per provider
(level string is used verbatim where a string is expected):

- **anthropic** (Messages API, normal requests only — the compaction summarizer never enables it): adds `"thinking": {"type": "enabled", "budget_tokens": B}` with `minimal`→1024, `low`→2048, `medium`→4096, `high`→8192, `xhigh`→16384, `max`→32768. Anthropic requires `max_tokens > budget_tokens`, so `max_tokens` is raised to `B + 1024` when the configured/registry value does not exceed `B`. Anthropic also rejects forced tool choice together with extended thinking, so `tool_choice {"type":"any"}` is OMITTED while thinking is enabled (tools are still sent; the model chooses). Without `thinking` (unset/off) the request is unchanged.
- **openai / meta / openrouter (`openrouter_api responses`)**: adds `"reasoning": {"effort": "<level>"}`.
- **kimi / deepseek / qwen**: adds `"reasoning_effort": "<level>"`.
- **z.ai**: adds `"thinking": {"type": "enabled", "clear_thinking": false}` plus `"reasoning_effort": "max"` for GLM-5.2 and newer (mirrors brainstorm-3 `openai_client.rb#model_supports_reasoning_effort?`).

### Prompt Caching

Per-provider cache-request behavior:

- **anthropic**: up to three `"cache_control": {"type": "ephemeral"}` breakpoints — on the LAST CUSTOM tool definition (custom tools are `{name,description,input_schema}` objects without a top-level `type`; server tools like `web_search_20250305`/`web_fetch_20250910` carry `type` and are never marked — Anthropic's acceptance of `cache_control` on server-tool definitions is unverified; if no custom tool exists the tools breakpoint is skipped), on the system text block (`system` is sent as a one-element content-block array for this purpose), and on the last content block of the last message (a plain-string last-message content is converted to a one-element text-block array to carry the breakpoint). The tools-less compaction summarizer request skips the tool breakpoint.
- **openai / meta / openrouter-responses**: `"prompt_cache_key": "yosh"` groups yosh requests so vendor prefix caches match. Meta additionally gets `"prompt_cache_retention": "in_memory"` (its in-memory cache tier) — never sent to OpenAI/OpenRouter, which reject unknown parameters with HTTP 400. Meta's summarizer request omits the retention flag (one-off request).
- **kimi / deepseek / qwen / z.ai / openrouter-chat**: nothing is sent — these vendors cache request prefixes automatically and accept no cache-control parameters.

### Reasoning Replay

Responses API reasoning items (`output[]` entries with `"type": "reasoning"` and a non-empty `encrypted_content`) are collected by `yo_collect_reasoning_items()`, attached to the normalized tool_use, stored on the session history exchange, and replayed on the next request BEFORE their `function_call` item (`yo_msg_add_tool_use`) — required for multi-turn reasoning models (OpenAI/Meta/OpenRouter-responses). Items without `encrypted_content` are dropped. Kimi-style thinking is replayed analogously: `reasoning_content` from `choices[].message` is stored and re-sent on the next request's assistant `tool_calls` message.

### Model Info & Registry

`yo_get_model_info()` resolves the current model's context window and max output tokens. Resolution order per value: `~/.yoconf` override (`context_window` / `max_output_tokens`) > provider model-info API > built-in registry (ported from brainstorm-3 `lib/shared/model_registry.rb`, case-insensitive prefix match, first match wins; vendor-prefixed IDs like OpenRouter's `meta/muse-spark-1.3` retry with the bare model name after the last `/` when the full string matches nothing) > unknown-model defaults (context 131072, output 16384).

API fetch (best effort, 10s timeout, fully silent on failure), cached per `(provider, model, base_url)` and fetched lazily on first use — NOT re-fetched on every LLM call, and nothing is fetched at startup. When a real network fetch is about to happen (providers with a model-info API, and only on a cache miss), the terminal shows `Fetching model info...` (same styling as the thinking indicator, no newline, prefixed with `\r\033[K` so it safely replaces whatever is on the line); it is erased/replaced when `[N.N%] Thinking...` is printed. Registry-only providers (kimi/deepseek/qwen/z.ai) never show it, and cache hits never re-show it:
- **openrouter**: `GET {base}/model/{id}` (singular `model`); reads `data.context_length` and `data.top_provider.max_completion_tokens`
- **anthropic / openai / meta**: `GET {base}/models/{id}`; sniffs `context_window`/`context_length` and `max_output_tokens`/`max_tokens` at the top level and inside `data`, `data.top_provider`, `top_provider`, and `model` objects
- **kimi / deepseek / qwen / z.ai**: no model-info API; registry only

`max_tokens`/`max_output_tokens` sent in requests = `max_output_tokens` override > API/registry value. The compaction summarizer caps its request at `min(2048, resolved max_output_tokens)`.

### Request Prompt Composition

All three request builders (`yo_build_anthropic_request_ex`, `yo_build_responses_api_request_ex`, `yo_build_chat_completions_api_request`) compose the system prompt for NORMAL requests via `yo_build_prompt_core()`, then append provider-specific paragraphs:

1. `You are powered by <model> (provider: <provider>).`
2. Blank line, then the config-info lines (`yo_build_config_info_lines()`): `Context window: <N> tokens (context is compacted automatically above 50% usage).` / `Max output tokens per response: <N>.` / `Server-side web search: <enabled|disabled>.` / `Thinking level: <level|provider default>.` / `Prompt caching: enabled.` (plus `API base URL: <url>.` when `base_url` is set)
3. Blank line, then the shell system prompt (`yo_system_prompt` — the four-tools guidance + shell intro + OS info)
4. Blank line, then the shell's tuned-prompt text (`yo_shell_tuned_prompt()` — the `rl_yo_prompt_callback_t` callback registered by `bash-5.2.32/bashline.c` as `yosh_get_tuned_prompt`; empty for the anthropic provider, the OpenAI-tuned text for openai/meta providers and for gpt/o1/o3/o4/muse-prefixed models (case-insensitive, also tried on the vendor-stripped name after the last `/`), the Kimi-tuned text otherwise). The two tuned texts live in bashline.c, not in yo.c — yo.c only composes. Selection is deliberately MODEL-PREFIX-based, NOT API-style-based. Consequences: OpenRouter chat with the default model `meta/muse-spark-1.3` gets the OPENAI-tuned text (the vendor-stripped name starts with `muse`); `anthropic/claude-*` models through OpenRouter get the KIMI-tuned text in either API style; muse/gpt/o-series models on Chat Completions providers get the OpenAI-tuned text, which therefore LACKS the "EXAMPLES FORMAT" and "SCROLLBACK TEMPORALITY" reminders (those ride only in the Kimi-tuned text). This asymmetry is intentional, per the model-prefix-based selection spec.
5. The web-search paragraph where it applies today: the Anthropic builder appends its web_search/web_fetch paragraph when `yo_server_web_enabled` and tools are included; the Responses API builder appends the "You have web search available..." paragraph when the provider sends a web_search tool (OpenAI/Meta) and web search is enabled (never for OpenRouter); the Chat Completions builder has none.

The compaction summarizer request is exempt: it passes its own minimal summarizer system prompt via `system_override`, and the builders skip the whole composition in that case (no powered-by line, no config info, no tuned text).

### Context Compaction & Usage Indicator

`yo_call_llm()` prints `[N.N%] Thinking...` before every request, where N.N is the estimated request size (history + system prompt + query + JSON slack) as a percentage of the EFFECTIVE context window (`context_window` override > `token_budget` when set > API/registry context window), computed in per-mille (`yo_usage_permille`) so the indicator carries one decimal digit. Before printing it, `yo_call_llm` resolves BOTH model-info accessors (`yo_get_effective_context_window()` and `yo_get_max_output_tokens()`) — either can trigger the lazy model-info fetch (a `context_window`/`token_budget` override skips the context-window lookup, so the max-output-tokens lookup can still be the first cache miss). The fetch prints its own "Fetching model info..." indicator, which the thinking indicator then replaces; `yo_print_fetching()` is additionally a NO-OP while the thinking line is visible (`yo_thinking_shown`), so a hypothetical late cache miss can never erase the thinking indicator mid-wait.

When the estimate exceeds half of the effective window, `yo_compact_history()` runs (requires ≥2 history entries totaling ≥256 estimated tokens, otherwise it is skipped):
1. Prints `Compacting...` (chat styling, no newline)
2. Sends the oldest ~75% of the history (by estimated tokens) as a FLATTENED PLAIN-TEXT transcript (`yo_build_summary_transcript`: `[USER] <query>` / `[ASSISTANT] <content>` lines — commands rendered as `Suggested command: <cmd> (the user <executed|did not execute> it)` — joined by blank lines) in ONE user message together with the `[compaction] Summarize the following conversation transcript...` instruction to a TOOLS-LESS request (`yo_call_api_summarize`: no tools array, no tool_choice, minimal summarizer system prompt, output capped at 2048; the Anthropic variant also skips the web-search beta header and keeps the system/final-message cache breakpoints). The transcript must be plain text because the summarizer request defines no tools — replaying native tool_use/tool_result blocks would reference undefined tools and be rejected (Responses-style servers likewise reject unpaired function calls)
3. Rebuilds the history as ONE synthetic summary exchange (query `[context compacted] Summary of the earlier conversation:`, chat response with the summary text, tool_use id `compaction_summary`, executed=1) followed by the kept ~25% of entries
4. `yo_call_llm()` redraws the indicator as `[M.N%] Thinking...` with the post-compaction estimate

Compaction is best-effort: if the summarizer call fails for a non-cancel reason (e.g. HTTP 500), the history is left untouched and the original estimate stands. If the summarizer request is Ctrl-C CANCELLED, `yo_call_llm` aborts the whole operation (returns 0 without redrawing the thinking indicator and without sending the main request) — "Cancelled" was already printed by the HTTP layer and the user must not have to press Ctrl-C twice.

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

Self-pipe trick: SIGINT handler writes to a pipe, `curl_multi_poll()` watches both curl sockets and the signal pipe for near-instantaneous cancellation during API calls. There is no API timeout; Ctrl-C is the user-controlled cancel path.

### Key Files

| File | Purpose |
|------|---------|
| `readline-8.2.13/yo.c` | All LLM code: multi-provider API calls (Anthropic + OpenAI + Kimi + DeepSeek + Qwen + z.ai + Meta + OpenRouter), model registry, compaction, session memory, PTY proxy, scrollback, continuation |
| `readline-8.2.13/yo.h` | Public API: `rl_yo_enable()`, `rl_yo_docs_callback_t`, `rl_yo_prompt_callback_t`, `rl_yo_accept_line()`, `rl_yo_get_scrollback()` |
| `bash-5.2.32/bashline.c` | Calls `rl_yo_enable()` with yosh's system prompt, docs callback (`yosh_get_documentation`), and tuned-prompt callback (`yosh_get_tuned_prompt` — the shell-specific "CRITICAL: You are a SHELL assistant..." texts) |
| `bash-5.2.32/shell.c` | Main shell init; readline must init before job control |

## Development Workflow

1. Make code changes in `bash-5.2.32/` or `readline-8.2.13/`
2. Run `./build_incremental.sh` to rebuild
3. Test at `../fil-c-5/pizfix/bin/yosh`
