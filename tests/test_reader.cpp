#include "HLas.h"
#include "test_data.h"
#include <cstdio>
#include <cstring>
#include <cmath>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

int test_reader() {
    int fails = 0;
    printf("test_reader:\n");

    // 2. File not found — kräver ingen testdata
    {
        HLas::Reader r;
        HLas::Status s = r.open("nonexistent.las");
        CHECK(s == HLas::Status::FileNotFound);
        CHECK(!r.is_open());
    }

    // 4. status_message
    {
        CHECK(HLas::status_message(HLas::Status::Ok) != nullptr);
        CHECK(HLas::status_message(HLas::Status::FileNotFound) != nullptr);
    }

    if (!testdata::exists(testdata::kLas)) {
        printf("  SKIP (resten) — %s saknas\n", testdata::kLas);
        return fails;
    }

    constexpr uint64_t N   = testdata::kPointCount;
    constexpr uint16_t LEN = testdata::kRecLen12;

    // 1. Open real LAS file
    {
        HLas::Reader r;
        CHECK(!r.is_open());

        HLas::Status s = r.open(testdata::kLas);
        CHECK(s == HLas::Status::Ok);
        CHECK(r.is_open());
        CHECK(r.last_error().empty());

        const auto& h = r.header();
        CHECK(h.version_major == 1);
        CHECK(h.version_minor == 2);
        CHECK(h.point_format == 3);
        CHECK(h.point_record_length == LEN);
        CHECK(h.point_count == N);
        CHECK(std::fabs(h.x_scale - testdata::kScale) < 1e-12);
        CHECK(!h.is_laz);
        // autzen.las skrivs utan VLR:er — de finns i .laz-varianten
        CHECK(h.num_vlrs == 0);
        CHECK(r.vlr_count() == 0);

        // Point access
        const uint8_t* p0 = r.point_data(0);
        CHECK(p0 != nullptr);
        CHECK(r.point_data(N) == nullptr);

        // get_xyz
        double x, y, z;
        r.get_xyz(0, x, y, z);
        CHECK(x >= h.min_x && x <= h.max_x);
        CHECK(y >= h.min_y && y <= h.max_y);
        CHECK(z >= h.min_z && z <= h.max_z);

        // get_xyz_batch
        double bx[10], by[10], bz[10];
        uint64_t n = r.get_xyz_batch(0, 10, bx, by, bz);
        CHECK(n == 10);
        CHECK(std::fabs(bx[0] - x) < 1e-12);
        CHECK(std::fabs(by[0] - y) < 1e-12);
        CHECK(std::fabs(bz[0] - z) < 1e-12);

        // Batch at end
        n = r.get_xyz_batch(N - 5, 10, bx, by, bz);
        CHECK(n == 5);

        // read_points batch
        uint8_t buf[LEN * 3];
        uint64_t rn = r.read_points(0, 3, buf);
        CHECK(rn == 3);
        CHECK(std::memcmp(buf, p0, LEN) == 0);
    }

    // 1b. VLR-åtkomst via det publika API:t — LAZ-filen har 5 stycken.
    //     Kräver no_cache, alltså en riktig LAZ-avkodare.
#ifdef HLAS_HAVE_LAZ
    if (testdata::exists(testdata::kLaz)) {
        HLas::Reader r;
        CHECK(r.open(testdata::kLaz) == HLas::Status::Ok);
        CHECK(r.header().is_laz);
        CHECK(r.vlr_count() == 5);
        HLas::VlrInfo vi = r.vlr(0);
        CHECK(vi.data_size > 0);
        CHECK(vi.data != nullptr);
    }
#endif

    // 3. Move semantics
    {
        HLas::Reader a;
        a.open(testdata::kLas);
        CHECK(a.is_open());

        HLas::Reader b(std::move(a));
        CHECK(b.is_open());
        CHECK(b.header().point_count == N);
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}
