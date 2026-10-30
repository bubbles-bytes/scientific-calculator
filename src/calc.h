#ifndef CALC_H
#define CALC_H

#include <stddef.h>

typedef enum { ANGLE_RAD = 0, ANGLE_DEG, ANGLE_GRAD } AngleMode;

typedef struct {
    AngleMode angle; /* unit used by trig functions */
    double ans;      /* value of `ans` (previous result) */
    double mem;      /* value of `M` (memory register) */
} CalcContext;

/*
 * Evaluate a mathematical expression.
 * Returns 0 on success and stores the value in *out.
 * Returns -1 on error and writes a human-readable message into err.
 */
int calc_eval(const char *expr, const CalcContext *ctx, double *out, char *err, size_t errlen);

/* Format a result for display: 15 significant digits, never "-0". */
void calc_format(double v, char *buf, size_t n);

#endif
