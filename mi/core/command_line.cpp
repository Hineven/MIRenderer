/*
 * Created: 2026/1/4
 * Author:  GitHub Copilot
 * See LICENSE for licensing.
 */

#include "core/util/command_line.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

#include "core/infra.h"

MI_NAMESPACE_BEGIN

static inline bool IsWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

CommandLineTokenizeResult CommandLineTokenize(std::string_view line) {
    CommandLineTokenizeResult out;

    std::string cur;
    size_t cur_begin = 0;
    bool in_token = false;
    bool in_quotes = false;
    bool token_quoted = false;

    auto flush = [&](size_t end_pos) {
        if (!in_token) return;
        CommandLineToken t;
        t.text = std::move(cur);
        t.begin = cur_begin;
        t.end = end_pos;
        t.quoted = token_quoted;
        out.tokens.emplace_back(std::move(t));
        cur.clear();
        in_token = false;
        token_quoted = false;
    };

    const size_t n = line.size();
    for (size_t i = 0; i < n; ++i) {
        char c = line[i];

        if (!in_quotes && IsWhitespace(c)) {
            if (in_token) {
                flush(i);
            }
            continue;
        }

        if (!in_token) {
            in_token = true;
            cur_begin = i;
        }

        if (c == '"') {
            in_quotes = !in_quotes;
            token_quoted = true;
            continue;
        }

        if (c == '\'') {
            // Escape next char verbatim.
            if (i + 1 < n) {
                cur.push_back(line[i + 1]);
                i++;
            } else {
                // Trailing escape marker, treat as literal.
                cur.push_back(c);
            }
            continue;
        }

        cur.push_back(c);
    }

    if (in_token) {
        flush(n);
    }

    out.ends_with_space = (line.empty() || IsWhitespace(line.back()));
    out.has_unclosed_quote = in_quotes;
    return out;
}

CommandTokenSpec CommandTokenSpec::KeywordSet(std::vector<std::string> keywords,
                                             std::string default_value,
                                             std::string name) {
    CommandTokenSpec s;
    s.type = CommandTokenType::kKeywordSet;
    s.keywords = std::move(keywords);
    s.default_value = std::move(default_value);
    s.name = std::move(name);
    return s;
}

CommandTokenSpec CommandTokenSpec::Path(std::string default_value, std::string name) {
    CommandTokenSpec s;
    s.type = CommandTokenType::kPath;
    s.default_value = std::move(default_value);
    s.name = std::move(name);
    return s;
}

CommandTokenSpec CommandTokenSpec::Free(std::string default_value, std::string name, FreeCompletionFn completion) {
    CommandTokenSpec s;
    s.type = CommandTokenType::kFree;
    s.default_value = std::move(default_value);
    s.name = std::move(name);
    s.free_completion = std::move(completion);
    return s;
}

Command::Command(std::string name, std::vector<CommandTokenSpec> pattern, ExecuteCallback execute)
    : name_(std::move(name)), pattern_(std::move(pattern)), execute_(std::move(execute)) {}

static bool HasDefault(const CommandTokenSpec &s) {
    return !s.default_value.empty();
}

CommandMatchResult Command::Match(const std::vector<std::string> &tokens) const {
    CommandMatchResult result;
    result.command = this;

    if (tokens.size() > pattern_.size()) {
        result.kind = CommandMatchKind::kNoMatch;
        return result;
    }

    result.args.reserve(pattern_.size());

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto &spec = pattern_[i];
        const std::string &tok = tokens[i];

        if (spec.type == CommandTokenType::kKeywordSet) {
            bool ok = false;
            for (const auto &kw : spec.keywords) {
                if (tok == kw) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                result.kind = CommandMatchKind::kNoMatch;
                result.args.clear();
                return result;
            }
        }

        // kPath/kFree accept anything.
        result.args.emplace_back(tok);
    }

    for (size_t i = tokens.size(); i < pattern_.size(); ++i) {
        const auto &spec = pattern_[i];
        if (HasDefault(spec)) {
            result.args.emplace_back(spec.default_value);
        } else {
            result.kind = CommandMatchKind::kPartial;
            return result;
        }
    }

    result.kind = CommandMatchKind::kFull;
    return result;
}

std::string Command::ToString() const {
    std::string out;

    // Prefer a readable explicit keyword in the first position if it exists.
    // Otherwise fall back to the command name.
    bool printed_first = false;

    for (size_t i = 0; i < pattern_.size(); ++i) {
        const auto &spec = pattern_[i];

        auto token_repr = [&]() -> std::string {
            // If this token can be omitted (has default), treat it as optional.
            const bool optional = !spec.default_value.empty();

            auto wrap_optional = [&](std::string s) {
                if (!optional) return s;
                return std::string("[") + s + "]";
            };

            if (spec.type == CommandTokenType::kKeywordSet) {
                // For keyword sets: use a concrete keyword if single; otherwise show {a|b|c}.
                if (spec.keywords.size() == 1) {
                    return wrap_optional(spec.keywords[0]);
                }
                std::string s = "{";
                for (size_t k = 0; k < spec.keywords.size(); ++k) {
                    if (k) s += "|";
                    s += spec.keywords[k];
                }
                s += "}";
                return wrap_optional(std::move(s));
            }

            // Non-keyword tokens: show by name if provided, else show by type.
            std::string name = spec.name;
            if (name.empty()) {
                switch (spec.type) {
                    case CommandTokenType::kPath: name = "path"; break;
                    case CommandTokenType::kFree: name = "value"; break;
                    default: name = "arg"; break;
                }
            }

            std::string s = std::string("<") + name + ">";
            return wrap_optional(std::move(s));
        }();

        // Special case: allow first token to be embedded into command name
        // if it's a single fixed keyword.
        if (!printed_first) {
            if (spec.type == CommandTokenType::kKeywordSet && spec.keywords.size() == 1 && spec.default_value.empty()) {
                out += spec.keywords[0];
                printed_first = true;
                continue;
            }

            out += name_;
            printed_first = true;
            // fall through to also print this token as an argument
        }

        out += " ";
        out += token_repr;
    }

    if (!printed_first) {
        out = name_;
    }

    return out;
}

static std::string NormalizeRelPathForCompletion(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(c == '\\' ? '/' : c);
    }
    return out;
}

static bool IsPathSafeRelative(const std::filesystem::path &p) {
    if (p.is_absolute()) return false;
    for (const auto &part : p) {
        if (part == "..") return false;
    }
    return true;
}

static std::vector<CommandLineCompletionItem> CompletePath(
    const std::filesystem::path &resource_root,
    std::string_view prefix
) {
    std::vector<CommandLineCompletionItem> items;

    const std::string norm = NormalizeRelPathForCompletion(prefix);

    std::string dir_part;
    std::string base_part;
    const auto slash_pos = norm.find_last_of('/');
    if (slash_pos == std::string::npos) {
        dir_part = "";
        base_part = norm;
    } else {
        dir_part = norm.substr(0, slash_pos + 1);
        base_part = norm.substr(slash_pos + 1);
    }

    std::filesystem::path rel_dir = std::filesystem::path(dir_part);
    if (!IsPathSafeRelative(rel_dir)) return items;

    std::filesystem::path dir_fs = resource_root / rel_dir;
    std::error_code ec;
    if (!std::filesystem::exists(dir_fs, ec) || !std::filesystem::is_directory(dir_fs, ec)) {
        return items;
    }

    for (const auto &entry : std::filesystem::directory_iterator(dir_fs, ec)) {
        if (ec) break;
        const auto filename_path = entry.path().filename();
        const std::string filename = filename_path.string();
        if (!base_part.empty() && filename.rfind(base_part, 0) != 0) continue;

        CommandLineCompletionItem item;
        item.kind = CommandLineCompletionKind::kPath;
        item.is_dir = entry.is_directory(ec);
        item.text = dir_part + filename;
        if (item.is_dir) item.text += "/";
        items.emplace_back(std::move(item));
    }

    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.text < b.text;
    });

    items.erase(std::unique(items.begin(), items.end(), [](const auto &a, const auto &b) {
        return a.text == b.text;
    }), items.end());

    return items;
}

std::optional<CommandLineCompletionResult> Command::Complete(
    const CommandLineCompletionRequest &req,
    const std::filesystem::path &resource_root
) const {
    std::string_view line = req.line;
    size_t cursor = req.cursor_pos;
    if (cursor == std::string_view::npos) cursor = line.size();
    if (cursor > line.size()) cursor = line.size();

    // Current implementation: end-of-line completion.
    if (cursor != line.size()) return std::nullopt;

    auto tok = CommandLineTokenize(line);

    size_t target_index = 0;
    size_t replace_begin = line.size();
    size_t replace_end = line.size();

    if (tok.tokens.empty()) {
        target_index = 0;
    } else if (tok.ends_with_space) {
        target_index = tok.tokens.size();
    } else {
        target_index = tok.tokens.size() - 1;
        replace_begin = tok.tokens.back().begin;
        replace_end = tok.tokens.back().end;
    }

    if (target_index >= pattern_.size()) return std::nullopt;

    // Verify tokens before target_index match.
    for (size_t i = 0; i < target_index; ++i) {
        if (i >= tok.tokens.size()) return std::nullopt;
        const auto &spec = pattern_[i];
        const auto &val = tok.tokens[i].text;
        if (spec.type == CommandTokenType::kKeywordSet) {
            bool ok = false;
            for (const auto &kw : spec.keywords) {
                if (val == kw) {
                    ok = true;
                    break;
                }
            }
            if (!ok) return std::nullopt;
        }
    }

    const auto &spec = pattern_[target_index];

    CommandLineCompletionResult res;
    res.replace_begin = replace_begin;
    res.replace_end = replace_end;

    if (spec.type == CommandTokenType::kKeywordSet) {
        std::string pref;
        if (!tok.tokens.empty() && !tok.ends_with_space) pref = tok.tokens.back().text;

        std::set<std::string> uniq;
        for (const auto &kw : spec.keywords) {
            if (!pref.empty() && kw.rfind(pref, 0) != 0) continue;
            uniq.insert(kw);
        }

        for (const auto &kw : uniq) {
            res.items.push_back(CommandLineCompletionItem{kw, CommandLineCompletionKind::kKeyword, false});
        }
        return res;
    }

    if (spec.type == CommandTokenType::kPath) {
        std::string path_prefix;
        if (!tok.tokens.empty() && !tok.ends_with_space) path_prefix = tok.tokens.back().text;
        res.items = CompletePath(resource_root, path_prefix);
        return res;
    }

    if (spec.type == CommandTokenType::kFree) {
        if (!spec.free_completion) return std::nullopt;

        std::string pref;
        if (!tok.tokens.empty() && !tok.ends_with_space) pref = tok.tokens.back().text;

        auto candidates = spec.free_completion(pref);
        std::set<std::string> uniq(candidates.begin(), candidates.end());
        for (const auto &c : uniq) {
            res.items.push_back(CommandLineCompletionItem{c, CommandLineCompletionKind::kCustomized, false});
        }
        return res;
    }

    return std::nullopt;
}

void Command::Execute(const CommandMatchResult &match) const {
    if (execute_) execute_(match);
}

static bool ContainsFreeOrPath(const CommandTokenSpec &s) {
    return s.type == CommandTokenType::kFree || s.type == CommandTokenType::kPath;
}

std::optional<std::string> CommandRegistry::DetectAmbiguity(const Command &a, const Command &b) {
    const auto &pa = a.GetPattern();
    const auto &pb = b.GetPattern();
    const size_t max_i = std::min(pa.size(), pb.size());

    // Common prefix analysis.
    for (size_t i = 0; i < max_i; ++i) {
        const auto &sa = pa[i];
        const auto &sb = pb[i];

        if (sa.type == CommandTokenType::kKeywordSet && sb.type == CommandTokenType::kKeywordSet) {
            bool intersect = false;
            for (const auto &kwa : sa.keywords) {
                for (const auto &kwb : sb.keywords) {
                    if (kwa == kwb) {
                        intersect = true;
                        break;
                    }
                }
                if (intersect) break;
            }
            if (!intersect) return std::nullopt;
            continue;
        }

        if (ContainsFreeOrPath(sa) || ContainsFreeOrPath(sb)) {
            return std::format("Ambiguous patterns between '{}' and '{}' around token {}", a.GetName(), b.GetName(), i);
        }
    }

    if (pa.size() == pb.size()) {
        return std::format("Ambiguous patterns between '{}' and '{}' (identical keyword prefix)", a.GetName(), b.GetName());
    }

    auto can_exec_with_defaults = [](const std::vector<CommandTokenSpec> &p, size_t start) {
        for (size_t i = start; i < p.size(); ++i) {
            if (p[i].default_value.empty()) return false;
        }
        return true;
    };

    if (pa.size() < pb.size()) {
        if (can_exec_with_defaults(pb, pa.size())) {
            return std::format("Ambiguous: '{}' is a prefix of '{}' (defaults make both executable)", a.GetName(), b.GetName());
        }
    } else {
        if (can_exec_with_defaults(pa, pb.size())) {
            return std::format("Ambiguous: '{}' is a prefix of '{}' (defaults make both executable)", b.GetName(), a.GetName());
        }
    }

    return std::nullopt;
}

void CommandRegistry::Register(std::unique_ptr<Command> cmd) {
    for (const auto &c : commands_) {
        if (auto msg = DetectAmbiguity(*c, *cmd)) {
            MI_WARN("CommandRegistry: {}", *msg);
        }
    }
    commands_.emplace_back(std::move(cmd));
}

std::optional<CommandMatchResult> CommandRegistry::Match(std::string_view line) const {
    auto tok = CommandLineTokenize(line);

    std::vector<std::string> tokens;
    tokens.reserve(tok.tokens.size());
    for (const auto &t : tok.tokens) tokens.emplace_back(t.text);

    struct Candidate {
        const Command *cmd = nullptr;
        CommandMatchResult match;
        long long score = 0;
        int exact_keyword_matches = 0;
        int default_used = 0;
    };

    auto score_candidate = [&](const Command &cmd, const CommandMatchResult &m) -> Candidate {
        Candidate c;
        c.cmd = &cmd;
        c.match = m;

        const auto &pat = cmd.GetPattern();

        // Token-by-token preference:
        // - Prefer matching more keyword tokens (i.e. deeper command set).
        // - Prefer exact keyword matches over defaults.
        // - Penalize consuming defaults for missing user input.
        // - Keep old "specificity" as a very small tie-breaker.
        long long s = 0;
        int exact_kw = 0;
        int def_used = 0;

        const size_t user_n = tokens.size();
        const size_t pat_n = pat.size();

        // How many leading tokens exist in both.
        const size_t n = std::min(user_n, pat_n);

        for (size_t i = 0; i < n; ++i) {
            const auto &spec = pat[i];
            if (spec.type == CommandTokenType::kKeywordSet) {
                // Exact keyword match is the strongest signal.
                bool exact = false;
                for (const auto &kw : spec.keywords) {
                    if (kw == tokens[i]) {
                        exact = true;
                        break;
                    }
                }
                if (exact) {
                    s += 10000;
                    exact_kw++;
                }
            } else {
                // For non-keywords, give a small reward for being present.
                s += 10;
            }
        }

        // Missing tokens satisfied by defaults (only meaningful for full match).
        if (m.kind == CommandMatchKind::kFull) {
            for (size_t i = user_n; i < pat_n; ++i) {
                if (!pat[i].default_value.empty()) {
                    def_used++;
                }
            }
        }

        // Prefer commands where the user has typed more of the command keyword sequence.
        // (E.g. `c pos` should beat `c dir` when both share `c` but second token differs.)
        s += (long long)exact_kw * 1000;

        // Penalize using defaults (so explicitly provided args win).
        s -= (long long)def_used * 50;

        // Very small old specificity tie-break.
        int specificity = 0;
        for (const auto &sp : pat) {
            if (sp.type == CommandTokenType::kKeywordSet) specificity += 3;
            else if (sp.type == CommandTokenType::kPath) specificity += 2;
        }
        s += specificity;

        c.score = s;
        c.exact_keyword_matches = exact_kw;
        c.default_used = def_used;
        return c;
    };

    std::vector<Candidate> full_cands;

    for (const auto &cmd : commands_) {
        auto m = cmd->Match(tokens);
        if (m.kind != CommandMatchKind::kFull) continue;
        full_cands.push_back(score_candidate(*cmd, m));
    }

    if (!full_cands.empty()) {
        std::sort(full_cands.begin(), full_cands.end(), [](const Candidate &a, const Candidate &b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.exact_keyword_matches != b.exact_keyword_matches) return a.exact_keyword_matches > b.exact_keyword_matches;
            if (a.default_used != b.default_used) return a.default_used < b.default_used;
            return a.cmd->GetName() < b.cmd->GetName();
        });

        if (full_cands.size() > 1 && full_cands[0].score == full_cands[1].score) {
            MI_WARN("CommandRegistry: multiple commands match input. Best='{}' Other='{}'",
                    full_cands[0].cmd->GetName(), full_cands[1].cmd->GetName());
        }

        return full_cands[0].match;
    }

    // No full match.
    // If the user has fully typed the first token and it uniquely maps to >=1 command(s),
    // return kBadMatch to allow consoles to show usage for the intended command.
    if (tokens.empty()) return std::nullopt;

    // Collect commands whose keyword prefix matches all user tokens up to the point it can.
    // This allows `c pos` to recommend `c pos ...` instead of an arbitrary `c dir ...`.
    std::vector<Candidate> prefix_cands;

    for (const auto &cmd_uptr : commands_) {
        const Command *cmd = cmd_uptr.get();
        if (!cmd) continue;
        const auto &pat = cmd->GetPattern();
        if (pat.empty()) continue;

        bool ok = true;
        const size_t n = std::min(tokens.size(), pat.size());
        for (size_t i = 0; i < n; ++i) {
            const auto &spec = pat[i];
            if (spec.type != CommandTokenType::kKeywordSet) {
                // Stop checking keyword prefix at first non-keyword pattern token.
                // If user still has more tokens beyond this point, treat as not a prefix-candidate.
                if (i < tokens.size()) {
                    // user token at i corresponds to non-keyword; accept for recommendation.
                }
                break;
            }
            bool exact = false;
            for (const auto &kw : spec.keywords) {
                if (kw == tokens[i]) {
                    exact = true;
                    break;
                }
            }
            if (!exact) {
                ok = false;
                break;
            }
        }

        if (!ok) continue;

        CommandMatchResult bad;
        bad.kind = CommandMatchKind::kBadMatch;
        bad.command = cmd;
        bad.args = tokens;
        prefix_cands.push_back(score_candidate(*cmd, bad));
    }

    if (prefix_cands.empty()) {
        return std::nullopt;
    }

    std::sort(prefix_cands.begin(), prefix_cands.end(), [](const Candidate &a, const Candidate &b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.exact_keyword_matches != b.exact_keyword_matches) return a.exact_keyword_matches > b.exact_keyword_matches;
        if (a.default_used != b.default_used) return a.default_used < b.default_used;
        return a.cmd->GetName() < b.cmd->GetName();
    });

    return prefix_cands[0].match;
}

CommandLineCompletionResult CommandRegistry::Complete(const CommandLineCompletionRequest &req) const {
    CommandLineCompletionResult merged;
    merged.replace_begin = req.line.size();
    merged.replace_end = req.line.size();

    std::set<std::string> seen;
    const auto resource_root = GetInfra().GetResourceDirectory();

    bool first = true;
    for (const auto &cmd : commands_) {
        auto r = cmd->Complete(req, resource_root);
        if (!r) continue;

        if (first) {
            merged.replace_begin = r->replace_begin;
            merged.replace_end = r->replace_end;
            first = false;
        } else {
            merged.replace_begin = std::max(merged.replace_begin, r->replace_begin);
            merged.replace_end = std::min(merged.replace_end, r->replace_end);
        }

        for (auto &it : r->items) {
            if (!seen.insert(it.text).second) continue;
            merged.items.emplace_back(std::move(it));
        }
    }

    std::sort(merged.items.begin(), merged.items.end(), [](const auto &a, const auto &b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.text < b.text;
    });

    return merged;
}

CommandLineCompletionResult CommandRegistry::Complete(std::string_view line_prefix_to_cursor) const {
    CommandLineCompletionRequest req;
    req.line = line_prefix_to_cursor;
    req.cursor_pos = line_prefix_to_cursor.size();
    return Complete(req);
}

CommandRegistry &CommandRegistry::Get() {
    static CommandRegistry instance;
    return instance;
}

MI_NAMESPACE_END
