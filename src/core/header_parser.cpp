#include "header_parser.h"
#include <cstring>

namespace HLas::detail {

ErrorCode parse_header(const uint8_t* buffer, size_t buffer_size,
                       ParsedHeader& out) {
    std::memset(&out, 0, sizeof(out));

    // 1. Minimum size check
    if (buffer_size < sizeof(LasHeaderBase))
        return ErrorCode::InvalidHeaderSize;

    // 2. Read base header (227 bytes)
    LasHeaderBase base;
    std::memcpy(&base, buffer, sizeof(base));

    // 3. Validate signature
    if (std::memcmp(base.file_signature, "LASF", 4) != 0)
        return ErrorCode::InvalidSignature;

    // 4. Validate version
    if (base.version_major != 1 ||
        base.version_minor < 1 || base.version_minor > 4)
        return ErrorCode::UnsupportedVersion;

    // 5. Validate header size
    if (base.version_minor <= 2) {
        if (base.header_size < 227)
            return ErrorCode::InvalidHeaderSize;
    } else if (base.version_minor == 3) {
        if (base.header_size < 235)
            return ErrorCode::InvalidHeaderSize;
    } else { // 1.4
        if (base.header_size != 375)
            return ErrorCode::InvalidHeaderSize;
    }

    if (buffer_size < base.header_size)
        return ErrorCode::InvalidHeaderSize;

    // 6. LAZ detection
    uint8_t format = base.point_format_id;
    bool is_laz = (format & 0x80) != 0;
    format &= 0x7F;

    // 7. Validate point format
    if (format > 10)
        return ErrorCode::UnsupportedPointFormat;

    if (format >= 6 && base.version_minor < 4)
        return ErrorCode::UnsupportedPointFormat;

    if ((format == 4 || format == 5) && base.version_minor < 3)
        return ErrorCode::UnsupportedPointFormat;

    if ((format == 2 || format == 3) && base.version_minor < 2)
        return ErrorCode::UnsupportedPointFormat;

    // 8. Validate point record length
    if (base.point_record_length < kMinPointSize[format])
        return ErrorCode::InvalidPointRecordLength;

    // 9. Read 1.3 extension
    LasHeader13Ext ext13{};
    if (base.version_minor >= 3 && buffer_size >= 235)
        std::memcpy(&ext13, buffer + 227, sizeof(ext13));

    // 10. Read 1.4 extension
    LasHeader14Ext ext14{};
    if (base.version_minor >= 4 && buffer_size >= 375)
        std::memcpy(&ext14, buffer + 235, sizeof(ext14));

    // 11. Populate ParsedHeader
    out.version_major = base.version_major;
    out.version_minor = base.version_minor;
    out.file_source_id = base.file_source_id;
    out.global_encoding = base.global_encoding;

    // Assemble GUID from parts
    std::memcpy(out.guid,      &base.guid_data_1, 4);
    std::memcpy(out.guid + 4,  &base.guid_data_2, 2);
    std::memcpy(out.guid + 6,  &base.guid_data_3, 2);
    std::memcpy(out.guid + 8,   base.guid_data_4, 8);

    std::memcpy(out.system_id, base.system_identifier, 32);
    std::memcpy(out.generating_software, base.generating_software, 32);
    out.creation_day = base.creation_day;
    out.creation_year = base.creation_year;

    out.header_size = base.header_size;
    out.offset_to_point_data = base.offset_to_point_data;
    out.num_vlrs = base.num_vlrs;
    out.point_format_id = format;
    out.point_record_length = base.point_record_length;
    out.is_laz = is_laz;

    out.x_scale = base.x_scale;
    out.y_scale = base.y_scale;
    out.z_scale = base.z_scale;
    out.x_offset = base.x_offset;
    out.y_offset = base.y_offset;
    out.z_offset = base.z_offset;
    out.max_x = base.max_x;  out.min_x = base.min_x;
    out.max_y = base.max_y;  out.min_y = base.min_y;
    out.max_z = base.max_z;  out.min_z = base.min_z;

    out.start_of_waveform_data = ext13.start_of_waveform_data;
    out.start_of_first_evlr = ext14.start_of_first_evlr;
    out.num_evlrs = ext14.num_evlrs;

    // 12. Normalize point counts to 64-bit
    if (base.version_minor >= 4) {
        // 1.4: prefer 64-bit fields
        if (ext14.point_count != 0 || format >= 6) {
            out.point_count = ext14.point_count;
            std::memcpy(out.points_by_return, ext14.points_by_return,
                        15 * sizeof(uint64_t));
        } else {
            // 1.4 with format < 6 and 64-bit count == 0: use legacy
            out.point_count = base.legacy_point_count;
            for (int i = 0; i < 5; ++i)
                out.points_by_return[i] = base.legacy_points_by_return[i];
        }
    } else {
        // 1.1–1.3: legacy 32-bit only
        out.point_count = base.legacy_point_count;
        for (int i = 0; i < 5; ++i)
            out.points_by_return[i] = base.legacy_points_by_return[i];
    }

    return ErrorCode::Ok;
}

} // namespace HLas::detail
