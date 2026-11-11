// Hand-written external scanner for tree-sitter-groovy.
//
// Handles §6.2 slashy-string-start, §6.6 line/block/groovydoc
// comments, and the §6.7 contextual `record` keyword. The first two
// start with `/`; combining their dispatch keeps the in-grammar lexer
// from partially consuming the leading `/`. The third is a look-ahead
// decision the grammar cannot express: `record` is a keyword only
// when a record header follows it.
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
    COMMAND_VALUE_START,
    LINE_BREAK,
    COMMAND_NAME_ARGUMENT,
    VAR_KEYWORD,
    PIPELINE_KEYWORD,
    TRAIT_KEYWORD,
    TYPE_ARGUMENTS_OPEN,
    LESS_THAN,
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
//
// The same identifier is `_command_name_argument`, the one argument of a
// command call on a dotted receiver (`a.b key`), when the statement ends
// after it: a line break, `;`, `}`, a comment or the end of input.
static bool finish_command_named_key(TSLexer *lexer, const bool *valid_symbols,
                                     const char *word, unsigned len);
static unsigned read_word(TSLexer *lexer, char *word, unsigned size);

static bool scan_command_named_key(TSLexer *lexer, const bool *valid_symbols) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, true);
    }
    if (!is_ident_start(lexer->lookahead)) {
        return false;
    }
    // Keep the first few chars to reject `case:` / `default:`, which
    // are switch labels, not keys, and the operators `as`, `in` and
    // `instanceof`, which are not arguments.
    char word[12];
    unsigned len = read_word(lexer, word, sizeof(word));
    return finish_command_named_key(lexer, valid_symbols, word, len);
}

// Reads an identifier into `word` (its first `size` chars) and returns its
// length.
static unsigned read_word(TSLexer *lexer, char *word, unsigned size) {
    unsigned len = 0;
    while (is_ident_part(lexer->lookahead)) {
        if (len < size) {
            word[len] = (char)lexer->lookahead;
        }
        len++;
        lexer->advance(lexer, false);
    }
    return len;
}

// The rest of `scan_command_named_key`, once the identifier is read.
static bool finish_command_named_key(TSLexer *lexer, const bool *valid_symbols,
                                     const char *word, unsigned len) {
    if ((len == 4 && memcmp(word, "case", 4) == 0)
        || (len == 7 && memcmp(word, "default", 7) == 0)) {
        return false;
    }
    lexer->mark_end(lexer);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        lexer->advance(lexer, false);
    }
    int32_t n = lexer->lookahead;
    if (n == ':' && valid_symbols[COMMAND_NAMED_KEY]) {
        lexer->advance(lexer, false);
        if (lexer->lookahead == ':') {
            return false;
        }
        lexer->result_symbol = COMMAND_NAMED_KEY;
        return true;
    }
    if (!valid_symbols[COMMAND_NAME_ARGUMENT]
        || (len == 2 && (memcmp(word, "as", 2) == 0 || memcmp(word, "in", 2) == 0))
        || (len == 10 && memcmp(word, "instanceof", 10) == 0)) {
        return false;
    }
    bool ends = lexer->eof(lexer) || n == '\n' || n == '\r' || n == ';' || n == '}';
    if (n == '/') {
        lexer->advance(lexer, false);
        ends = lexer->lookahead == '/' || lexer->lookahead == '*';
    }
    if (!ends) {
        return false;
    }
    lexer->result_symbol = COMMAND_NAME_ARGUMENT;
    return true;
}

static inline void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
}

// A byte order mark counts too: a file may start with two of them.
static inline bool is_whitespace(int32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xFEFF;
}

static inline bool is_inline_space(int32_t c) {
    return c == ' ' || c == '\t';
}

// Mirrors the grammar's `identifier` token, /[A-Za-z_$][A-Za-z0-9_$]*/.
static inline bool is_identifier_start(int32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || c == '_' || c == '$';
}

static inline bool is_identifier_char(int32_t c) {
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

// Skips the trivia Groovy's lexer hides inside a record header:
// spaces, tabs and block comments, plus line breaks and line comments
// when `newlines` is set. Returns false on a `/` that opens neither
// comment kind (a slashy string or division cannot be part of a
// header) and on an unterminated block comment.
static bool skip_header_trivia(TSLexer *lexer, bool newlines) {
    for (;;) {
        while (is_inline_space(lexer->lookahead)
            || (newlines
                && (lexer->lookahead == '\n' || lexer->lookahead == '\r'))) {
            advance(lexer);
        }
        if (lexer->lookahead != '/') {
            return true;
        }
        advance(lexer);
        if (lexer->lookahead == '*') {
            advance(lexer);
            // EOF-bounded: an unterminated comment ends the scan here.
            for (;;) {
                if (lexer->eof(lexer)) {
                    return false;
                }
                if (lexer->lookahead == '*') {
                    advance(lexer);
                    if (lexer->lookahead == '/') {
                        advance(lexer);
                        break;
                    }
                } else {
                    advance(lexer);
                }
            }
        } else if (newlines && lexer->lookahead == '/') {
            while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
                advance(lexer);
            }
        } else {
            return false;
        }
    }
}

// SPECIFICATION.md §6.7 — `record` is a keyword only at the start of
// a record header: `record`, a name on the same line, then `(` with
// Groovy's lexer trivia allowed in between, or `<` straight after the
// name (type parameters; the grammar's `type_parameters` opener is
// `token.immediate` too). Everywhere else the word
// is an identifier (`record.key()`, `private Record record`,
// `public void record()`), as Apache's `identifier` rule allows. The
// grammar cannot make that choice: with `word: $.identifier` the
// keyword wins wherever it has an action, and once
// record_declaration is a class member that is every member start.
//
// The scanner has already consumed `record` when this is called.
// `mark_end` fixes the token there; the look-ahead that follows is
// read but not kept. Between `record` and the name only spaces, tabs
// and block comments may appear: `classDeclaration` in
// GroovyParser.g4 has no `nls` there, so `record` at the end of a line
// is never a keyword, which keeps `private Record record` followed by
// a constructor on the next line a field and a constructor. Before
// `(` the rule has `nls`, so line breaks and line comments are
// skipped there too. A command-style call `record Point(1, 2)`
// matches the pattern and misfires; §6.7 lists the residuals.
static bool scan_record_header(TSLexer *lexer) {
    if (is_identifier_char(lexer->lookahead)) {
        return false;
    }
    lexer->mark_end(lexer);
    if (!is_inline_space(lexer->lookahead) && lexer->lookahead != '/') {
        return false;
    }
    if (!skip_header_trivia(lexer, false)) {
        return false;
    }
    if (!is_identifier_start(lexer->lookahead)) {
        return false;
    }
    // EOF-bounded: lookahead is 0 at end of input, which fails the
    // predicate.
    while (is_identifier_char(lexer->lookahead)) {
        advance(lexer);
    }
    if (lexer->lookahead == '<') {
        return true;
    }
    if (!skip_header_trivia(lexer, true)) {
        return false;
    }
    return lexer->lookahead == '(';
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

    // SPECIFICATION.md §5.14 — the `<` of a generic type used as a value,
    // right after its name: `Mock(Supplier<Long>)`, `List<X>.class`. Only
    // when a balanced `<…>` of names, dots, commas, `?`, `[]` and nested
    // `<…>` follows on the line, and after its `>` comes `)`, `,`, `]`,
    // `::`, or `.` and a name (also on the next line). `i<n`, `a<b && c>d`
    // and `f(a<b, c>d)` don't match, so their `<` stays an operator.
    // After a space the `<` must also be followed, past its `>`, by a name
    // and `=`, `;`, `(` or the line's end, or by a `(`: a declaration,
    // `Map <K, V> m = [:]`, or `new TreeSet <String>()`.
    bool spaced = false;
    if (valid_symbols[TYPE_ARGUMENTS_OPEN] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
            spaced = true;
        }
    }
    if (valid_symbols[TYPE_ARGUMENTS_OPEN] && !valid_symbols[ERROR_SENTINEL]
        && lexer->lookahead == '<') {
        advance(lexer);
        lexer->mark_end(lexer);
        // A comparison's `<` when no balanced, type-shaped `<…>` follows.
        // Not `<=`, `<<`, `<..` or `<=>`, which the grammar lexes.
        bool less_than = valid_symbols[LESS_THAN] && lexer->lookahead != '='
            && lexer->lookahead != '<' && lexer->lookahead != '.';
        unsigned depth = 1;
        bool after_name = false;
        // No space just inside the outer `<` and `>`, as in `<V>` but not in
        // the comparisons `x < y, z > w`.
        bool tight = lexer->lookahead != ' ' && lexer->lookahead != '\t';
        bool prev_space = false;
        for (unsigned steps = 0; depth > 0; steps++) {
            int32_t c = lexer->lookahead;
            if (steps > 400 || lexer->eof(lexer)) {
                return false;
            }
            if (c == '\n' || c == '\r') {
                goto not_type;
            }
            if (c == ' ' || c == '\t') {
                advance(lexer);
                prev_space = true;
                continue;
            } else if (is_ident_start(c)) {
                while (is_ident_part(lexer->lookahead)) {
                    advance(lexer);
                }
                after_name = true;
                prev_space = false;
                continue;
            } else if (c == '.') {
                advance(lexer);
                if (!after_name || !is_ident_start(lexer->lookahead)) {
                    goto not_type;
                }
            } else if (c == ',' || c == '?' || c == '@') {
                advance(lexer);
            } else if (c == '[') {
                advance(lexer);
                if (lexer->lookahead != ']') {
                    goto not_type;
                }
                advance(lexer);
            } else if (c == '<') {
                advance(lexer);
                depth++;
            } else if (c == '>') {
                advance(lexer);
                depth--;
                if (depth == 0 && prev_space) {
                    tight = false;
                }
            } else {
                goto not_type;
            }
            after_name = false;
            prev_space = false;
        }
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            advance(lexer);
        }
        int32_t c = lexer->lookahead;
        bool ok = c == ')' || c == ',' || c == ']';
        if (spaced) {
            // A cast `(List <T>) x`, or a call `new TreeSet <String>()`.
            ok = c == '(' || (c == ')' && tight);
            if (is_ident_start(c)) {
                while (is_ident_part(lexer->lookahead)) {
                    advance(lexer);
                }
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    advance(lexer);
                }
                c = lexer->lookahead;
                // `Closure <V> value)`, `Map <K, V> m, …`: a parameter.
                ok = c == ';' || c == '(' || ((c == ')' || c == ',') && tight)
                    || c == '\n' || c == '\r' || lexer->eof(lexer);
                if (c == '=') {
                    advance(lexer);
                    ok = lexer->lookahead != '=' && lexer->lookahead != '~';
                }
            }
        } else if (c == ':') {
            advance(lexer);
            ok = lexer->lookahead == ':';
        } else if (c == '.' || c == '\n' || c == '\r') {
            while (is_whitespace(lexer->lookahead)) {
                advance(lexer);
            }
            if (lexer->lookahead == '.') {
                advance(lexer);
                ok = is_ident_start(lexer->lookahead);
            }
        }
        if (!ok) {
            return false;
        }
        lexer->result_symbol = TYPE_ARGUMENTS_OPEN;
        return true;
    not_type:
        if (!less_than) {
            return false;
        }
        lexer->result_symbol = LESS_THAN;
        return true;
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
            // ... unless a dot follows, as in a variable named `in`: `(T) in.x`.
            if (((len == 2 && (memcmp(word, "in", 2) == 0 || memcmp(word, "as", 2) == 0))
                 || (len == 10 && memcmp(word, "instanceof", 10) == 0))
                && lexer->lookahead != '.') {
                return false;
            }
            lexer->result_symbol = CAST_VALUE_START;
            return true;
        }
        // At a line break the `_line_break` branch below decides.
        if (c != '\n' && c != '\r') {
            return false;
        }
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
        // A backslash at the end of the line continues it: `include \`.
        for (;;) {
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                lexer->advance(lexer, true);
            } else if (lexer->lookahead == '\\') {
                lexer->advance(lexer, true);
                if (lexer->lookahead == '\r') {
                    lexer->advance(lexer, true);
                }
                if (lexer->lookahead != '\n') {
                    return false;
                }
                lexer->advance(lexer, true);
            } else {
                break;
            }
        }
        int32_t c = lexer->lookahead;
        if (c == '\'' || c == '"' || (c >= '0' && c <= '9') || c == '~') {
            lexer->mark_end(lexer);
            lexer->result_symbol = COMMAND_LITERAL_START;
            return true;
        }
        // A number with no digits before the point: `x .42` calls `x(.42)`.
        if (c == '.') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
                lexer->result_symbol = COMMAND_LITERAL_START;
                return true;
            }
            return false;
        }
        // `!x`, not `!=`, `!in` or `!instanceof`, which continue the receiver.
        if (c == '!') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '=') {
                return false;
            }
            if (lexer->lookahead == 'i') {
                char word[12];
                unsigned len = read_word(lexer, word, sizeof(word));
                if ((len == 2 && memcmp(word, "in", 2) == 0)
                    || (len == 10 && memcmp(word, "instanceof", 10) == 0)) {
                    return false;
                }
            }
            lexer->result_symbol = COMMAND_LITERAL_START;
            return true;
        }
        // `new`, `null`, `true` and `false` start an argument too, since none
        // can be a declaration's name: `respond new Foo(), model: m`,
        // `assertNull null.x()`. So does a name followed by `.` or `?.` and
        // another name, since a declared name has no dot: `println it.name`,
        // `reply message.reverse()`. Any other name is left to the
        // named-key branch.
        if (is_ident_start(c)) {
            lexer->mark_end(lexer);
            char word[12];
            unsigned len = read_word(lexer, word, sizeof(word));
            if ((len == 3 && memcmp(word, "new", 3) == 0)
                || (len == 4 && memcmp(word, "null", 4) == 0)
                || (len == 4 && memcmp(word, "true", 4) == 0)
                || (len == 5 && memcmp(word, "false", 5) == 0)) {
                // `foo new: 1` names an argument `new`.
                if (lexer->lookahead == ':' && valid_symbols[COMMAND_NAMED_KEY]) {
                    lexer->mark_end(lexer);
                    advance(lexer);
                    if (lexer->lookahead != ':') {
                        lexer->result_symbol = COMMAND_NAMED_KEY;
                        return true;
                    }
                    return false;
                }
                lexer->result_symbol = COMMAND_LITERAL_START;
                return true;
            }
            if (lexer->lookahead == '.' || lexer->lookahead == '?') {
                bool safe = lexer->lookahead == '?';
                advance(lexer);
                if (!safe || lexer->lookahead == '.') {
                    if (safe) {
                        advance(lexer);
                    }
                    if (is_ident_start(lexer->lookahead)) {
                        lexer->result_symbol = COMMAND_LITERAL_START;
                        return true;
                    }
                }
                return false;
            }
            if (valid_symbols[COMMAND_NAMED_KEY] || valid_symbols[COMMAND_NAME_ARGUMENT]) {
                return finish_command_named_key(lexer, valid_symbols, word, len);
            }
            return false;
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

    if ((valid_symbols[COMMAND_NAMED_KEY] || valid_symbols[COMMAND_NAME_ARGUMENT])
        && !valid_symbols[ERROR_SENTINEL]) {
        // Where no statement may end (after a comma between arguments), the
        // key may start on the next line: `sh returnStdout: true,` then
        // `script: 'x'`. Elsewhere a key on the next line is a label.
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t'
            || (!valid_symbols[LINE_BREAK]
                && (lexer->lookahead == '\n' || lexer->lookahead == '\r'))) {
            lexer->advance(lexer, true);
        }
        // An identifier is either the key or, once consumed, not something
        // the code below could start (a comment or slashy string), so stop
        // here. Falling through after the scan advanced past `b` in
        // `foo a, b` and a newline made the comment branch emit `b` as a
        // line_comment, since mark_end had already been called.
        if (is_ident_start(lexer->lookahead)) {
            return scan_command_named_key(lexer, valid_symbols);
        }
    }

    // SPECIFICATION.md §5.5 — zero-width, before the first argument of a
    // command call that is a declaration's or assignment's value: a name,
    // string or number on the receiver's line. A name that continues the
    // expression (`as`, `in`, `instanceof`) or the statement (`else`) is not
    // an argument.
    if (valid_symbols[COMMAND_VALUE_START] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        int32_t c = lexer->lookahead;
        if (c == '\'' || c == '"' || (c >= '0' && c <= '9')) {
            lexer->mark_end(lexer);
            lexer->result_symbol = COMMAND_VALUE_START;
            return true;
        }
        if (is_ident_start(c)) {
            lexer->mark_end(lexer);
            char word[12];
            unsigned len = 0;
            while (is_ident_part(lexer->lookahead)) {
                if (len < sizeof(word)) {
                    word[len] = (char)lexer->lookahead;
                }
                len++;
                advance(lexer);
            }
            if ((len == 2 && (memcmp(word, "as", 2) == 0 || memcmp(word, "in", 2) == 0))
                || (len == 4 && memcmp(word, "else", 4) == 0)
                || (len == 10 && memcmp(word, "instanceof", 10) == 0)) {
                return false;
            }
            lexer->result_symbol = COMMAND_VALUE_START;
            return true;
        }
    }

    // SPECIFICATION.md §5.10 — zero-width, before a statement that starts on
    // a new line with a name or `(`, where the line before may end one. The
    // names that continue a statement or expression across a line break are
    // left alone: `else`, `catch`, `finally`, `while` (of `do`), `case`,
    // `default`, `as`, `in`, `instanceof`, `throws`, `extends`, `implements`,
    // `permits`.
    if (valid_symbols[LINE_BREAK] && !valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            lexer->advance(lexer, true);
        }
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            while (is_whitespace(lexer->lookahead)) {
                lexer->advance(lexer, true);
            }
            if (lexer->lookahead == '(') {
                lexer->mark_end(lexer);
                lexer->result_symbol = LINE_BREAK;
                return true;
            }
            if (is_ident_start(lexer->lookahead)) {
                lexer->mark_end(lexer);
                char word[12];
                unsigned len = 0;
                while (is_ident_part(lexer->lookahead)) {
                    if (len < sizeof(word)) {
                        word[len] = (char)lexer->lookahead;
                    }
                    len++;
                    advance(lexer);
                }
                static const char *const continuing[] = {
                    "else", "catch", "finally", "while", "case", "default",
                    "as", "in", "instanceof", "throws", "extends",
                    "implements", "permits",
                };
                // `in.property`: a name before a dot starts a statement.
                bool dotted = lexer->lookahead == '.';
                for (unsigned i = 0; i < sizeof(continuing) / sizeof(continuing[0]) && !dotted; i++) {
                    if (strlen(continuing[i]) == len && memcmp(word, continuing[i], len) == 0) {
                        return false;
                    }
                }
                lexer->result_symbol = LINE_BREAK;
                return true;
            }
        }
    }

    // SPECIFICATION.md §4 — `var` and `trait` are keywords only before a name
    // on the same line (not `in`, `as` or `instanceof`, which make them a
    // value), and `pipeline` only before a `{` on its line. Otherwise the
    // grammar lexes the word as an identifier: `var.set(1)`, `{ var -> }`,
    // `def var = 1`, `trait.parentFile`, `Pipeline pipeline = p`.
    if ((valid_symbols[VAR_KEYWORD] || valid_symbols[PIPELINE_KEYWORD]
         || valid_symbols[TRAIT_KEYWORD]) && !valid_symbols[ERROR_SENTINEL]) {
        while (is_whitespace(lexer->lookahead)) {
            lexer->advance(lexer, true);
        }
        int32_t c = lexer->lookahead;
        if (c == 'v' || c == 'p' || c == 't') {
            char word[12];
            unsigned len = 0;
            while (is_ident_part(lexer->lookahead)) {
                if (len < sizeof(word)) {
                    word[len] = (char)lexer->lookahead;
                }
                len++;
                advance(lexer);
            }
            enum TokenType sym;
            if (len == 3 && memcmp(word, "var", 3) == 0 && valid_symbols[VAR_KEYWORD]) {
                sym = VAR_KEYWORD;
            } else if (len == 8 && memcmp(word, "pipeline", 8) == 0
                && valid_symbols[PIPELINE_KEYWORD]) {
                sym = PIPELINE_KEYWORD;
            } else if (len == 5 && memcmp(word, "trait", 5) == 0
                && valid_symbols[TRAIT_KEYWORD]) {
                sym = TRAIT_KEYWORD;
            } else {
                return false;
            }
            lexer->mark_end(lexer);
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            }
            if (sym == PIPELINE_KEYWORD) {
                if (lexer->lookahead != '{') {
                    return false;
                }
            } else {
                if (!is_ident_start(lexer->lookahead)) {
                    return false;
                }
                len = 0;
                while (is_ident_part(lexer->lookahead)) {
                    if (len < sizeof(word)) {
                        word[len] = (char)lexer->lookahead;
                    }
                    len++;
                    advance(lexer);
                }
                if ((len == 2 && (memcmp(word, "in", 2) == 0 || memcmp(word, "as", 2) == 0))
                    || (len == 10 && memcmp(word, "instanceof", 10) == 0)) {
                    return false;
                }
            }
            lexer->result_symbol = sym;
            return true;
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
    // recovery. §6.7 `record` shares the whitespace skip and is
    // dispatched on `r` before the `/` branches.
    bool wants_comment = valid_symbols[LINE_COMMENT]
        || valid_symbols[BLOCK_COMMENT]
        || valid_symbols[GROOVYDOC_COMMENT];
    bool wants_slashy = valid_symbols[SLASHY_STRING_START];
    bool wants_record = valid_symbols[RECORD_KEYWORD] && !valid_symbols[ERROR_SENTINEL];
    if (!wants_comment && !wants_slashy && !wants_record) {
        return false;
    }

    while (is_whitespace(lexer->lookahead)) {
        lexer->advance(lexer, true);
    }
    if (lexer->lookahead == 'r' && wants_record) {
        // An `r` can never open a comment or a slashy string, so
        // every path out of this branch returns. On `false` the
        // runtime rewinds to the token start and the in-grammar
        // lexer reads the word as an identifier.
        static const char keyword[] = "record";
        for (const char *c = keyword; *c != '\0'; c++) {
            if (lexer->lookahead != *c) {
                return false;
            }
            advance(lexer);
        }
        if (!scan_record_header(lexer)) {
            return false;
        }
        lexer->result_symbol = RECORD_KEYWORD;
        return true;
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
    // Where an expression must start (after `=~`, `~`, `(`, `,` or an
    // operator) and no statement or command argument could start instead,
    // a `/` can't be division: the body may then start with whitespace
    // (`=~ / X /`, `split(/ /)`) and run over several lines (`def s = /`).
    bool lenient = !valid_symbols[LINE_BREAK] && !valid_symbols[COMMAND_LITERAL_START]
        && !valid_symbols[COMMAND_NAMED_KEY] && !valid_symbols[COMMAND_VALUE_START]
        && !valid_symbols[ERROR_SENTINEL];
    if (lexer->lookahead == '/' || (is_whitespace(lexer->lookahead) && !lenient)) {
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
        // `\/` is an escaped slash; any other backslash is one character.
        if (lexer->lookahead == '\\') {
            advance(lexer);
            if (lexer->lookahead == '/') {
                advance(lexer);
            }
            continue;
        }
        if (lexer->lookahead == '/') {
            found_close = true;
            break;
        }
        if ((lexer->lookahead == '\n' || lexer->lookahead == '\r') && !lenient) {
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
