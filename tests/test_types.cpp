#include "las_types.h"
#include "las_error.h"
#include <cstdio>
#include <cstddef>

using namespace HLas::detail;

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

int test_types() {
    int fails = 0;
    printf("test_types:\n");

    // --- LasHeaderBase (227 bytes) ---
    CHECK(sizeof(LasHeaderBase) == 227);
    CHECK(offsetof(LasHeaderBase, file_signature)         == 0);
    CHECK(offsetof(LasHeaderBase, file_source_id)         == 4);
    CHECK(offsetof(LasHeaderBase, global_encoding)        == 6);
    CHECK(offsetof(LasHeaderBase, guid_data_1)            == 8);
    CHECK(offsetof(LasHeaderBase, guid_data_2)            == 12);
    CHECK(offsetof(LasHeaderBase, guid_data_3)            == 14);
    CHECK(offsetof(LasHeaderBase, guid_data_4)            == 16);
    CHECK(offsetof(LasHeaderBase, version_major)          == 24);
    CHECK(offsetof(LasHeaderBase, version_minor)          == 25);
    CHECK(offsetof(LasHeaderBase, system_identifier)      == 26);
    CHECK(offsetof(LasHeaderBase, generating_software)    == 58);
    CHECK(offsetof(LasHeaderBase, creation_day)           == 90);
    CHECK(offsetof(LasHeaderBase, creation_year)          == 92);
    CHECK(offsetof(LasHeaderBase, header_size)            == 94);
    CHECK(offsetof(LasHeaderBase, offset_to_point_data)   == 96);
    CHECK(offsetof(LasHeaderBase, num_vlrs)               == 100);
    CHECK(offsetof(LasHeaderBase, point_format_id)        == 104);
    CHECK(offsetof(LasHeaderBase, point_record_length)    == 105);
    CHECK(offsetof(LasHeaderBase, legacy_point_count)     == 107);
    CHECK(offsetof(LasHeaderBase, legacy_points_by_return) == 111);
    CHECK(offsetof(LasHeaderBase, x_scale)                == 131);
    CHECK(offsetof(LasHeaderBase, y_scale)                == 139);
    CHECK(offsetof(LasHeaderBase, z_scale)                == 147);
    CHECK(offsetof(LasHeaderBase, x_offset)               == 155);
    CHECK(offsetof(LasHeaderBase, y_offset)               == 163);
    CHECK(offsetof(LasHeaderBase, z_offset)               == 171);
    CHECK(offsetof(LasHeaderBase, max_x)                  == 179);
    CHECK(offsetof(LasHeaderBase, min_x)                  == 187);
    CHECK(offsetof(LasHeaderBase, max_y)                  == 195);
    CHECK(offsetof(LasHeaderBase, min_y)                  == 203);
    CHECK(offsetof(LasHeaderBase, max_z)                  == 211);
    CHECK(offsetof(LasHeaderBase, min_z)                  == 219);

    // --- LasHeader13Ext (8 bytes, offset 227) ---
    CHECK(sizeof(LasHeader13Ext) == 8);

    // --- LasHeader14Ext (140 bytes, offset 235) ---
    CHECK(sizeof(LasHeader14Ext) == 140);
    CHECK(offsetof(LasHeader14Ext, start_of_first_evlr)  == 0);
    CHECK(offsetof(LasHeader14Ext, num_evlrs)            == 8);
    CHECK(offsetof(LasHeader14Ext, point_count)          == 12);
    CHECK(offsetof(LasHeader14Ext, points_by_return)     == 20);

    // --- VlrHeader (54 bytes) ---
    CHECK(sizeof(VlrHeader) == 54);
    CHECK(offsetof(VlrHeader, reserved)       == 0);
    CHECK(offsetof(VlrHeader, user_id)        == 2);
    CHECK(offsetof(VlrHeader, record_id)      == 18);
    CHECK(offsetof(VlrHeader, record_length)  == 20);
    CHECK(offsetof(VlrHeader, description)    == 22);

    // --- EvlrHeader (60 bytes) ---
    CHECK(sizeof(EvlrHeader) == 60);
    CHECK(offsetof(EvlrHeader, reserved)       == 0);
    CHECK(offsetof(EvlrHeader, user_id)        == 2);
    CHECK(offsetof(EvlrHeader, record_id)      == 18);
    CHECK(offsetof(EvlrHeader, record_length)  == 20);
    CHECK(offsetof(EvlrHeader, description)    == 28);

    // --- PointCore0 (20 bytes) ---
    CHECK(sizeof(PointCore0) == 20);
    CHECK(offsetof(PointCore0, x)                == 0);
    CHECK(offsetof(PointCore0, y)                == 4);
    CHECK(offsetof(PointCore0, z)                == 8);
    CHECK(offsetof(PointCore0, intensity)        == 12);
    CHECK(offsetof(PointCore0, flags)            == 14);
    CHECK(offsetof(PointCore0, classification)   == 15);
    CHECK(offsetof(PointCore0, scan_angle_rank)  == 16);
    CHECK(offsetof(PointCore0, user_data)        == 17);
    CHECK(offsetof(PointCore0, point_source_id)  == 18);

    // --- PointCore6 (30 bytes) ---
    CHECK(sizeof(PointCore6) == 30);
    CHECK(offsetof(PointCore6, x)                == 0);
    CHECK(offsetof(PointCore6, y)                == 4);
    CHECK(offsetof(PointCore6, z)                == 8);
    CHECK(offsetof(PointCore6, intensity)        == 12);
    CHECK(offsetof(PointCore6, flags)            == 14);
    CHECK(offsetof(PointCore6, classification)   == 16);
    CHECK(offsetof(PointCore6, user_data)        == 17);
    CHECK(offsetof(PointCore6, scan_angle)       == 18);
    CHECK(offsetof(PointCore6, point_source_id)  == 20);
    CHECK(offsetof(PointCore6, gps_time)         == 22);

    // --- Component structs ---
    CHECK(sizeof(GpsTime)    == 8);
    CHECK(sizeof(Rgb)        == 6);
    CHECK(sizeof(Nir)        == 2);
    CHECK(sizeof(WavePacket) == 29);

    // --- kMinPointSize ---
    CHECK(kMinPointSize[0]  == 20);
    CHECK(kMinPointSize[1]  == 28);
    CHECK(kMinPointSize[2]  == 26);
    CHECK(kMinPointSize[3]  == 34);
    CHECK(kMinPointSize[4]  == 57);
    CHECK(kMinPointSize[5]  == 63);
    CHECK(kMinPointSize[6]  == 30);
    CHECK(kMinPointSize[7]  == 36);
    CHECK(kMinPointSize[8]  == 38);
    CHECK(kMinPointSize[9]  == 59);
    CHECK(kMinPointSize[10] == 67);

    // --- ErrorCode ---
    CHECK(error_message(ErrorCode::Ok) != nullptr);
    CHECK(error_message(ErrorCode::InvalidSignature) != nullptr);

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}
