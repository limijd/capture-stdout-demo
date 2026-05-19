#include "framework.h"
#include "helpers.h"

int g_test_pass = 0;
int g_test_fail = 0;
const char *g_current_test = NULL;

int main(void) {
    /* tests will be added in Phase C tasks 5-10 */
    fprintf(stderr, "\nTotal: %d passed, %d failed\n",
            g_test_pass, g_test_fail);
    return g_test_fail > 0 ? 1 : 0;
}
