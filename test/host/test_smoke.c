#include "t.h"
static void test_harness(void) { T_EQ_INT(1 + 1, 2); T_EQ_STR("a", "a"); }
int main(void) { T_RUN(test_harness); T_DONE(); }
