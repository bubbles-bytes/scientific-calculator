#include "../src/calc.h"

#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int total, failures;

static CalcContext make_ctx(AngleMode angle) {
    CalcContext ctx = {angle, 21, 4}; /* ans = 21, M = 4 */
    return ctx;
}

static void ok(const char *expr, AngleMode angle, double expect) {
    CalcContext ctx = make_ctx(angle);
    char err[256];
    double v;
    total++;
    if (calc_eval(expr, &ctx, &v, err, sizeof err) != 0) {
        failures++;
        printf("FAIL  %-28s expected %.15g, got error: %s\n", expr, expect, err);
    } else if (fabs(v - expect) > 1e-9 * fmax(1.0, fabs(expect))) {
        failures++;
        printf("FAIL  %-28s expected %.15g, got %.15g\n", expr, expect, v);
    }
}

static void bad(const char *expr, AngleMode angle) {
    CalcContext ctx = make_ctx(angle);
    char err[256];
    double v;
    total++;
    if (calc_eval(expr, &ctx, &v, err, sizeof err) == 0) {
        failures++;
        printf("FAIL  %-28s expected an error, got %.15g\n", expr, v);
    }
}

static void fmt(double v, const char *expect) {
    char buf[64];
    total++;
    calc_format(v, buf, sizeof buf);
    for (const char *a = buf, *b = expect;; a++, b++) {
        if (*a != *b) {
            failures++;
            printf("FAIL  format(%.17g) expected \"%s\", got \"%s\"\n", v, expect, buf);
            return;
        }
        if (!*a) return;
    }
}

int main(void) {
    const AngleMode R = ANGLE_RAD, D = ANGLE_DEG, G = ANGLE_GRAD;

    /* arithmetic and precedence */
    ok("1+2*3", R, 7);
    ok("(1+2)*3", R, 9);
    ok("10/4", R, 2.5);
    ok("2^3^2", R, 512);
    ok("-2^2", R, -4);
    ok("2^-1", R, 0.5);
    ok("2**10", R, 1024);
    ok("7 mod 3", R, 1);
    ok("-7 mod 3", R, 2);
    ok("3 \xC3\x97 4 \xC3\xB7 2 \xE2\x88\x92 1", R, 5); /* 3 × 4 ÷ 2 − 1 */
    ok("--3", R, 3);
    ok("50%", R, 0.5);
    ok("200*15%", R, 30);
    ok("1.5e3", R, 1500);
    ok("2E-3", R, 0.002);
    ok(".5+.25", R, 0.75);

    /* implicit multiplication */
    ok("2pi", R, 2 * M_PI);
    ok("2(3+4)", R, 14);
    ok("(1+1)(2+2)", R, 8);
    ok("3sin(90)", D, 3);
    ok("2e", R, 2 * 2.718281828459045);

    /* trigonometry */
    ok("sin(30)", D, 0.5);
    ok("cos(60)", D, 0.5);
    ok("tan(45)", D, 1);
    ok("sin(180)", D, 0);
    ok("cos(90)", D, 0);
    ok("sin(pi/2)", R, 1);
    ok("sin(100)", G, 1);
    ok("asin(1)", D, 90);
    ok("acos(0)", R, M_PI / 2);
    ok("atan2(1,1)", D, 45);
    ok("sec(60)", D, 2);
    ok("sin(30\xC2\xB0)", R, 0.5); /* 30° in radian mode */
    ok("sin 30", D, 0.5);
    ok("sinh(0)", R, 0);
    ok("atanh(0.5)", R, 0.5493061443340549);
    ok("cosh(1)^2-sinh(1)^2", R, 1);

    /* roots, powers, logs */
    ok("sqrt(16)", R, 4);
    ok("\xE2\x88\x9A" "16", R, 4); /* √16 */
    ok("cbrt(-27)", R, -3);
    ok("root(32,5)", R, 2);
    ok("(-8)^(1/3)", R, -2);
    ok("ln(e)", R, 1);
    ok("log(1000)", R, 3);
    ok("log(8,2)", R, 3);
    ok("log2(1024)", R, 10);
    ok("exp(0)", R, 1);
    ok("10^-3", R, 0.001);

    /* combinatorics and number theory */
    ok("5!", R, 120);
    ok("0!", R, 1);
    ok("0.5!", R, 0.886226925452758);
    ok("ncr(5,2)", R, 10);
    ok("nCr(52,5)", R, 2598960);
    ok("npr(5,2)", R, 20);
    ok("gcd(12,18)", R, 6);
    ok("lcm(4,6,10)", R, 60);
    ok("gamma(5)", R, 24);

    /* statistics */
    ok("mean(1,2,3,4)", R, 2.5);
    ok("median(3,1,2)", R, 2);
    ok("median(4,1,3,2)", R, 2.5);
    ok("stdev(2,4,4,4,5,5,7,9)", R, 2.138089935299395);
    ok("min(3,-1,2)", R, -1);
    ok("max(3,-1,2)", R, 3);
    ok("sum(1,2,3)", R, 6);

    /* misc */
    ok("round(3.14159,2)", R, 3.14);
    ok("abs(-3)", R, 3);
    ok("hypot(3,4)", R, 5);
    ok("floor(-2.5)", R, -3);
    ok("ans*2", R, 42);
    ok("M+1", R, 5);
    ok("sin(30", D, 0.5); /* unclosed parenthesis at end is allowed */
    ok("PI", R, M_PI);
    ok("\xCF\x80", R, M_PI); /* π */

    /* errors */
    bad("", R);
    bad("1/0", R);
    bad("5 mod 0", R);
    bad("sqrt(-1)", R);
    bad("ln(0)", R);
    bad("log(5,1)", R);
    bad("asin(2)", R);
    bad("tan(90)", D);
    bad("(-1)!", R);
    bad("200!", R);
    bad("foo(2)", R);
    bad("x", R);
    bad("2+", R);
    bad("1)", R);
    bad("1.2.3", R);
    bad("ncr(2,5)", R);
    bad("(-8)^(1/2)", R);
    bad("0^-1", R);
    bad("exp(1000)", R);
    bad("sin()", R);
    bad("hypot(1)", R);
    bad("2 # 3", R);

    /* display formatting */
    fmt(0.1 + 0.2, "0.3");
    fmt(-0.0, "0");
    fmt(1e20, "1e+20");
    fmt(1.0 / 3, "0.333333333333333");

    printf("%d/%d tests passed\n", total - failures, total);
    return failures ? 1 : 0;
}
