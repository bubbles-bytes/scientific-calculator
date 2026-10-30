# Scientific Calculator (C backend + web frontend)

All of the math happens in C. The browser UI sends each expression to the C server and shows the result.

```
src/calc.c      expression engine (recursive-descent parser + evaluator)
src/server.c    HTTP server (POSIX sockets), CLI and REPL
public/         frontend (HTML/CSS/JS), served by the C server
tests/          engine test suite
```

## Build and run

```sh
make            # builds ./calc
make test       # runs the engine tests
make run        # starts http://localhost:8080  (PORT=9000 make run to change)
```

Then open http://localhost:8080.

Terminal use:

```sh
./calc -e "sin(30)" -d     # one-shot, degrees
./calc -i                  # REPL (commands: deg, rad, grad, quit)
```

## API

`GET /api/eval?expr=<expr>&angle=deg|rad|grad&ans=<number>&mem=<number>`

```json
{"ok":true,"result":"0.5","raw":"0.5"}
{"ok":false,"error":"Division by zero"}
```

`GET /api/health` returns `{"ok":true,...}`.

## Supported syntax

| Category | Syntax |
|---|---|
| Operators | `+ - * / ^` (also `× ÷ − **`), `mod`, postfix `!` `%` `°`, implicit multiplication (`2π`, `3(4+1)`) |
| Constants | `pi`/`π`, `e`, `phi`, `tau`, `ans` (previous result), `M` (memory) |
| Trig | `sin cos tan sec csc cot asin acos atan atan2` in DEG / RAD / GRAD |
| Hyperbolic | `sinh cosh tanh asinh acosh atanh` |
| Logs and powers | `ln log(x) log(x,b) log2 log10 exp sqrt/√ cbrt root(x,n) pow` |
| Rounding | `abs sign floor ceil round(x[,digits]) trunc frac` |
| Combinatorics | `n! fact gamma nCr nPr gcd lcm` |
| Statistics | `min max sum mean/avg median var stdev` (sample variance and stdev) |
| Other | `hypot deg rad rand` |

Errors are reported with a clear message, for example division by zero, domain errors such as `sqrt(-1)` and `asin(2)`, `tan(90°)`, overflow, and syntax errors.
