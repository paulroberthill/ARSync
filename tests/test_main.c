/* test_main.c - unit-test runner */
#include "test.h"
#include "plat.h"

int test_checks;
int test_failures;

static int run_tests(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_wire();
    test_amitime();
    test_plat();
    test_session();
    test_auth_suite();

    printf("%d checks, %d failed\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    return plat_run(run_tests, argc, argv);
}
