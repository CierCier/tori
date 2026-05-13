#include <tori/kernel/demangle.hpp>
#include <stdint.h>
#include <stddef.h>

namespace {

struct Ctx {
    const char* in;
    char* out;
    char* out_end;
    int depth;

    char* mark; // output position at start of current substitutable type

    // substitution table entries store the demangled text
    struct Sub {
        char data[128];
        size_t len;
    };
    Sub subs[32];
    int num_subs;

    bool has_std; // whether 'std' substitution is in the table
    bool has_alloc, has_string, has_basic_string, has_istream, has_ostream, has_iostream;
};

// ---- helpers ----

static char peek(Ctx& c) { return *c.in; }
static char adv(Ctx& c) { return *c.in++; }
static bool match(Ctx& c, char ch) {
    if (*c.in == ch) { c.in++; return true; }
    return false;
}

static bool eof(Ctx& c) { return *c.in == '\0'; }

static bool put(Ctx& c, char ch) {
    if (c.out >= c.out_end) return false;
    *c.out++ = ch;
    return true;
}

static bool puts(Ctx& c, const char* s) {
    for (; *s; ++s) if (!put(c, *s)) return false;
    return true;
}

static bool putn(Ctx& c, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) if (!put(c, s[i])) return false;
    return true;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// read decimal number from input
static bool read_number(Ctx& c, unsigned& val) {
    if (!is_digit(peek(c))) return false;
    val = 0;
    while (is_digit(peek(c))) {
        unsigned d = (unsigned)(adv(c) - '0');
        if (val > (unsigned)-1 / 10) return false;
        val = val * 10 + d;
    }
    return true;
}

// read until a given character is found, return the span in [start, end)
// does NOT consume the terminator
static bool read_until(Ctx& c, char term, const char*& start, const char*& end) {
    start = c.in;
    const char* p = c.in;
    while (*p && *p != term) p++;
    if (!*p) return false;
    end = p;
    return true;
}

// consume optional discriminator: _<number>
static void skip_discriminator(Ctx& c) {
    if (peek(c) != '_') return;
    const char* saved = c.in;
    c.in++; // skip '_'
    unsigned d;
    if (!read_number(c, d)) { c.in = saved; return; }
    // discriminator consumed
}

// ---- substitution table ----

static void add_sub(Ctx& c, const char* start, size_t len) {
    if (c.num_subs >= 32) return;
    size_t copy = len;
    if (copy > 127) copy = 127;
    auto& e = c.subs[c.num_subs];
    for (size_t i = 0; i < copy; ++i) e.data[i] = start[i];
    e.data[copy] = '\0';
    e.len = copy;
    c.num_subs++;
}

// check if type should be added to substitution table
static bool is_substitutable_type(char first) {
    switch (first) {
    case 'v': case 'w': case 'b': case 'c': case 'a': case 'h':
    case 's': case 't': case 'i': case 'j': case 'l': case 'm':
    case 'x': case 'y': case 'n': case 'o': case 'f': case 'd': case 'e':
    case 'g':
        return false;
    default:
        return true;
    }
}

// ---- forward declarations ----

static bool parse_encoding(Ctx& c);
static bool parse_nested_name(Ctx& c);
static bool parse_prefix(Ctx& c);
static bool parse_unqualified_name(Ctx& c);
static bool parse_source_name(Ctx& c);
static bool parse_type(Ctx& c);
static bool parse_builtin_type(Ctx& c);
static bool parse_cv_qualifiers(Ctx& c);
static bool parse_bare_function_type(Ctx& c);
static bool parse_template_args(Ctx& c);
static bool parse_template_arg(Ctx& c);
static bool parse_substitution(Ctx& c);
static bool parse_special_name(Ctx& c);

// ---- forward declarations for things that need mark support ----

// wraps parse_type with substitution tracking
static bool parse_type_sub(Ctx& c, bool add_to_subs);

// ---- grammar productions ----

// <source-name> ::= <number> <identifier>
static bool parse_source_name(Ctx& c) {
    unsigned len;
    if (!read_number(c, len)) return false;
    // check for _GLOBAL__N_1 => {anonymous}
    if (len == 12) {
        if (c.in[0] == '_' && c.in[1] == 'G' && c.in[2] == 'L' && c.in[3] == 'O' &&
            c.in[4] == 'B' && c.in[5] == 'A' && c.in[6] == 'L' && c.in[7] == '_' &&
            c.in[8] == '_' && c.in[9] == 'N' && c.in[10] == '_' && c.in[11] == '1') {
            c.in += 12;
            return puts(c, "{anonymous}");
        }
    }
    // check for __1 (libc++ inline namespace)
    if (len == 3 && c.in[0] == '_' && c.in[1] == '_' && c.in[2] == '1') {
        c.in += 3;
        return true; // skip this component silently
    }
    if (len == 0) return false;
    for (unsigned i = 0; i < len; ++i) {
        if (eof(c)) return false;
        if (!put(c, adv(c))) return false;
    }
    return true;
}

// <unqualified-name> ::= <source-name> | <local-source-name> | <operator-name> | <ctor-dtor-name>
static bool parse_unqualified_name(Ctx& c) {
    if (peek(c) == 'L') {
        // <local-source-name> ::= L <source-name> [<discriminator>]
        adv(c); // L
        if (!parse_source_name(c)) return false;
        skip_discriminator(c);
        return true;
    }
    if (peek(c) == 'D') {
        // might be ctor/dtor or operator
        char n = c.in[1];
        if (n == '0' || n == '1' || n == '2' || n == '3' || n == '4' || n == '5') {
            // D0-D5 = constructor/destructor
            adv(c); // D
            char kind = adv(c);
            if (kind == '0') puts(c, "(constructor)");
            else if (kind == '1') puts(c, "(destructor)");
            else if (kind == '2') puts(c, "(constructor)");
            else if (kind == '3') puts(c, "(destructor)");
            else if (kind == '4') puts(c, "(deleting destructor)");
            else if (kind == '5') puts(c, "(constructor)");
            return true;
        }
        if (n == 't' || n == 'T') {
            // Dt / DT = destructor (base/vtable)
            adv(c); adv(c);
            puts(c, "(destructor)");
            return true;
        }
    }
    if (peek(c) >= '0' && peek(c) <= '9') {
        return parse_source_name(c);
    }
    // operator names - prefix 'op' or operator symbols
    // For simplicity, just try source name
    if (peek(c) >= '0' && peek(c) <= '9') {
        return parse_source_name(c);
    }
    return false;
}

// <prefix> ::= <prefix> <unqualified-name>
//           ::= <template-prefix> <template-args>
//           ::= <substitution>
// We handle nested name prefix components (the "path" before the function name)
static bool parse_prefix(Ctx& c) {
    // try source name
    if (peek(c) >= '0' && peek(c) <= '9') {
        char* saved_out = c.out;
        if (!parse_source_name(c)) return false;
        // add to substitution table
        size_t len = (size_t)(c.out - saved_out);
        add_sub(c, saved_out, len);
        // after a prefix component, we expect :: separator
        if (peek(c) == 'E') return true;
        if (peek(c) == 'I') {
            // template args follow
            if (!parse_template_args(c)) return false;
        }
        // more prefix components or end
        return true;
    }
    // local source name within prefix
    if (peek(c) == 'L') {
        // L <source-name>
        adv(c); // L
        char* saved = c.out;
        if (!parse_source_name(c)) return false;
        size_t len = (size_t)(c.out - saved);
        add_sub(c, saved, len);
        skip_discriminator(c);
        return true;
    }
    // substitution (e.g. St for std::)
    if (peek(c) == 'S') {
        char* saved = c.out;
        if (!parse_substitution(c)) return false;
        size_t len = (size_t)(c.out - saved);
        add_sub(c, saved, len);
        // after substitution, might have more prefix or template args
        if (peek(c) == 'I') {
            if (!parse_template_args(c)) return false;
        }
        return true;
    }
    return false;
}

// <nested-name> ::= N [<CV-qualifiers>] [<ref-qualifier>] <prefix> <unqualified-name> E
//                ::= N [<CV-qualifiers>] [<ref-qualifier>] <template-prefix> <template-args> E
static bool parse_nested_name(Ctx& c) {
    if (!match(c, 'N')) return false;

    // optional cv-qualifiers on the entire name (for member functions)
    // We can skip these here; they apply to the 'this' pointer
    while (peek(c) == 'r' || peek(c) == 'V' || peek(c) == 'K') adv(c);

    // optional ref-qualifier (O = &&, R = &)
    if (peek(c) == 'O' || peek(c) == 'R') adv(c);

    // parse prefix components
    bool first = true;
    while (peek(c) != 'E' && !eof(c)) {
        if (!first) {
            if (!puts(c, "::")) return false;
        }
        first = false;

        if (peek(c) == 'S') {
            char n1 = c.in[1];
            if (n1 == 't') {
                // St => std::
                adv(c); adv(c);
                puts(c, "std");
                // add std to substitution table
                add_sub(c, c.out - 3, 3);
            } else if (n1 == 'a') {
                adv(c); adv(c);
                puts(c, "std::allocator");
                add_sub(c, c.out - 15, 15);
            } else if (n1 == 'b') {
                adv(c); adv(c);
                puts(c, "std::basic_string");
                add_sub(c, c.out - 17, 17);
            } else if (n1 == 's') {
                adv(c); adv(c);
                puts(c, "std::string");
                add_sub(c, c.out - 12, 12);
            } else if (n1 == 'i') {
                adv(c); adv(c);
                puts(c, "std::istream");
                add_sub(c, c.out - 13, 13);
            } else if (n1 == 'o') {
                adv(c); adv(c);
                puts(c, "std::ostream");
                add_sub(c, c.out - 13, 13);
            } else if (n1 == 'd') {
                adv(c); adv(c);
                puts(c, "std::iostream");
                add_sub(c, c.out - 14, 14);
            } else if (n1 == '_' || (is_digit(n1))) {
                // S_ or S<digit>_ : back-reference to substitution table
                // But we need the substitution text, which might contain "::"
                // For simplicity, write the text directly
                int idx = 0;
                if (n1 == '_') { adv(c); adv(c); }
                else { adv(c); // skip S
                    unsigned d = (unsigned)(adv(c) - '0');
                    idx = (int)d;
                    if (!match(c, '_')) return false;
                }
                // standard substitutions are indices 0-6, non-standard from 7+
                // non-standard idx = standard_count + seq_id
                // S_ = non-standard seq_id 0
                // S0_ = non-standard seq_id 1, etc.
                int nss = idx; // non-standard seq_id
                int total_idx = 7 + nss; // 7 standard subs
                if (total_idx < c.num_subs) {
                    puts(c, c.subs[total_idx].data);
                } else {
                    puts(c, "<?>");
                }
            } else {
                // unknown substitution, treat as source name
                adv(c);
                if (!parse_source_name(c)) return false;
            }
            // after substitution, might have template args
            if (peek(c) == 'I') {
                if (!parse_template_args(c)) return false;
            }
        } else if (peek(c) >= '0' && peek(c) <= '9') {
            char* saved = c.out;
            if (!parse_source_name(c)) return false;
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
            if (peek(c) == 'I') {
                if (!parse_template_args(c)) return false;
            }
        } else if (peek(c) == 'L') {
            adv(c); // L
            char* saved = c.out;
            if (!parse_source_name(c)) return false;
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
            skip_discriminator(c);
            if (peek(c) == 'I') {
                if (!parse_template_args(c)) return false;
            }
        } else if (peek(c) == 'D') {
            // D0/D1 = constructor/destructor
            if (c.in[1] == '0') { adv(c); adv(c); puts(c, "(constructor)"); }
            else if (c.in[1] == '1') { adv(c); adv(c); puts(c, "(destructor)"); }
            else if (c.in[1] == '2') { adv(c); adv(c); puts(c, "(constructor)"); }
            else if (c.in[1] == '3') { adv(c); adv(c); puts(c, "(destructor)"); }
            else if (c.in[1] == '4') { adv(c); adv(c); puts(c, "(deleting destructor)"); }
            else if (c.in[1] == '5') { adv(c); adv(c); puts(c, "(constructor)"); }
            else if (c.in[1] == 't' || c.in[1] == 'T') { adv(c); adv(c); puts(c, "(destructor)"); }
            else { return false; }
        } else {
            return false;
        }
    }

    if (!match(c, 'E')) return false;
    return true;
}

// <substitution> ::= St | Sa | Sb | Ss | Si | So | Sd | S_ | S<seq-id>_
static bool parse_substitution(Ctx& c) {
    if (!match(c, 'S')) return false;

    if (peek(c) == 't') { adv(c); puts(c, "std"); return true; }
    if (peek(c) == 'a') { adv(c); puts(c, "std::allocator"); return true; }
    if (peek(c) == 'b') { adv(c); puts(c, "std::basic_string"); return true; }
    if (peek(c) == 's') { adv(c); puts(c, "std::basic_string<char, std::char_traits<char>, std::allocator<char>>"); return true; }
    if (peek(c) == 'i') { adv(c); puts(c, "std::basic_istream<char, std::char_traits<char>>"); return true; }
    if (peek(c) == 'o') { adv(c); puts(c, "std::basic_ostream<char, std::char_traits<char>>"); return true; }
    if (peek(c) == 'd') { adv(c); puts(c, "std::basic_iostream<char, std::char_traits<char>>"); return true; }

    // S_ or S<digit>_
    if (peek(c) == '_') {
        adv(c);
        // reference first non-standard substitution (index 7 overall)
        int idx = 7; // first non-standard = seq_id 0
        if (idx < c.num_subs) {
            puts(c, c.subs[idx].data);
        } else {
            puts(c, "<?>");
        }
        return true;
    }
    if (is_digit(peek(c))) {
        unsigned seq = (unsigned)(adv(c) - '0');
        if (!match(c, '_')) return false;
        int idx = 7 + (int)seq;
        if (idx < c.num_subs) {
            puts(c, c.subs[idx].data);
        } else {
            puts(c, "<?>");
        }
        return true;
    }
    return false;
}

// <builtin-type> ::= v | b | c | a | h | s | t | i | j | l | m | x | y | n | o | f | d | e | ...
static bool parse_builtin_type(Ctx& c) {
    switch (peek(c)) {
    case 'v': adv(c); puts(c, "void"); return true;
    case 'w': adv(c); puts(c, "wchar_t"); return true;
    case 'b': adv(c); puts(c, "bool"); return true;
    case 'c': adv(c); puts(c, "char"); return true;
    case 'a': adv(c); puts(c, "signed char"); return true;
    case 'h': adv(c); puts(c, "unsigned char"); return true;
    case 's': adv(c); puts(c, "short"); return true;
    case 't': adv(c); puts(c, "unsigned short"); return true;
    case 'i': adv(c); puts(c, "int"); return true;
    case 'j': adv(c); puts(c, "unsigned int"); return true;
    case 'l': adv(c); puts(c, "long"); return true;
    case 'm': adv(c); puts(c, "unsigned long"); return true;
    case 'x': adv(c); puts(c, "long long"); return true;
    case 'y': adv(c); puts(c, "unsigned long long"); return true;
    case 'n': adv(c); puts(c, "__int128"); return true;
    case 'o': adv(c); puts(c, "unsigned __int128"); return true;
    case 'f': adv(c); puts(c, "float"); return true;
    case 'd': adv(c); puts(c, "double"); return true;
    case 'e': adv(c); puts(c, "long double"); return true;
    case 'g': adv(c); puts(c, "__float128"); return true;
    case 'D':
        // two-character builtin types
        if (c.in[1] == 'i') { c.in += 2; puts(c, "char32_t"); return true; }
        if (c.in[1] == 's') { c.in += 2; puts(c, "char16_t"); return true; }
        if (c.in[1] == 'h') { c.in += 2; puts(c, "float16"); return true; }
        if (c.in[1] == 'a') { c.in += 2; puts(c, "auto"); return true; }
        if (c.in[1] == 'c') { c.in += 2; puts(c, "decltype(auto)"); return true; }
        if (c.in[1] == 'n') { c.in += 2; puts(c, "std::nullptr_t"); return true; }
        if (c.in[1] == 'd') { c.in += 2; puts(c, "decimal64"); return true; }
        if (c.in[1] == 'e') { c.in += 2; puts(c, "decimal128"); return true; }
        if (c.in[1] == 'f') { c.in += 2; puts(c, "decimal32"); return true; }
        return false;
    default:
        return false;
    }
}

// <CV-qualifiers> ::= [r] [V] [K]  (restrict, volatile, const)
static bool parse_cv_qualifiers(Ctx& c) {
    if (peek(c) == 'r') { adv(c); puts(c, "restrict "); }
    if (peek(c) == 'V') { adv(c); puts(c, "volatile "); }
    if (peek(c) == 'K') { adv(c); puts(c, "const "); }
    return true;
}

// <type> ::= <CV-qualifiers> <type>
//         ::= P <type>  (pointer)
//         ::= R <type>  (reference)
//         ::= O <type>  (rvalue reference)
//         ::= C <type>  (complex)
//         ::= G <type>  (imaginary)
//         ::= <builtin-type>
//         ::= <function-type>
//         ::= <nested-name> (class enum type)
//         ::= <substitution>
//         ::= <template-param>
//         ::= <pointer-to-member-type>
//         ::= <array-type>
static bool parse_type(Ctx& c) {
    return parse_type_sub(c, true);
}

static bool parse_type_sub(Ctx& c, bool add_to_subs) {
    char* saved = c.out;
    const char* saved_in = c.in;

    // CV-qualifiers before a type
    // These apply to a base type; we parse them first (prepend)
    bool had_cv = false;
    while (peek(c) == 'r' || peek(c) == 'V' || peek(c) == 'K') {
        had_cv = true;
        if (peek(c) == 'r') { adv(c); if (!puts(c, "restrict ")) return false; }
        if (peek(c) == 'V') { adv(c); if (!puts(c, "volatile ")) return false; }
        if (peek(c) == 'K') { adv(c); if (!puts(c, "const ")) return false; }
    }

    if (had_cv) {
        // The rest of the type follows
        if (!parse_type_sub(c, add_to_subs)) return false;
        if (add_to_subs && is_substitutable_type(*saved_in)) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Try builtin type first
    if (peek(c) == 'v' || peek(c) == 'w' || peek(c) == 'b' || peek(c) == 'c' ||
        peek(c) == 'a' || peek(c) == 'h' || peek(c) == 's' || peek(c) == 't' ||
        peek(c) == 'i' || peek(c) == 'j' || peek(c) == 'l' || peek(c) == 'm' ||
        peek(c) == 'x' || peek(c) == 'y' || peek(c) == 'n' || peek(c) == 'o' ||
        peek(c) == 'f' || peek(c) == 'd' || peek(c) == 'e' || peek(c) == 'g' ||
        (peek(c) == 'D' && c.in[1] != 'T' && c.in[1] != 't' &&
         c.in[1] != '0' && c.in[1] != '1' && c.in[1] != '2' && c.in[1] != '3' &&
         c.in[1] != '4' && c.in[1] != '5'))
    {
        // Nope, for two-char builtins starting with D, check if it's actually a special name
        // For simple builtins:
        if (!parse_builtin_type(c)) return false;
        // built-in types are NOT added to substitution table
        return true;
    }

    // Pointer: P <type>
    if (peek(c) == 'P') {
        adv(c);
        // For pointer-to-member M, handled below
        if (!parse_type_sub(c, true)) return false;
        if (!puts(c, "*")) return false;
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Reference: R <type>
    if (peek(c) == 'R') {
        adv(c);
        if (!parse_type_sub(c, true)) return false;
        if (!puts(c, "&")) return false;
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Rvalue reference: O <type>
    if (peek(c) == 'O') {
        adv(c);
        if (!parse_type_sub(c, true)) return false;
        if (!puts(c, "&&")) return false;
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Pointer-to-member: M <class-type> <member-type>
    if (peek(c) == 'M') {
        adv(c);
        if (!parse_type_sub(c, true)) return false; // class type
        if (!puts(c, "::*")) return false;
        if (!parse_type_sub(c, true)) return false; // member type
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Nested name as type
    if (peek(c) == 'N') {
        char* saved2 = c.out;
        if (!parse_nested_name(c)) return false;
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved2);
            add_sub(c, saved2, len);
        }
        if (add_to_subs) {
            size_t len2 = (size_t)(c.out - saved);
            add_sub(c, saved, len2);
        }
        return true;
    }

    // Substitution
    if (peek(c) == 'S') {
        char* saved2 = c.out;
        if (!parse_substitution(c)) return false;
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Template param: T <template-param-key> _
    if (peek(c) == 'T') {
        adv(c);
        // T_ = first template param, T0_ = second, etc.
        unsigned idx = 0;
        if (peek(c) >= '0' && peek(c) <= '9') { idx = (unsigned)(adv(c) - '0') + 1; }
        if (!match(c, '_')) return false;
        // format as template parameter
        if (!put(c, 'T')) return false;
        if (idx > 0) {
            char buf[16]; int bi = 0;
            if (idx >= 10) { buf[bi++] = '0' + (idx / 10); }
            buf[bi++] = '0' + (idx % 10);
            buf[bi] = '\0';
            if (!puts(c, buf)) return false;
        }
        // template params are substitutable
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Complex function type F...E
    if (peek(c) == 'F') {
        adv(c); // F
        // function type: F <bare-function-type> E
        // write as "("
        if (!put(c, '(')) return false;
        if (!parse_bare_function_type(c)) return false;
        if (peek(c) == 'E') {
            adv(c); // E (end of function type)
        }
        if (!put(c, ')')) return false;
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    // Array type: A <type> or A <number> <type>
    if (peek(c) == 'A') {
        adv(c);
        if (!put(c, '[')) return false;
        if (peek(c) >= '0' && peek(c) <= '9') {
            unsigned n;
            if (!read_number(c, n)) return false;
            char buf[16]; int bi = 0;
            if (n >= 100) { buf[bi++] = '0' + (n / 100) % 10; }
            if (n >= 10) { buf[bi++] = '0' + (n / 10) % 10; }
            buf[bi++] = '0' + n % 10;
            buf[bi] = '\0';
            if (!puts(c, buf)) return false;
        }
        if (!put(c, ']')) return false;
        if (!parse_type_sub(c, true)) return false;
        if (add_to_subs) {
            size_t len = (size_t)(c.out - saved);
            add_sub(c, saved, len);
        }
        return true;
    }

    return false;
}

// <bare-function-type> ::= <type>+
// parameter types for a function
static bool parse_bare_function_type(Ctx& c) {
    bool first = true;
    while (!eof(c) && peek(c) != 'E') {
        if (!first) {
            if (!puts(c, ", ")) return false;
        }
        first = false;
        if (!parse_type(c)) return false;
    }
    return true;
}

// <template-args> ::= I <template-arg>+ E
static bool parse_template_args(Ctx& c) {
    if (!match(c, 'I')) return false;
    if (!put(c, '<')) return false;
    bool first = true;
    while (peek(c) != 'E' && !eof(c)) {
        if (!first) {
            if (!puts(c, ", ")) return false;
        }
        first = false;
        if (!parse_template_arg(c)) return false;
    }
    if (!match(c, 'E')) return false;
    if (!put(c, '>')) return false;
    return true;
}

// <template-arg> ::= <type> | <expr-primary> | X <expression> E | ...
static bool parse_template_arg(Ctx& c) {
    // type argument
    // A template argument that starts with a type character
    char ch = peek(c);
    if (ch == 'L') {
        // L <expr-primary> E - literal argument
        // For now, skip it
        adv(c);
        const char* start, *end;
        if (!read_until(c, 'E', start, end)) return false;
        if (!putn(c, start, (size_t)(end - start))) return false;
        if (!match(c, 'E')) return false;
        return true;
    }
    if (ch == 'X') {
        // X <expression> E - expression argument
        adv(c);
        const char* start, *end;
        if (!read_until(c, 'E', start, end)) return false;
        if (!putn(c, start, (size_t)(end - start))) return false;
        if (!match(c, 'E')) return false;
        return true;
    }
    // type
    return parse_type(c);
}

// <special-name> ::= TV <type> | TT <type> | TI <type> | TS <type> | ...
static bool parse_special_name(Ctx& c) {
    if (!match(c, 'T')) return false;
    if (eof(c)) return false;
    char kind = adv(c);
    switch (kind) {
    case 'V': puts(c, "vtable for "); return parse_type(c);
    case 'T': puts(c, "VTT for "); return parse_type(c);
    case 'I': puts(c, "typeinfo for "); return parse_type(c);
    case 'S': puts(c, "typeinfo name for "); return parse_type(c);
    case 'W': puts(c, "vtable for "); return parse_type(c);
    case 'v': puts(c, "vcall offset "); return parse_type(c); // not quite right
    case 'c': puts(c, "covariant return "); return parse_type(c);
    case 'C': puts(c, "construction vtable for "); return parse_type(c);
    case 'F': puts(c, "virtual function offset "); return parse_type(c);
    case 'G': puts(c, "guard variable for "); return parse_type(c);
    default: return false;
    }
}

// <encoding> ::= <function-name> <bare-function-type>
//             ::= <data-name>
//             ::= <special-name>
static bool parse_encoding(Ctx& c) {
    // Check for special-name (T*)
    if (peek(c) == 'T' && c.in[1] != 'N') {
        // T followed by a special name kind
        return parse_special_name(c);
    }

    // Save name output start position
    char* name_start = c.out;

    // Try nested name first
    if (peek(c) == 'N') {
        if (!parse_nested_name(c)) return false;
        // The function type follows
        if (eof(c)) return true; // data name (no function type)
        if (peek(c) == 'E') return true; // shouldn't happen here
        if (peek(c) == 'v' && (c.in[1] == '\0' || c.in[1] == 'E')) {
            // void function type (no params)
        }
        // Output function parameter list
        if (!put(c, '(')) return false;
        if (!parse_bare_function_type(c)) return false;
        if (!put(c, ')')) return false;
        return true;
    }

    // Local source name (static file-scope function): L<source-name>
    if (peek(c) == 'L') {
        adv(c); // L
        if (!parse_source_name(c)) return false;
        if (!put(c, '(')) return false;
        if (!parse_bare_function_type(c)) return false;
        if (!put(c, ')')) return false;
        return true;
    }

    // Simple source name
    if (peek(c) >= '0' && peek(c) <= '9') {
        if (!parse_source_name(c)) return false;
        if (eof(c)) return true; // data name
        if (peek(c) == 'E') return true;
        // function type follows
        if (!put(c, '(')) return false;
        if (!parse_bare_function_type(c)) return false;
        if (!put(c, ')')) return false;
        return true;
    }

    // std:: name: St<unqualified-name>
    // NOT YET IMPLEMENTED - rare

    return false;
}

// ---- entry point ----

static bool demangle_internal(const char* mangled, Ctx& c) {
    // Check for _Z prefix
    if (mangled[0] != '_' || mangled[1] != 'Z') return false;
    c.in = mangled + 2;

    if (!parse_encoding(c)) return false;

    // null-terminate
    *c.out = '\0';
    return true;
}

} // anonymous namespace

// ---- public API ----

const char* tori::demangle::demangle(const char* mangled, char* buf, size_t bufsz) {
    if (!mangled || !buf || bufsz < 2) return mangled;

    Ctx c;
    c.in = mangled;
    c.out = buf;
    c.out_end = buf + bufsz - 1; // reserve space for null terminator
    c.depth = 0;
    c.num_subs = 0;
    c.mark = buf;

    if (!demangle_internal(mangled, c)) {
        // demangling failed, return original
        // but copy it into buffer for safety
        buf[0] = '\0';
        size_t i = 0;
        for (; mangled[i] && i < bufsz - 1; ++i) buf[i] = mangled[i];
        buf[i] = '\0';
        return buf;
    }

    return buf;
}
