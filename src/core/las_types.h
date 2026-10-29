#pragma once
#include <bit>
#include <cstdint>
#include <cstddef>

namespace HLas::detail {

// LAS och LAZ är little-endian i varje fält, och hela HLas bygger på det:
// läsaren memcpy:ar rakt ur den mmap:ade filen och skrivaren rakt in i den.
// Att byteväxla på ett ställe och inte det andra vore teater, så kravet ställs
// i stället uttalat och på ett ställe — den här headern dras in av
// header_parser, point_reader och vlr_parser, alltså av varje bygge.
//
// Ett big-endian-bygge ska falla här, med en förklaring, i stället för att tyst
// läsa och skriva filer ingen kan använda.
static_assert(std::endian::native == std::endian::little,
              "HLas läser och skriver LAS/LAZ med värdmaskinens byteordning och "
              "kräver därför en little-endian plattform");

#pragma pack(push, 1)

// Public Header Block — common to LAS 1.1–1.4 (bytes 0–226, 227 bytes)
struct LasHeaderBase {
    char     file_signature[4];        // "LASF"
    uint16_t file_source_id;
    uint16_t global_encoding;
    uint32_t guid_data_1;
    uint16_t guid_data_2;
    uint16_t guid_data_3;
    uint8_t  guid_data_4[8];
    uint8_t  version_major;
    uint8_t  version_minor;
    char     system_identifier[32];
    char     generating_software[32];
    uint16_t creation_day;
    uint16_t creation_year;
    uint16_t header_size;
    uint32_t offset_to_point_data;
    uint32_t num_vlrs;
    uint8_t  point_format_id;
    uint16_t point_record_length;
    uint32_t legacy_point_count;
    uint32_t legacy_points_by_return[5];
    double   x_scale, y_scale, z_scale;
    double   x_offset, y_offset, z_offset;
    double   max_x, min_x;
    double   max_y, min_y;
    double   max_z, min_z;
};
static_assert(sizeof(LasHeaderBase) == 227);

// LAS 1.3 extension (bytes 227–234)
struct LasHeader13Ext {
    uint64_t start_of_waveform_data;
};
static_assert(sizeof(LasHeader13Ext) == 8);

// LAS 1.4 extension (bytes 235–374)
struct LasHeader14Ext {
    uint64_t start_of_first_evlr;
    uint32_t num_evlrs;
    uint64_t point_count;
    uint64_t points_by_return[15];
};
static_assert(sizeof(LasHeader14Ext) == 140);

// Variable Length Record header (54 bytes)
struct VlrHeader {
    uint16_t reserved;
    char     user_id[16];
    uint16_t record_id;
    uint16_t record_length;
    char     description[32];
};
static_assert(sizeof(VlrHeader) == 54);

// Extended Variable Length Record header (60 bytes, LAS 1.3+)
struct EvlrHeader {
    uint16_t reserved;
    char     user_id[16];
    uint16_t record_id;
    uint64_t record_length;
    char     description[32];
};
static_assert(sizeof(EvlrHeader) == 60);

// Point Data Record core — formats 0–5 (20 bytes)
struct PointCore0 {
    int32_t  x, y, z;
    uint16_t intensity;
    uint8_t  flags;            // return_num:3, num_returns:3, scan_dir:1, edge:1
    uint8_t  classification;
    int8_t   scan_angle_rank;
    uint8_t  user_data;
    uint16_t point_source_id;
};
static_assert(sizeof(PointCore0) == 20);

// Point Data Record core — formats 6–10 (30 bytes)
struct PointCore6 {
    int32_t  x, y, z;
    uint16_t intensity;
    uint16_t flags;            // return_num:4, num_returns:4, class_flags:4, scanner_ch:2, scan_dir:1, edge:1
    uint8_t  classification;
    uint8_t  user_data;
    int16_t  scan_angle;
    uint16_t point_source_id;
    double   gps_time;
};
static_assert(sizeof(PointCore6) == 30);

struct GpsTime {
    double gps_time;
};
static_assert(sizeof(GpsTime) == 8);

struct Rgb {
    uint16_t r, g, b;
};
static_assert(sizeof(Rgb) == 6);

struct Nir {
    uint16_t nir;
};
static_assert(sizeof(Nir) == 2);

struct WavePacket {
    uint8_t  descriptor_index;
    uint64_t byte_offset;
    uint32_t packet_size;
    float    return_point_location;
    float    dx, dy, dz;
};
static_assert(sizeof(WavePacket) == 29);

#pragma pack(pop)

// Minimum point record sizes per format (0–10)
constexpr uint16_t kMinPointSize[] = {
    20, 28, 26, 34, 57, 63,   // formats 0–5
    30, 36, 38, 59, 67         // formats 6–10
};

} // namespace HLas::detail
