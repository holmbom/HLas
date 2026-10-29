#include <cstdio>
#include "HLas.h"

int test_types();
int test_file_io();
int test_header();
int test_vlr();
int test_point_reader();
int test_reader();
int test_integration();
int test_sidecar();
int test_sidecar_lite();
int test_copc();
int test_copc_root_extent();
int test_copc_reader();
int test_net_settings();
int test_point_view();
int test_point_view_files();
int test_classification_flags();
int test_golden();
int test_golden_encode();
int test_golden10();
int test_golden10_encode();
int test_encoder_refusal();
int test_write_full();
int test_write_full_legacy();
int test_reencode_legacy();
int test_las_writer();
int test_laz_encoder();

int main() {
    int fails = 0;
    fails += test_types();
    fails += test_file_io();
    fails += test_header();
    fails += test_vlr();
    fails += test_point_reader();
    fails += test_reader();
    fails += test_copc();
    fails += test_copc_root_extent();
    fails += test_copc_reader();
    fails += test_net_settings();
    fails += test_point_view();
    fails += test_point_view_files();
    fails += test_classification_flags();
    fails += test_golden();
    fails += test_golden_encode();
    fails += test_golden10();
    fails += test_golden10_encode();
    fails += test_encoder_refusal();
    fails += test_write_full();
    fails += test_write_full_legacy();
    fails += test_reencode_legacy();
    fails += test_las_writer();
    fails += test_laz_encoder();
    fails += test_integration();
    fails += test_sidecar_lite();
    fails += test_sidecar();

    if (fails == 0)
        printf("All tests passed.\n");
    else
        printf("%d test(s) FAILED.\n", fails);

    return fails;
}
