#pragma once
#include "las_types.h"
#include "las_error.h"
#include <cstddef>

namespace HLas::detail {

struct ParsedHeader {
    // Version
    uint8_t  version_major;
    uint8_t  version_minor;

    // Identification
    uint16_t file_source_id;
    uint16_t global_encoding;
    uint8_t  guid[16];
    char     system_id[32];
    char     generating_software[32];
    uint16_t creation_day;
    uint16_t creation_year;

    // Layout
    uint16_t header_size;
    uint32_t offset_to_point_data;
    uint32_t num_vlrs;
    uint8_t  point_format_id;        // 0–10, LAZ bit stripped
    uint16_t point_record_length;
    bool     is_laz;

    // Point counts — always 64-bit (normalized from legacy if needed)
    uint64_t point_count;
    uint64_t points_by_return[15];

    // Scale / offset / bounds
    double x_scale, y_scale, z_scale;
    double x_offset, y_offset, z_offset;
    double max_x, min_x;
    double max_y, min_y;
    double max_z, min_z;

    // 1.3+
    uint64_t start_of_waveform_data;

    // 1.4
    uint64_t start_of_first_evlr;
    uint32_t num_evlrs;
};

ErrorCode parse_header(const uint8_t* buffer, size_t buffer_size,
                       ParsedHeader& out);

} // namespace HLas::detail
