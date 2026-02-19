# Yosh 0.1.2

Yosh is an LLM-enabled shell. It's a custom build of GNU Bash 5.2.32 with GNU Readline 8.2.13, featuring built-in LLM integration for natural language command generation and assistance. It supports both **Anthropic Claude** and **OpenAI** as providers.

The key feature is the **yo** command: type `yo <natural language>` at the prompt and the shell calls an LLM to either generate a shell command or answer a question directly.

## Features

- **Natural language to shell commands**: Type `yo list all python files modified today` and get an executable command prefilled at your prompt
- **Interactive Q&A**: Ask questions like `yo what does the -exec flag in find do?` and get answers inline
- **Multi-provider**: Supports Anthropic Claude and OpenAI models, configurable via `~/.yoconf`
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
   ```

   **Option B: Config file (more control)**
   ```bash
   cat > ~/.yoconf << 'EOF'
   # Provider: "anthropic" (default) or "openai"
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
# Provider: "anthropic" (default) or "openai"
provider anthropic

# Model name (provider-specific, optional)
# Anthropic default: claude-sonnet-4-20250514
# OpenAI default: gpt-4o-mini
model claude-sonnet-4-20250514

# API key (optional if using a key file instead)
key sk-ant-api03-...

# Chat display prefix/reset (optional, supports C-style escapes)
# chat_prefix \033[3;36m
# chat_reset \033[0m
```

Values for `chat_prefix` and `chat_reset` support C-style escape sequences (`\033`, `\n`, `\t`, `\\`) and optional quoting with `"` or `'` to preserve whitespace.

### API Key Files

If `~/.yoconf` doesn't contain a `key` directive (or doesn't exist), yosh looks for the API key in a standalone key file (mode 0600, single line with the key):

- If `provider` is set in `~/.yoconf`: checks `~/.anthropickey` or `~/.openaikey` (matching the provider).
- If no provider is set: checks `~/.anthropickey`, then `~/.yoshkey` (legacy), then `~/.openaikey`. The provider is set automatically based on which file is found.
- If no provider is determined from any source, it defaults to Anthropic.

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `YO_MODEL` | Provider default | Override the model from `~/.yoconf` |
| `YO_HISTORY_LIMIT` | `10` | Max conversation exchanges to remember |
| `YO_TOKEN_BUDGET` | `4096` | Max tokens for history context |
| `YO_SCROLLBACK_ENABLED` | `1` | Set to `0` to disable terminal scrollback capture |
| `YO_SCROLLBACK_BYTES` | `1048576` | Max scrollback buffer size (1MB) |
| `YO_SCROLLBACK_LINES` | `1000` | Max lines to return to the LLM |
| `YO_SERVER_WEB` | `1` | Set to `0` to disable server-side web search |

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
