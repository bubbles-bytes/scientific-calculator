/*
 * Recursive-descent expression evaluator.
 *
 * Grammar (lowest to highest precedence):
 *   add     := mul (('+' | '-') mul)*
 *   mul     := unary (('*' | '/' | 'mod' | <implicit>) unary)*
 *   unary   := ('-' | '+') unary | power
 *   power   := postfix (('^' | '**') unary)?          right-associative
 *   postfix := primary ('!' | '%' | '°')*
 *   primary := number | '(' add ')' | constant | function ['(' args ')' | unary]
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "calc.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define CALC_E   2.71828182845904523536
#define CALC_PHI 1.61803398874989484820

#define MAX_ARGS  64
#define MAX_DEPTH 128

/* UTF-8 symbols accepted as aliases, so the UI can show proper math glyphs. */
#define U_PI    "\xCF\x80"     /* π */
#define U_TAU   "\xCF\x84"     /* τ */
#define U_TIMES "\xC3\x97"     /* × */
#define U_DOT   "\xC2\xB7"     /* · */
#define U_DIV   "\xC3\xB7"     /* ÷ */
#define U_MINUS "\xE2\x88\x92" /* − */
#define U_SQRT  "\xE2\x88\x9A" /* √ */
#define U_CBRT  "\xE2\x88\x9B" /* ∛ */
#define U_DEG   "\xC2\xB0"     /* ° */

typedef enum {
    F_SIN, F_COS, F_TAN, F_SEC, F_CSC, F_COT,
    F_ASIN, F_ACOS, F_ATAN, F_ATAN2,
    F_SINH, F_COSH, F_TANH, F_ASINH, F_ACOSH, F_ATANH,
    F_LN, F_LOG, F_LOG2, F_LOG10, F_EXP,
    F_SQRT, F_CBRT, F_ROOT, F_POW, F_ABS, F_SIGN,
    F_FLOOR, F_CEIL, F_ROUND, F_TRUNC, F_FRAC,
    F_FACT, F_GAMMA, F_NCR, F_NPR, F_GCD, F_LCM, F_MOD, F_HYPOT,
    F_MIN, F_MAX, F_SUM, F_MEAN, F_MEDIAN, F_VAR, F_STDEV,
    F_DEG, F_RAD, F_RAND
} FuncId;

typedef struct {
    const char *name;
    FuncId id;
    int min_args;
    int max_args; /* -1 = variadic */
} FuncDef;

static const FuncDef FUNCS[] = {
    {"sin", F_SIN, 1, 1},       {"cos", F_COS, 1, 1},       {"tan", F_TAN, 1, 1},
    {"sec", F_SEC, 1, 1},       {"csc", F_CSC, 1, 1},       {"cot", F_COT, 1, 1},
    {"asin", F_ASIN, 1, 1},     {"acos", F_ACOS, 1, 1},     {"atan", F_ATAN, 1, 1},
    {"arcsin", F_ASIN, 1, 1},   {"arccos", F_ACOS, 1, 1},   {"arctan", F_ATAN, 1, 1},
    {"atan2", F_ATAN2, 2, 2},
    {"sinh", F_SINH, 1, 1},     {"cosh", F_COSH, 1, 1},     {"tanh", F_TANH, 1, 1},
    {"asinh", F_ASINH, 1, 1},   {"acosh", F_ACOSH, 1, 1},   {"atanh", F_ATANH, 1, 1},
    {"ln", F_LN, 1, 1},         {"log", F_LOG, 1, 2},       {"log2", F_LOG2, 1, 1},
    {"log10", F_LOG10, 1, 1},   {"exp", F_EXP, 1, 1},
    {"sqrt", F_SQRT, 1, 1},     {"cbrt", F_CBRT, 1, 1},     {"root", F_ROOT, 2, 2},
    {"pow", F_POW, 2, 2},       {"abs", F_ABS, 1, 1},       {"sign", F_SIGN, 1, 1},
    {"floor", F_FLOOR, 1, 1},   {"ceil", F_CEIL, 1, 1},     {"round", F_ROUND, 1, 2},
    {"trunc", F_TRUNC, 1, 1},   {"frac", F_FRAC, 1, 1},
    {"fact", F_FACT, 1, 1},     {"gamma", F_GAMMA, 1, 1},
    {"ncr", F_NCR, 2, 2},       {"npr", F_NPR, 2, 2},
    {"gcd", F_GCD, 1, -1},      {"lcm", F_LCM, 1, -1},      {"mod", F_MOD, 2, 2},
    {"hypot", F_HYPOT, 2, 2},
    {"min", F_MIN, 1, -1},      {"max", F_MAX, 1, -1},      {"sum", F_SUM, 1, -1},
    {"mean", F_MEAN, 1, -1},    {"avg", F_MEAN, 1, -1},     {"median", F_MEDIAN, 1, -1},
    {"var", F_VAR, 2, -1},      {"stdev", F_STDEV, 2, -1},
    {"deg", F_DEG, 1, 1},       {"rad", F_RAD, 1, 1},       {"rand", F_RAND, 0, 0},
};

typedef struct {
    const char *s;
    size_t p;
    const CalcContext *ctx;
    int failed;
    char *err;
    size_t errlen;
    int depth;
} Parser;

static double parse_add(Parser *ps);
static double parse_unary(Parser *ps);
static double parse_power(Parser *ps);

/* ------------------------------------------------------------------ helpers */

static void fail(Parser *ps, const char *fmt, ...) {
    va_list ap;
    if (ps->failed) return;
    ps->failed = 1;
    if (ps->errlen == 0) return;
    va_start(ap, fmt);
    vsnprintf(ps->err, ps->errlen, fmt, ap);
    va_end(ap);
}

static const char *cur(Parser *ps) {
    while (isspace((unsigned char)ps->s[ps->p])) ps->p++;
    return ps->s + ps->p;
}

static int has_prefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int accept(Parser *ps, const char *tok) {
    if (has_prefix(cur(ps), tok)) {
        ps->p += strlen(tok);
        return 1;
    }
    return 0;
}

/* Accept a keyword operator such as "mod" only when it is a whole word. */
static int accept_word(Parser *ps, const char *word) {
    const char *c = cur(ps);
    size_t n = strlen(word);
    if (strncasecmp(c, word, n) == 0 && !isalnum((unsigned char)c[n]) && c[n] != '_') {
        ps->p += n;
        return 1;
    }
    return 0;
}

static int enter(Parser *ps) {
    if (++ps->depth > MAX_DEPTH) {
        fail(ps, "Expression is nested too deeply");
        return 0;
    }
    return 1;
}

static int is_int(double x) { return isfinite(x) && x == floor(x); }

static int starts_operand(const char *c) {
    unsigned char ch = (unsigned char)*c;
    return isdigit(ch) || ch == '.' || ch == '(' || isalpha(ch) || ch == '_' ||
           has_prefix(c, U_PI) || has_prefix(c, U_TAU) ||
           has_prefix(c, U_SQRT) || has_prefix(c, U_CBRT);
}

static int name_is(const char *s, size_t len, const char *name) {
    return strlen(name) == len && strncasecmp(s, name, len) == 0;
}

/* --------------------------------------------------------------- angles */

/* Convert an angle in the current unit to radians, reducing full turns first for precision. */
static double trig_arg(const Parser *ps, double x) {
    switch (ps->ctx->angle) {
    case ANGLE_DEG:  return fmod(x, 360.0) * (M_PI / 180.0);
    case ANGLE_GRAD: return fmod(x, 400.0) * (M_PI / 200.0);
    default:         return x;
    }
}

static double from_rad(const Parser *ps, double r) {
    switch (ps->ctx->angle) {
    case ANGLE_DEG:  return r * (180.0 / M_PI);
    case ANGLE_GRAD: return r * (200.0 / M_PI);
    default:         return r;
    }
}

/* The ° postfix: express a value given in degrees in the current angle unit. */
static double degrees_to_mode(const Parser *ps, double x) {
    switch (ps->ctx->angle) {
    case ANGLE_DEG:  return x;
    case ANGLE_GRAD: return x * (400.0 / 360.0);
    default:         return x * (M_PI / 180.0);
    }
}

static void sin_cos(const Parser *ps, double x, double *s, double *c) {
    double r = trig_arg(ps, x);
    double eps = 4 * DBL_EPSILON * fabs(r);
    *s = sin(r);
    *c = cos(r);
    /* Snap rounding noise at multiples of a quarter turn: sin(180°) is 0, not 1.2e-16. */
    if (fabs(*s) < eps) *s = 0;
    if (fabs(*c) < eps) *c = 0;
}

/* ----------------------------------------------------------------- maths */

static double factorial(Parser *ps, double x) {
    if (isnan(x)) return x;
    if (x < 0 && is_int(x)) {
        fail(ps, "Factorial of a negative integer is undefined");
        return NAN;
    }
    if (x > 170.62) {
        fail(ps, "Factorial overflow (maximum is 170!)");
        return NAN;
    }
    if (is_int(x)) {
        double r = 1;
        for (int i = 2; i <= (int)x; i++) r *= i;
        return r;
    }
    return tgamma(x + 1); /* Gamma extends factorial to non-integers */
}

static double power(Parser *ps, double b, double e) {
    if (b == 0 && e < 0) {
        fail(ps, "Division by zero");
        return NAN;
    }
    if (b < 0 && !is_int(e) && isfinite(e)) {
        /* Odd roots of negative numbers are real: (-8)^(1/3) = -2. */
        double inv = 1.0 / e;
        if (fabs(inv - round(inv)) < 1e-9 && fmod(round(inv), 2.0) != 0) return -pow(-b, e);
        fail(ps, "Negative base with a fractional exponent is not a real number");
        return NAN;
    }
    return pow(b, e);
}

static double math_mod(Parser *ps, double a, double b) {
    double r;
    if (b == 0) {
        fail(ps, "Modulo by zero");
        return NAN;
    }
    r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b; /* result takes the divisor's sign */
    return r;
}

static double gcd2(double a, double b) {
    a = fabs(a);
    b = fabs(b);
    while (b != 0) {
        double t = fmod(a, b);
        a = b;
        b = t;
    }
    return a;
}

static int cmp_double(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

static double apply(Parser *ps, const FuncDef *f, const double *a, int n) {
    double x = n > 0 ? a[0] : 0;
    double s, c, acc;
    int i;

    switch (f->id) {
    case F_SIN: sin_cos(ps, x, &s, &c); return s;
    case F_COS: sin_cos(ps, x, &s, &c); return c;
    case F_TAN:
        sin_cos(ps, x, &s, &c);
        if (c == 0) { fail(ps, "tan is undefined at this angle"); return NAN; }
        return s / c;
    case F_SEC:
        sin_cos(ps, x, &s, &c);
        if (c == 0) { fail(ps, "sec is undefined at this angle"); return NAN; }
        return 1 / c;
    case F_CSC:
        sin_cos(ps, x, &s, &c);
        if (s == 0) { fail(ps, "csc is undefined at this angle"); return NAN; }
        return 1 / s;
    case F_COT:
        sin_cos(ps, x, &s, &c);
        if (s == 0) { fail(ps, "cot is undefined at this angle"); return NAN; }
        return c / s;

    case F_ASIN:
        if (x < -1 || x > 1) { fail(ps, "asin requires -1 <= x <= 1"); return NAN; }
        return from_rad(ps, asin(x));
    case F_ACOS:
        if (x < -1 || x > 1) { fail(ps, "acos requires -1 <= x <= 1"); return NAN; }
        return from_rad(ps, acos(x));
    case F_ATAN:  return from_rad(ps, atan(x));
    case F_ATAN2: return from_rad(ps, atan2(a[0], a[1]));

    case F_SINH:  return sinh(x);
    case F_COSH:  return cosh(x);
    case F_TANH:  return tanh(x);
    case F_ASINH: return asinh(x);
    case F_ACOSH:
        if (x < 1) { fail(ps, "acosh requires x >= 1"); return NAN; }
        return acosh(x);
    case F_ATANH:
        if (x <= -1 || x >= 1) { fail(ps, "atanh requires -1 < x < 1"); return NAN; }
        return atanh(x);

    case F_LN:
        if (x <= 0) { fail(ps, "ln requires a positive argument"); return NAN; }
        return log(x);
    case F_LOG:
        if (x <= 0) { fail(ps, "log requires a positive argument"); return NAN; }
        if (n == 2) {
            if (a[1] <= 0 || a[1] == 1) { fail(ps, "log base must be positive and not 1"); return NAN; }
            return log(x) / log(a[1]);
        }
        return log10(x);
    case F_LOG2:
        if (x <= 0) { fail(ps, "log2 requires a positive argument"); return NAN; }
        return log2(x);
    case F_LOG10:
        if (x <= 0) { fail(ps, "log10 requires a positive argument"); return NAN; }
        return log10(x);
    case F_EXP: return exp(x);

    case F_SQRT:
        if (x < 0) { fail(ps, "Square root of a negative number is not real"); return NAN; }
        return sqrt(x);
    case F_CBRT: return cbrt(x);
    case F_ROOT:
        if (a[1] == 0) { fail(ps, "root: the 0th root is undefined"); return NAN; }
        return power(ps, x, 1.0 / a[1]);
    case F_POW: return power(ps, a[0], a[1]);

    case F_ABS:   return fabs(x);
    case F_SIGN:  return (x > 0) - (x < 0);
    case F_FLOOR: return floor(x);
    case F_CEIL:  return ceil(x);
    case F_TRUNC: return trunc(x);
    case F_FRAC:  return x - trunc(x);
    case F_ROUND:
        if (n == 2) {
            double scale;
            if (!is_int(a[1]) || fabs(a[1]) > 15) { fail(ps, "round digits must be an integer from -15 to 15"); return NAN; }
            scale = pow(10, a[1]);
            return round(x * scale) / scale;
        }
        return round(x);

    case F_FACT: return factorial(ps, x);
    case F_GAMMA:
        if (x <= 0 && is_int(x)) { fail(ps, "gamma is undefined at non-positive integers"); return NAN; }
        return tgamma(x);
    case F_NCR:
    case F_NPR: {
        double nn = a[0], r = a[1], res = 1;
        if (!is_int(nn) || !is_int(r) || nn < 0 || r < 0) {
            fail(ps, "%s requires non-negative integers", f->name);
            return NAN;
        }
        if (r > nn) { fail(ps, "%s requires r <= n", f->name); return NAN; }
        if (f->id == F_NPR) {
            for (double k = 0; k < r && isfinite(res); k++) res *= nn - k;
            return res;
        }
        if (r > nn - r) r = nn - r;
        for (double k = 1; k <= r && isfinite(res); k++) res = res * (nn - r + k) / k;
        return round(res);
    }
    case F_GCD:
    case F_LCM:
        acc = fabs(a[0]);
        for (i = 0; i < n; i++) {
            if (!is_int(a[i])) { fail(ps, "%s requires integers", f->name); return NAN; }
            if (i == 0) continue;
            if (f->id == F_GCD) acc = gcd2(acc, a[i]);
            else acc = (acc == 0 || a[i] == 0) ? 0 : fabs(acc / gcd2(acc, a[i]) * a[i]);
        }
        return acc;
    case F_MOD:   return math_mod(ps, a[0], a[1]);
    case F_HYPOT: return hypot(a[0], a[1]);

    case F_MIN:
        acc = a[0];
        for (i = 1; i < n; i++) if (a[i] < acc) acc = a[i];
        return acc;
    case F_MAX:
        acc = a[0];
        for (i = 1; i < n; i++) if (a[i] > acc) acc = a[i];
        return acc;
    case F_SUM:
    case F_MEAN:
        acc = 0;
        for (i = 0; i < n; i++) acc += a[i];
        return f->id == F_SUM ? acc : acc / n;
    case F_MEDIAN: {
        double sorted[MAX_ARGS];
        memcpy(sorted, a, (size_t)n * sizeof *a);
        qsort(sorted, (size_t)n, sizeof *sorted, cmp_double);
        return n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    }
    case F_VAR:
    case F_STDEV: {
        double mean = 0, ss = 0;
        for (i = 0; i < n; i++) mean += a[i];
        mean /= n;
        for (i = 0; i < n; i++) ss += (a[i] - mean) * (a[i] - mean);
        ss /= n - 1; /* sample variance */
        return f->id == F_VAR ? ss : sqrt(ss);
    }

    case F_DEG:  return x * (180.0 / M_PI);
    case F_RAD:  return x * (M_PI / 180.0);
    case F_RAND: return rand() / ((double)RAND_MAX + 1.0);
    }
    fail(ps, "Unsupported function %s", f->name);
    return NAN;
}

/* ---------------------------------------------------------------- parser */

static double parse_number(Parser *ps) {
    const char *s = ps->s + ps->p;
    char buf[128];
    size_t i = 0, n = 0;
    int too_long = 0;

#define TAKE(ch) do { if (n + 1 < sizeof buf) buf[n++] = (ch); else too_long = 1; } while (0)
    while (isdigit((unsigned char)s[i])) { TAKE(s[i]); i++; }
    if (s[i] == '.') {
        TAKE('.');
        i++;
        while (isdigit((unsigned char)s[i])) { TAKE(s[i]); i++; }
    }
    if (s[i] == 'e' || s[i] == 'E') {
        size_t j = i + 1;
        char sign = 0;
        if (s[j] == '+' || s[j] == '-') sign = s[j++];
        else if (has_prefix(s + j, U_MINUS)) { sign = '-'; j += strlen(U_MINUS); }
        /* Only an exponent if digits follow; otherwise "2e" means 2 × e. */
        if (isdigit((unsigned char)s[j])) {
            TAKE('e');
            if (sign) TAKE(sign);
            while (isdigit((unsigned char)s[j])) { TAKE(s[j]); j++; }
            i = j;
        }
    }
#undef TAKE
    buf[n] = '\0';
    ps->p += i;
    if (too_long) { fail(ps, "Number is too long"); return NAN; }
    if (s[i] == '.') { fail(ps, "Malformed number"); return NAN; }
    return strtod(buf, NULL);
}

static double call_function(Parser *ps, const FuncDef *f) {
    double args[MAX_ARGS];
    int argc = 0;

    if (accept(ps, "(")) {
        if (!accept(ps, ")")) {
            for (;;) {
                if (argc == MAX_ARGS) { fail(ps, "Too many arguments to %s", f->name); return NAN; }
                args[argc++] = parse_add(ps);
                if (ps->failed) return NAN;
                if (accept(ps, ",")) continue;
                if (accept(ps, ")") || *cur(ps) == '\0') break; /* allow unclosed ')' at end */
                fail(ps, "Expected ',' or ')' in %s(...)", f->name);
                return NAN;
            }
        }
    } else if (f->max_args != 0) {
        args[argc++] = parse_unary(ps); /* "sin 30", "ln2" */
        if (ps->failed) return NAN;
    }

    if (argc < f->min_args || (f->max_args >= 0 && argc > f->max_args)) {
        const char *plural = f->min_args == 1 ? "" : "s";
        if (f->max_args < 0)
            fail(ps, "%s() needs at least %d argument%s", f->name, f->min_args, plural);
        else if (f->min_args == f->max_args)
            fail(ps, "%s() takes %d argument%s", f->name, f->min_args, plural);
        else
            fail(ps, "%s() takes %d to %d arguments", f->name, f->min_args, f->max_args);
        return NAN;
    }
    return apply(ps, f, args, argc);
}

static const FuncDef *find_func(const char *s, size_t len) {
    for (size_t i = 0; i < sizeof FUNCS / sizeof FUNCS[0]; i++)
        if (name_is(s, len, FUNCS[i].name)) return &FUNCS[i];
    return NULL;
}

static int find_const(const Parser *ps, const char *s, size_t len, double *out) {
    if (name_is(s, len, "pi")) *out = M_PI;
    else if (name_is(s, len, "e")) *out = CALC_E;
    else if (name_is(s, len, "phi")) *out = CALC_PHI;
    else if (name_is(s, len, "tau")) *out = 2 * M_PI;
    else if (name_is(s, len, "ans")) *out = ps->ctx->ans;
    else if (name_is(s, len, "m") || name_is(s, len, "mem")) *out = ps->ctx->mem;
    else return 0;
    return 1;
}

static double parse_ident(Parser *ps) {
    const char *s = ps->s + ps->p;
    size_t full = 0, alpha = 0;
    size_t lens[2];

    while (isalnum((unsigned char)s[full]) || s[full] == '_') full++;
    while (isalpha((unsigned char)s[alpha])) alpha++;

    /* Prefer the whole identifier ("log2"), then its alphabetic prefix ("sin30" -> sin 30). */
    lens[0] = full;
    lens[1] = alpha;
    for (int k = 0; k < 2; k++) {
        size_t len = lens[k];
        const FuncDef *f;
        double value;
        if (len == 0 || (k == 1 && len == full)) continue;
        if ((f = find_func(s, len)) != NULL) {
            ps->p += len;
            return call_function(ps, f);
        }
        if (find_const(ps, s, len, &value)) {
            ps->p += len;
            return value;
        }
    }
    fail(ps, "Unknown name '%.*s'", (int)full, s);
    return NAN;
}

static double parse_primary(Parser *ps) {
    const char *c;
    double v = NAN;

    if (!enter(ps)) return NAN;
    c = cur(ps);

    if (isdigit((unsigned char)c[0]) || (c[0] == '.' && isdigit((unsigned char)c[1]))) {
        v = parse_number(ps);
    } else if (accept(ps, "(")) {
        v = parse_add(ps);
        if (!ps->failed && !accept(ps, ")") && *cur(ps) != '\0') fail(ps, "Expected ')'");
    } else if (accept(ps, U_PI)) {
        v = M_PI;
    } else if (accept(ps, U_TAU)) {
        v = 2 * M_PI;
    } else if (accept(ps, U_SQRT)) {
        double x = parse_unary(ps);
        if (!ps->failed) v = apply(ps, find_func("sqrt", 4), &x, 1);
    } else if (accept(ps, U_CBRT)) {
        v = cbrt(parse_unary(ps));
    } else if (isalpha((unsigned char)c[0]) || c[0] == '_') {
        v = parse_ident(ps);
    } else if (c[0] == '\0') {
        fail(ps, "Incomplete expression");
    } else if ((unsigned char)c[0] < 0x80) {
        fail(ps, "Unexpected '%c'", c[0]);
    } else {
        fail(ps, "Unexpected symbol");
    }

    ps->depth--;
    return v;
}

static double parse_postfix(Parser *ps) {
    double v = parse_primary(ps);
    while (!ps->failed) {
        if (accept(ps, "!")) v = factorial(ps, v);
        else if (accept(ps, "%")) v /= 100;
        else if (accept(ps, U_DEG)) v = degrees_to_mode(ps, v);
        else break;
    }
    return v;
}

static double parse_power(Parser *ps) {
    double base = parse_postfix(ps);
    if (!ps->failed && (accept(ps, "^") || accept(ps, "**"))) {
        double e = parse_unary(ps); /* right-associative; allows 2^-1 */
        if (ps->failed) return NAN;
        return power(ps, base, e);
    }
    return base;
}

static double parse_unary(Parser *ps) {
    double v;
    if (!enter(ps)) return NAN;
    if (accept(ps, "-") || accept(ps, U_MINUS)) v = -parse_unary(ps);
    else if (accept(ps, "+")) v = parse_unary(ps);
    else v = parse_power(ps);
    ps->depth--;
    return v;
}

static double parse_mul(Parser *ps) {
    double v = parse_unary(ps);
    while (!ps->failed) {
        const char *c = cur(ps);
        if (c[0] == '*' && c[1] != '*') {
            ps->p++;
            v *= parse_unary(ps);
        } else if (accept(ps, U_TIMES) || accept(ps, U_DOT)) {
            v *= parse_unary(ps);
        } else if (accept(ps, "/") || accept(ps, U_DIV)) {
            double d = parse_unary(ps);
            if (!ps->failed && d == 0) { fail(ps, "Division by zero"); return NAN; }
            v /= d;
        } else if (accept_word(ps, "mod")) {
            double d = parse_unary(ps);
            if (!ps->failed) v = math_mod(ps, v, d);
        } else if (starts_operand(c)) {
            v *= parse_power(ps); /* implicit multiplication: 2pi, 3(4+1), 2sin(30) */
        } else {
            break;
        }
    }
    return v;
}

static double parse_add(Parser *ps) {
    double v = parse_mul(ps);
    while (!ps->failed) {
        if (accept(ps, "+")) v += parse_mul(ps);
        else if (accept(ps, "-") || accept(ps, U_MINUS)) v -= parse_mul(ps);
        else break;
    }
    return v;
}

/* ------------------------------------------------------------ public API */

int calc_eval(const char *expr, const CalcContext *ctx, double *out, char *err, size_t errlen) {
    Parser ps = {expr, 0, ctx, 0, err, errlen, 0};
    double v;

    if (errlen) err[0] = '\0';
    if (*cur(&ps) == '\0') {
        fail(&ps, "Empty expression");
        return -1;
    }

    v = parse_add(&ps);
    if (!ps.failed && *cur(&ps) != '\0') {
        char ch = *cur(&ps);
        if (ch == ')') fail(&ps, "Unmatched ')'");
        else if ((unsigned char)ch < 0x80) fail(&ps, "Unexpected '%c'", ch);
        else fail(&ps, "Unexpected symbol");
    }
    if (!ps.failed && isnan(v)) fail(&ps, "Math error: result is undefined");
    if (!ps.failed && isinf(v)) fail(&ps, "Math error: result is too large");
    if (ps.failed) return -1;

    *out = v;
    return 0;
}

void calc_format(double v, char *buf, size_t n) {
    if (v == 0) v = 0; /* turns -0 into 0 */
    snprintf(buf, n, "%.15g", v);
}
