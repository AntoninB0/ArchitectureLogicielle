#ifndef TEST_H
#define TEST_H

#include <stdio.h>

static int tests_ok, tests_ko;

#define VERIFIER(cond) do { \
    if (cond) { tests_ok++; } \
    else { tests_ko++; printf("  ECHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define BILAN() (printf("%d verifications OK, %d en echec\n", tests_ok, tests_ko), tests_ko != 0)

#endif
