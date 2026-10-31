#pragma once
#include <stdio.h>
#include <string.h>
static int t_fail, t_count;
#define T_CHECK(c) do { t_count++; if (!(c)) { t_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define T_EQ_INT(a, b) do { long long _a = (a), _b = (b); t_count++; if (_a != _b) { t_fail++; printf("FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define T_EQ_STR(a, b) do { const char *_a = (a), *_b = (b); t_count++; if (strcmp(_a, _b)) { t_fail++; printf("FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define T_RUN(fn) do { printf("- %s\n", #fn); fn(); } while (0)
#define T_DONE() do { printf("%d checks, %d failed\n", t_count, t_fail); return t_fail ? 1 : 0; } while (0)
