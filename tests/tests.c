#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include <stdio.h>
#include "wows-geometry.h"

void test_wows_parse_geometry_fp(void) {
	return;
}

int main() {
    CU_initialize_registry();
    CU_pSuite suite = CU_add_suite("wows_parse_geometry", NULL, NULL);
    CU_add_test(suite, "test_wows_parse_geometry_fp", test_wows_parse_geometry_fp);
    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();

    return CU_get_error();
}
