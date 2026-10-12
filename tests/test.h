/* test.h - minimal unit-test harness (runs on the host and under vamos) */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

extern int test_checks;
extern int test_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        test_checks++;                                                       \
        if (!(cond)) {                                                       \
            test_failures++;                                                 \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* Integer comparison that prints both values; long long keeps it portable. */
#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long a_ = (long long)(a), b_ = (long long)(b);                  \
        test_checks++;                                                       \
        if (a_ != b_) {                                                      \
            test_failures++;                                                 \
            printf("%s:%d: %s == %s failed: %lld != %lld\n",                 \
                   __FILE__, __LINE__, #a, #b, a_, b_);                      \
        }                                                                    \
    } while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

void test_wire(void);
void test_amitime(void);
void test_plat(void);
void test_session(void);
void test_auth_suite(void);
void test_flist_suite(void);

#endif
