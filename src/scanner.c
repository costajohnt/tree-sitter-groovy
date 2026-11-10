// Hand-written external scanner for tree-sitter-groovy.
//
// Handles §6.2 slashy-string-start and §6.6 line/block/groovydoc
// comments. Both start with `/`; combining the dispatch keeps the
// in-grammar lexer from partially consuming the leading `/`.
//
// Stateless — `create` returns NULL and the runtime calls
// `destroy` / `serialize` / `deserialize` as no-ops. When a future
// branch needs persistent state, introduce a struct then.

#include "tree_sitter/parser.h"

#include <string.h>

enum TokenType {
    SLASHY_STRING_START,
    LINE_COMMENT,
    BLOCK_COMMENT,
    GROOVYDOC_COMMENT,
    COMMAND_NAMED_KEY,
    SUBSCRIPT_OPEN,
    COMMAND_LITERAL_START,
    COMMAND_CLOSURE_START,
    CALL_OPEN,
    CAST_VALUE_START,
    STRING_BODY,
    RECORD_KEYWORD,
    ERROR_SENTINEL,
};

static inline bool is_ident_start(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || c == '$' || c > 127;
}

static inline bool is_ident_part(int32_t c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

// SPECIFICATION.md §5.5 — the key of a named argument in a
// parenthesis-free call (`exclude group: 'a'`). Matches an identifier
// on the current line that is followed by `:` (not `::`), consuming
// only the identifier. A newline before the key means a new statement
// (a Spock `where:` label, say), so no match.
static bool scan_command_named_key(TSLexer *lexer) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, true);
    }
    if (!is_ident_start(lexer->lookahead)) {
        return false;
    }
    // Keep the first few chars to reject `case:` / `default:`, which
    // are switch labels, not keys.
    char word[8];
    unsigned len = 0;
    while (is_ident_part(lexer->lookahead)) {
        if (len < sizeof(word)) {
            word[len] = (char)lexer->lookahead;
        }
        len++;
        lexer->advance(lexer, false);
    }
    if ((len == 4 && memcmp(word, "case", 4) == 0)
        || (len == 7 && memcmp(word, "default", 7) == 0)) {
        return false;
    }
    lexer->mark_end(lexer);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, false);
    }
    if (lexer->lookahead != ':') {
        return false;
    }
    lexer->advance(lexer, false);
    if (lexer->lookahead == ':') {
        return false;
    }
    lexer->result_symbol = COMMAND_NAMED_KEY;
    return true;
}

static inline void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
}

static inline bool is_whitespace(int32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void *tree_sitter_groovy_external_scanner_create(void) {
    return NULL;
}

void tree_sitter_groovy_external_scanner_destroy(void *payload) {
    (void)payload;
}

unsigned tree_sitter_groovy_external_scanner_serialize(
    void *payload,
    char *buffer
) {
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_groovy_external_scanner_deserialize(
    void *payload,
    const char *buffer,
    unsigned length
) {
    (void)payload;
    (void)buffer;
    (void)length;
}

bool tree_sitter_groovy_external_scanner_scan(
    void *payload,
    TSLexer *lexer,
    const bool *valid_symbols
) {
    (void)payload;

    // SPECIFICATION.md §5.7 — inside a string body `//` and `/*` are text,
    // never a comment; the grammar's text tokens take them.
    if (valid_symbols[STRING_BODY] && !valid_symbols[ERROR_SENTINEL]) {
        return false;
    }

    // SPECIFICATION.md §5.15 — zero-width, after a cast's `)`: the value
    // starts on the same line with a name (not `in`, `as` or `instanceof`,
    // which continue a parenthesized expression), a string or a number.
    if (valid_symbols[CAST_VALUE_START] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        int32_t c = lexer->lookahead;
        lexer->mark_end(lexer);
        if (c == '\'' || c == '"' || (c >= '0' && c <= '9')) {
            lexer->result_symbol = CAST_VALUE_START;
            return true;
        }
        if (is_ident_start(c)) {
            char word[12];
            unsigned len = 0;
            while (is_ident_part(lexer->lookahead)) {
                if (len < sizeof(word)) {
                    word[len] = (char)lexer->lookahead;
                }
                len++;
                lexer->advance(lexer, false);
            }
            if ((len == 2 && (memcmp(word, "in", 2) == 0 || memcmp(word, "as", 2) == 0))
                || (len == 10 && memcmp(word, "instanceof", 10) == 0)) {
                return false;
            }
            lexer->result_symbol = CAST_VALUE_START;
            return true;
        }
        return false;
    }

    // SPECIFICATION.md §5.10 — a `[` on the current line continues the
    // expression (subscript or array type). Only spaces and tabs are
    // skipped; at a newline we fall through, and the in-grammar `[` starts
    // a list or map literal instead.
    if (valid_symbols[SUBSCRIPT_OPEN] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        if (lexer->lookahead == '[') {
            lexer->advance(lexer, false);
            lexer->result_symbol = SUBSCRIPT_OPEN;
            return true;
        }
    }

    // SPECIFICATION.md §5.5 — the `(` of an argument list must be on the
    // line of the callee. At a newline we fall through, and the in-grammar
    // `(` starts a new statement instead.
    if (valid_symbols[CALL_OPEN] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        if (lexer->lookahead == '(') {
            lexer->advance(lexer, false);
            lexer->result_symbol = CALL_OPEN;
            return true;
        }
    }

    // SPECIFICATION.md §5.5 — a zero-width marker before a command call's
    // first argument: matches only when a string or number starts on the
    // current line, so `a.b` followed by `'x'` on the next line stays two
    // statements.
    if (valid_symbols[COMMAND_LITERAL_START] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        int32_t c = lexer->lookahead;
        if (c == '\'' || c == '"' || (c >= '0' && c <= '9')) {
            lexer->mark_end(lexer);
            lexer->result_symbol = COMMAND_LITERAL_START;
            return true;
        }
    }

    // The same for a closure after a bare identifier receiver, so the
    // closure may lead a longer argument expression.
    if (valid_symbols[COMMAND_CLOSURE_START] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        if (lexer->lookahead == '{') {
            lexer->mark_end(lexer);
            lexer->result_symbol = COMMAND_CLOSURE_START;
            return true;
        }
    }

    if (valid_symbols[COMMAND_NAMED_KEY] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        // An identifier is either the key or, once consumed, not something
        // the code below could start (a comment or slashy string), so stop
        // here. Falling through after the scan advanced past `b` in
        // `foo a, b` and a newline made the comment branch emit `b` as a
        // line_comment, since mark_end had already been called.
        if (is_ident_start(lexer->lookahead)) {
            return scan_command_named_key(lexer);
        }
    }

    // SPECIFICATION.md §4 — `record` starts a record declaration only when
    // a name and then `(` or `<` follow on the same line. Otherwise it is
    // left to the grammar, which lexes it as an identifier.
    if (valid_symbols[RECORD_KEYWORD] && !valid_symbols[ERROR_SENTINEL]) {
        while (is_whitespace(lexer->lookahead)) {
            lexer->advance(lexer, true);
        }
        if (lexer->lookahead == 'r') {
            for (const char *p = "record"; *p; p++) {
                if (lexer->lookahead != *p) {
                    return false;
                }
                advance(lexer);
            }
            if (is_ident_part(lexer->lookahead)) {
                return false;
            }
            lexer->mark_end(lexer);
            if (lexer->lookahead != ' ' && lexer->lookahead != '\t') {
                return false;
            }
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            }
            if (!is_ident_start(lexer->lookahead)) {
                return false;
            }
            while (is_ident_part(lexer->lookahead)) {
                advance(lexer);
            }
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            }
            if (lexer->lookahead == '(' || lexer->lookahead == '<') {
                lexer->result_symbol = RECORD_KEYWORD;
                return true;
            }
            return false;
        }
    }

    // SPECIFICATION.md §6.6 — line / block / groovydoc comments
    // and §6.2 slashy strings. Both start with `/`; combining their
    // dispatch keeps the in-grammar lexer from partially consuming
    // the leading `/`. Leading whitespace is skipped via
    // advance(true) so the scanner gets a chance to fire after the
    // in-grammar `\s` SKIP rule — without this, tree-sitter's lexer
    // eats `\n`, does NOT re-invoke the external scanner, then
    // fails on the following `/` and consumes it through error
    // recovery.
    bool wants_comment = valid_symbols[LINE_COMMENT]
        || valid_symbols[BLOCK_COMMENT]
        || valid_symbols[GROOVYDOC_COMMENT];
    bool wants_slashy = valid_symbols[SLASHY_STRING_START];
    if (!wants_comment && !wants_slashy) {
        return false;
    }

    while (is_whitespace(lexer->lookahead)) {
        lexer->advance(lexer, true);
    }
    if (lexer->lookahead != '/') {
        return false;
    }

    advance(lexer);
    // Dispatch on the char after `/`:
    //   `/`  → line comment
    //   `*`  → block / groovydoc comment
    //   `=`  → augmented assign, in-grammar handles it
    //   else → slashy string (when valid_symbols allow)
    int32_t next = lexer->lookahead;
    if (next == '/') {
        if (!valid_symbols[LINE_COMMENT]) {
            return false;
        }
        advance(lexer);
        // EOF-bounded loop: lexer->eof() guarantees termination on
        // unterminated input. Do not tighten without preserving the
        // EOF guard.
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            advance(lexer);
        }
        lexer->result_symbol = LINE_COMMENT;
        return true;
    }
    if (next == '*') {
        if (!wants_comment) {
            return false;
        }
        advance(lexer);
        bool is_groovydoc = false;
        if (lexer->lookahead == '*') {
            advance(lexer);
            is_groovydoc = true;
            if (lexer->lookahead == '/') {
                advance(lexer);
                if (!valid_symbols[GROOVYDOC_COMMENT]) {
                    return false;
                }
                lexer->result_symbol = GROOVYDOC_COMMENT;
                return true;
            }
        }
        // EOF-bounded loop: a `/* …` without a closer terminates
        // here via lexer->eof(), not by going past end-of-input.
        while (!lexer->eof(lexer)) {
            if (lexer->lookahead == '*') {
                advance(lexer);
                if (lexer->lookahead == '/') {
                    advance(lexer);
                    if (is_groovydoc) {
                        if (!valid_symbols[GROOVYDOC_COMMENT]) {
                            return false;
                        }
                        lexer->result_symbol = GROOVYDOC_COMMENT;
                    } else {
                        if (!valid_symbols[BLOCK_COMMENT]) {
                            return false;
                        }
                        lexer->result_symbol = BLOCK_COMMENT;
                    }
                    return true;
                }
            } else {
                advance(lexer);
            }
        }
        return false;
    }
    if (next == '=') {
        // `/=` is the augmented-assign operator. The in-grammar
        // lexer owns it. Returning false here causes tree-sitter
        // to rewind the cursor to before the leading `/`, so the
        // in-grammar lexer can match `/=` cleanly.
        return false;
    }
    // Slashy string opening `/` — we've consumed it; the body and
    // closing `/` are matched by the grammar's `slashy_string` rule
    // via regex tokens, so `$identifier` and `${expr}` segments can
    // be exposed as structured children. SPECIFICATION.md §5.4 /
    // §6.2 require a context-sensitive emit. Three pragmatic guards
    // keep `a / b` and similar division contexts from being
    // mis-tokenised as the opening of a slashy:
    //
    //   (1) Reject an empty body (the next char after the opening
    //       `/` is a closing `/`) — `//` is already a line comment
    //       and `/=` is already trapped above, so an empty slashy
    //       is excluded.
    //   (2) Reject a body that starts with whitespace — `a / b`
    //       has a space immediately after the operator `/`, but
    //       every legal slashy regex starts with a non-whitespace
    //       char.
    //   (3) Look ahead through the body to confirm there is a
    //       closing `/` on the same line. Without this, `a / b\nc`
    //       at end of file would emit a slashy opener and leave
    //       the body unterminated.
    if (!wants_slashy) {
        return false;
    }
    if (lexer->lookahead == '/' || is_whitespace(lexer->lookahead)) {
        return false;
    }
    // Look ahead (without consuming) to confirm a closing `/` exists
    // before a newline or EOF. `lexer->mark_end` records the position
    // where the slashy opener token ends; advancing past it during
    // look-ahead does not extend the token range as long as we do
    // not call `mark_end` again.
    lexer->mark_end(lexer);
    bool found_close = false;
    while (!lexer->eof(lexer)) {
        if (lexer->lookahead == '\\') {
            advance(lexer);
            if (!lexer->eof(lexer)) {
                advance(lexer);
            }
            continue;
        }
        if (lexer->lookahead == '/') {
            found_close = true;
            break;
        }
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            break;
        }
        advance(lexer);
    }
    if (!found_close) {
        return false;
    }
    lexer->result_symbol = SLASHY_STRING_START;
    return true;
}
