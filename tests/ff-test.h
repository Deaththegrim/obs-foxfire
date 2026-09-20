#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static int ff_fails = 0, ff_checks = 0;
#define CHECK(cond) do { ff_checks++; if (!(cond)) { ff_fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))
#define FF_TEST_MAIN_END() do { fprintf(stderr, "%s: %d checks, %d failed\n", __FILE__, ff_checks, ff_fails); return ff_fails ? 1 : 0; } while (0)
