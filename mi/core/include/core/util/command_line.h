/*
 * Created: 2026/1/4
 * Author:  GitHub Copilot
 * See LICENSE for licensing.
 */

#ifndef MI_CORE_UTIL_COMMAND_LINE_H
#define MI_CORE_UTIL_COMMAND_LINE_H

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "core/common.h"

MI_NAMESPACE_BEGIN

// A tiny, self-contained command line tokenizer + command registry.
// Designed for in-app consoles and renderer debug UIs.

struct CommandLineToken {
    std::string text;
    size_t begin = 0; // inclusive
    size_t end = 0;   // exclusive
    bool quoted = false; // token was produced from a quoted segment
};

struct CommandLineTokenizeResult {
    std::vector<CommandLineToken> tokens;
    bool ends_with_space = false;
    bool has_unclosed_quote = false;
};

// Tokenize a command line.
// Rules (intentionally simple):
// - whitespace splits tokens
// - double quotes (") group spaces into a token
// - single quote (') escapes the next character verbatim (including quotes and spaces)
CommandLineTokenizeResult CommandLineTokenize(std::string_view line);

// Completion request/response are designed for end-of-line completion.
// (cursor_pos is optional for now; default is end-of-line)
struct CommandLineCompletionRequest {
    std::string_view line;
    size_t cursor_pos = std::string_view::npos;
};

enum class CommandLineCompletionKind {
    kKeyword,
    kPath,
    kCustomized,
};

struct CommandLineCompletionItem {
    std::string text;
    CommandLineCompletionKind kind = CommandLineCompletionKind::kKeyword;
    bool is_dir = false; // only meaningful when kind==kPath
};

struct CommandLineCompletionResult {
    // Replace substring [replace_begin, replace_end) of the original line with selected item.text.
    size_t replace_begin = 0;
    size_t replace_end = 0;
    std::vector<CommandLineCompletionItem> items;
};

enum class CommandTokenType {
    kKeywordSet,
    kPath,
    kFree,
};

struct CommandTokenSpec {
    CommandTokenType type = CommandTokenType::kFree;

    // For kKeywordSet.
    std::vector<std::string> keywords;

    // If the user omits this token (i.e. not enough input tokens), this value can be used.
    // Empty means no default.
    std::string default_value;

    // Optional, for debugging/help messages.
    std::string name;

    // For kFree: optional custom completion callback.
    // Input: current token prefix (already unquoted/unescaped by tokenizer).
    // Output: candidate strings to replace the current token.
    using FreeCompletionFn = std::function<std::vector<std::string>(std::string_view prefix)>;
    FreeCompletionFn free_completion;

    static CommandTokenSpec KeywordSet(std::vector<std::string> keywords,
                                      std::string default_value = {},
                                      std::string name = {});
    static CommandTokenSpec Path(std::string default_value = {}, std::string name = {});
    static CommandTokenSpec Free(std::string default_value = {}, std::string name = {}, FreeCompletionFn completion = {});
};

enum class CommandMatchKind {
    kNoMatch,
    // The current input is a valid prefix but not enough to execute.
    kPartial,
    // The command keyword matches, but arguments are invalid (e.g., wrong keyword/arg count).
    kBadMatch,
    // Executable (missing tokens have been satisfied by defaults).
    kFull,
};

class Command;

struct CommandMatchResult {
    CommandMatchKind kind {CommandMatchKind::kNoMatch};
    const Command *command {};
    // Parsed args aligned with pattern (missing tokens filled with defaults when possible).
    std::vector<std::string> args;
};

class Command {
public:
    using ExecuteCallback = std::function<void(const CommandMatchResult &)>;

    Command(std::string name,
            std::vector<CommandTokenSpec> pattern,
            ExecuteCallback execute = {});

    FORCEINLINE static std::unique_ptr<Command> Make(
        std::string name,
        std::vector<CommandTokenSpec> pattern,
        ExecuteCallback execute = {}
    ) {
        return std::make_unique<Command>(std::move(name), std::move(pattern), std::move(execute));
    }

    const std::string &GetName() const { return name_; }
    const std::vector<CommandTokenSpec> &GetPattern() const { return pattern_; }

    CommandMatchResult Match(const std::vector<std::string> &tokens) const;

    // Get a usage string, e.g. `s <cvar> <value>` or `cmd <a> [b]`.
    std::string ToString() const;

    // Compute completion candidates for end-of-line completion.
    // resource_root: used for CommandTokenType::kPath completion as the root directory.
    std::optional<CommandLineCompletionResult> Complete(
        const CommandLineCompletionRequest &req,
        const std::filesystem::path &resource_root
    ) const;

    void Execute(const CommandMatchResult &match) const;

private:
    std::string name_;
    std::vector<CommandTokenSpec> pattern_;
    ExecuteCallback execute_;
};

class CommandRegistry {
public:
    void Register(std::unique_ptr<Command> cmd);

    void MakeAndRegister(
        std::string name,
        std::vector<CommandTokenSpec> pattern,
        Command::ExecuteCallback execute = {}
    ) {
        Register(Command::Make(std::move(name), std::move(pattern), std::move(execute)));
    }

    // Match best command for full execution.
    std::optional<CommandMatchResult> Match(std::string_view line) const;

    // Gather completion candidates from all commands.
    CommandLineCompletionResult Complete(const CommandLineCompletionRequest &req) const;

    // Convenience wrapper for end-of-line completion.
    CommandLineCompletionResult Complete(std::string_view line_prefix_to_cursor) const;

    const std::vector<std::unique_ptr<Command>> &GetCommands() const { return commands_; }

    static CommandRegistry & Get();

private:
    std::vector<std::unique_ptr<Command>> commands_;

    // Detect ambiguity between two command patterns. If ambiguous, return a message.
    static std::optional<std::string> DetectAmbiguity(const Command &a, const Command &b);
};

MI_NAMESPACE_END

#endif // MI_CORE_UTIL_COMMAND_LINE_H

