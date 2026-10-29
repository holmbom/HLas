#include "point_reader.h"
#include "header_parser.h"
#include "file_io.h"
#include "test_data.h"
#include <cstdio>
#include <cstring>

using namespace HLas::detail;

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

int test_point_reader() {
    int fails = 0;
    printf("test_point_reader:\n");

    // 1. Real file — format 3, 34 bytes/point
    {
        constexpr uint64_t N   = testdata::kPointCount;
        constexpr uint16_t LEN = testdata::kRecLen12;

        MappedFile f;
        f.open(testdata::kLas);
        if (f.is_open()) {
            ParsedHeader h;
            parse_header(f.data(), f.size(), h);

            PointReader pr;
            ErrorCode err = pr.init(h, f.data(), f.size());
            CHECK(err == ErrorCode::Ok);
            CHECK(pr.count() == N);
            CHECK(pr.record_length() == LEN);
            CHECK(pr.format() == 3);
            CHECK(pr.extra_bytes_size() == 0);

            // First point
            const uint8_t* p0 = pr.raw_point(0);
            CHECK(p0 != nullptr);

            // Last point
            const uint8_t* pn = pr.raw_point(N - 1);
            CHECK(pn != nullptr);

            // Out of range
            CHECK(pr.raw_point(N) == nullptr);

            // Batch read first 10 points
            uint8_t buf[LEN * 10];
            uint64_t read = pr.read_raw(0, 10, buf);
            CHECK(read == 10);
            // First point in batch should match raw_point(0)
            CHECK(std::memcmp(buf, p0, LEN) == 0);

            // Batch at end — partial read
            read = pr.read_raw(N - 5, 10, buf);
            CHECK(read == 5);

            // Batch past end
            read = pr.read_raw(N, 10, buf);
            CHECK(read == 0);

            // Verify XYZ from first point (raw int32)
            int32_t x, y, z;
            std::memcpy(&x, p0, 4);
            std::memcpy(&y, p0 + 4, 4);
            std::memcpy(&z, p0 + 8, 4);
            double dx = x * h.x_scale + h.x_offset;
            double dy = y * h.y_scale + h.y_offset;
            double dz = z * h.z_scale + h.z_offset;
            // Should be within file bounds
            CHECK(dx >= h.min_x && dx <= h.max_x);
            CHECK(dy >= h.min_y && dy <= h.max_y);
            CHECK(dz >= h.min_z && dz <= h.max_z);
        } else {
            printf("  SKIP (riktig fil) — %s saknas\n", testdata::kLas);
        }
    }

    // 2. Extra bytes detection
    {
        // Fake a header with record_length > min size
        ParsedHeader h{};
        h.version_major = 1;
        h.version_minor = 2;
        h.point_format_id = 0;
        h.point_record_length = 24;  // 20 + 4 extra
        h.point_count = 1;
        h.offset_to_point_data = 0;

        uint8_t buf[24] = {};
        PointReader pr;
        ErrorCode err = pr.init(h, buf, sizeof(buf));
        CHECK(err == ErrorCode::Ok);
        CHECK(pr.extra_bytes_size() == 4);
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}
