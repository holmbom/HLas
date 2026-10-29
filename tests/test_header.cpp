#include "header_parser.h"
#include "file_io.h"
#include "test_data.h"
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace HLas::detail;

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

// Build a minimal LAS 1.2 header (227 bytes)
static void make_header_12(uint8_t* buf, uint8_t format, uint16_t rec_len,
                           uint32_t point_count) {
    std::memset(buf, 0, 227);
    std::memcpy(buf, "LASF", 4);           // signature
    buf[24] = 1;                            // version_major
    buf[25] = 2;                            // version_minor
    uint16_t hdr_size = 227;
    std::memcpy(buf + 94, &hdr_size, 2);    // header_size
    uint32_t offset = 227;
    std::memcpy(buf + 96, &offset, 4);      // offset_to_point_data
    buf[104] = format;                      // point_format_id
    std::memcpy(buf + 105, &rec_len, 2);    // point_record_length
    std::memcpy(buf + 107, &point_count, 4);// legacy_point_count
    // Scale factors
    double scale = 0.001;
    std::memcpy(buf + 131, &scale, 8);      // x_scale
    std::memcpy(buf + 139, &scale, 8);      // y_scale
    std::memcpy(buf + 147, &scale, 8);      // z_scale
}

// Build a minimal LAS 1.4 header (375 bytes)
static void make_header_14(uint8_t* buf, uint8_t format, uint16_t rec_len,
                           uint64_t point_count) {
    std::memset(buf, 0, 375);
    std::memcpy(buf, "LASF", 4);
    buf[24] = 1;
    buf[25] = 4;
    uint16_t hdr_size = 375;
    std::memcpy(buf + 94, &hdr_size, 2);
    uint32_t offset = 375;
    std::memcpy(buf + 96, &offset, 4);
    buf[104] = format;
    std::memcpy(buf + 105, &rec_len, 2);
    // 1.4 point count at offset 247
    std::memcpy(buf + 247, &point_count, 8);
    double scale = 0.01;
    std::memcpy(buf + 131, &scale, 8);
    std::memcpy(buf + 139, &scale, 8);
    std::memcpy(buf + 147, &scale, 8);
}

int test_header() {
    int fails = 0;
    printf("test_header:\n");

    // 1. Synthetic LAS 1.2 format 2
    {
        uint8_t buf[227];
        make_header_12(buf, 2, 26, 1000);
        ParsedHeader h;
        ErrorCode err = parse_header(buf, sizeof(buf), h);
        CHECK(err == ErrorCode::Ok);
        CHECK(h.version_major == 1);
        CHECK(h.version_minor == 2);
        CHECK(h.point_format_id == 2);
        CHECK(h.point_record_length == 26);
        CHECK(h.point_count == 1000);
        CHECK(!h.is_laz);
        CHECK(std::fabs(h.x_scale - 0.001) < 1e-12);
    }

    // 2. Synthetic LAS 1.4 format 6
    {
        uint8_t buf[375];
        make_header_14(buf, 6, 30, 5000000000ULL);
        ParsedHeader h;
        ErrorCode err = parse_header(buf, sizeof(buf), h);
        CHECK(err == ErrorCode::Ok);
        CHECK(h.version_minor == 4);
        CHECK(h.point_format_id == 6);
        CHECK(h.point_count == 5000000000ULL);
        CHECK(h.header_size == 375);
    }

    // 3. LAZ detection
    {
        uint8_t buf[227];
        make_header_12(buf, 0x82, 26, 100);  // format 2 + LAZ bit
        ParsedHeader h;
        ErrorCode err = parse_header(buf, sizeof(buf), h);
        CHECK(err == ErrorCode::Ok);
        CHECK(h.is_laz);
        CHECK(h.point_format_id == 2);
    }

    // 4. Real file — okomprimerad LAS 1.2 format 3
    if (testdata::exists(testdata::kLas)) {
        MappedFile f;
        ErrorCode err = f.open(testdata::kLas);
        CHECK(err == ErrorCode::Ok);
        if (f.is_open()) {
            ParsedHeader h;
            err = parse_header(f.data(), f.size(), h);
            CHECK(err == ErrorCode::Ok);
            CHECK(h.version_major == 1);
            CHECK(h.version_minor == 2);
            CHECK(h.point_format_id == 3);
            CHECK(h.point_record_length == testdata::kRecLen12);
            CHECK(h.point_count == testdata::kPointCount);
            CHECK(std::fabs(h.x_scale - testdata::kScale) < 1e-12);
            CHECK(std::fabs(h.y_scale - testdata::kScale) < 1e-12);
            CHECK(h.num_vlrs == 0);
            CHECK(h.offset_to_point_data == 227);
            CHECK(!h.is_laz);
        }
    } else {
        printf("  SKIP (riktig fil) — %s saknas\n", testdata::kLas);
    }

    // 4b. Real file — LAZ 1.2, samma punkter, LAZ-biten satt
    if (testdata::exists(testdata::kLaz)) {
        MappedFile f;
        CHECK(f.open(testdata::kLaz) == ErrorCode::Ok);
        if (f.is_open()) {
            ParsedHeader h;
            CHECK(parse_header(f.data(), f.size(), h) == ErrorCode::Ok);
            CHECK(h.is_laz);
            CHECK(h.point_format_id == 3);
            CHECK(h.point_count == testdata::kPointCount);
            CHECK(h.num_vlrs == 5);
        }
    }

    // 4c. Real file — COPC LAS 1.4 format 7
    if (testdata::exists(testdata::kCopc)) {
        MappedFile f;
        CHECK(f.open(testdata::kCopc) == ErrorCode::Ok);
        if (f.is_open()) {
            ParsedHeader h;
            CHECK(parse_header(f.data(), f.size(), h) == ErrorCode::Ok);
            CHECK(h.version_minor == 4);
            CHECK(h.point_format_id == 7);
            CHECK(h.point_record_length == testdata::kRecLen14);
            CHECK(h.point_count == testdata::kPointCount);
            CHECK(h.header_size == 375);
            CHECK(h.is_laz);
        }
    }

    // 5. Bad signature
    {
        uint8_t buf[227];
        make_header_12(buf, 0, 20, 100);
        std::memcpy(buf, "NOPE", 4);
        ParsedHeader h;
        CHECK(parse_header(buf, sizeof(buf), h) == ErrorCode::InvalidSignature);
    }

    // 6. Unsupported version 2.0
    {
        uint8_t buf[227];
        make_header_12(buf, 0, 20, 100);
        buf[24] = 2;  // version_major = 2
        ParsedHeader h;
        CHECK(parse_header(buf, sizeof(buf), h) == ErrorCode::UnsupportedVersion);
    }

    // 7. Truncated buffer
    {
        uint8_t buf[100];
        std::memset(buf, 0, sizeof(buf));
        ParsedHeader h;
        CHECK(parse_header(buf, sizeof(buf), h) == ErrorCode::InvalidHeaderSize);
    }

    // 8. Format 6 in version 1.2
    {
        uint8_t buf[227];
        make_header_12(buf, 6, 30, 100);
        ParsedHeader h;
        CHECK(parse_header(buf, sizeof(buf), h) == ErrorCode::UnsupportedPointFormat);
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}
