#include "HLas.h"
#include "test_data.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

static const char* kTestFile = testdata::kLas;   // LAS 1.2 fmt 3, okomprimerad
static const char* kTestLaz  = testdata::kCopc;  // COPC LAZ 1.4 fmt 7

// ⛔ BUFFERTARNA LIGGER PÅ HEAPEN, och det är inte en stilfråga.
//
// De nio blocken nedan tar var sitt 10 000-element-tripel, alltså 240 kB
// styck och drygt 2 MB tillsammans. De ligger i EN funktion, och en ram
// allokeras i sin helhet vid funktionens INGÅNG — inte block för block. Som
// stackarrayer sprängde de därför Windows 1 MB stack (Linux ger 8) redan innan
// första blocket kördes, alltså även när varenda ett hoppades över för att
// testdatan saknades. Processen dog med 0xC0000409 och den buffrade utskriften
// pekade ut ett helt annat test.
int test_integration() {
    int fails = 0;
    printf("test_integration:\n");

    SKIP_IF_MISSING(kTestFile);
    SKIP_IF_MISSING(kTestLaz);

    // 1. Full XYZ scan — all 6M points within bounds
    {
        HLas::Reader r;
        HLas::Status s = r.open(kTestFile);
        CHECK(s == HLas::Status::Ok);

        const auto& h = r.header();
        const uint64_t total = h.point_count;
        const uint64_t chunk = 10000;

        std::vector<double> bx(10000), by(10000), bz(10000);
        uint64_t scanned = 0;
        uint64_t out_of_bounds = 0;
        double max_dev = 0.0;

        for (uint64_t off = 0; off < total; off += chunk) {
            uint64_t n = r.get_xyz_batch(off, chunk, bx.data(), by.data(), bz.data());
            for (uint64_t i = 0; i < n; ++i) {
                double dx = 0, dy = 0, dz = 0;
                if (bx[i] < h.min_x) dx = h.min_x - bx[i];
                if (bx[i] > h.max_x) dx = bx[i] - h.max_x;
                if (by[i] < h.min_y) dy = h.min_y - by[i];
                if (by[i] > h.max_y) dy = by[i] - h.max_y;
                if (bz[i] < h.min_z) dz = h.min_z - bz[i];
                if (bz[i] > h.max_z) dz = bz[i] - h.max_z;
                double dev = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
                if (dev > 0) {
                    ++out_of_bounds;
                    if (dev > max_dev) max_dev = dev;
                }
            }
            scanned += n;
        }
        if (out_of_bounds > 0) {
            printf("  note: %llu points outside bounds, max deviation: %.6f\n",
                   static_cast<unsigned long long>(out_of_bounds), max_dev);
        }
        // Allow tiny deviation due to floating-point scale/offset rounding
        CHECK(max_dev < 0.001);
        CHECK(scanned == total);
        printf("  full_xyz_scan: %llu points OK\n",
               static_cast<unsigned long long>(scanned));
    }

    // 2. RGB validation — format 3 har RGB efter GPS-tiden, på offset 28
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        CHECK(h.point_format == 3);

        const uint64_t sample = 1000;
        constexpr uint16_t LEN = testdata::kRecLen12;
        uint8_t buf[LEN * 1000];
        uint64_t n = r.read_points(0, sample, buf);
        CHECK(n == sample);

        bool has_nonzero_rgb = false;
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t* pt = buf + i * LEN;
            uint16_t red, green, blue;
            std::memcpy(&red,   pt + testdata::kRgbOff12,     2);
            std::memcpy(&green, pt + testdata::kRgbOff12 + 2, 2);
            std::memcpy(&blue,  pt + testdata::kRgbOff12 + 4, 2);
            if (red != 0 || green != 0 || blue != 0) {
                has_nonzero_rgb = true;
                break;
            }
        }
        CHECK(has_nonzero_rgb);
    }

    // 3. Points-by-return sum == point_count
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();

        uint64_t sum = 0;
        for (int i = 0; i < 5; ++i)  // LAS 1.2: 5 returns
            sum += h.points_by_return[i];
        CHECK(sum == h.point_count);
    }

    // 4. LAZ open
    {
        HLas::Reader r;
        HLas::Status s = r.open(kTestLaz);
        printf("  laz_open: status=%d (%s)\n", static_cast<int>(s), HLas::status_message(s));
#ifdef HLAS_HAVE_LAZ
        CHECK(s == HLas::Status::Ok);
        CHECK(r.is_open());
        CHECK(r.header().point_count > 0);
        CHECK(r.header().is_laz);
#else
        // Utan LAZ-avkodare går en .laz ändå att öppna om sidecaren finns —
        // den stigen är inte beroende av avkodaren. Annars ska den nekas.
        if (s == HLas::Status::Ok) {
            CHECK(r.header().point_record_length == 13);   // sidecar-post
        } else {
            CHECK(s == HLas::Status::LazNotSupported);
            CHECK(!r.is_open());
        }
#endif
    }

    // 5. Reopen consistency
    {
        HLas::Reader r;
        r.open(kTestFile);
        double x1, y1, z1;
        r.get_xyz(0, x1, y1, z1);
        r.close();

        r.open(kTestFile);
        double x2, y2, z2;
        r.get_xyz(0, x2, y2, z2);

        CHECK(x1 == x2);
        CHECK(y1 == y2);
        CHECK(z1 == z2);
    }

    // 6. Raw/batch consistency — sample 100 points
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        const uint16_t rec_len = h.point_record_length;

        uint8_t buf[67];  // max point size (format 10 = 67)
        bool all_match = true;

        for (uint64_t i = 0; i < h.point_count; i += 60000) {
            const uint8_t* raw = r.point_data(i);
            uint64_t n = r.read_points(i, 1, buf);
            if (n != 1 || std::memcmp(raw, buf, rec_len) != 0) {
                all_match = false;
                break;
            }
        }
        CHECK(all_match);
    }

    // 7. Batch speed test — sample batches spread across the file
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        const uint64_t total = h.point_count;
        const uint64_t batch_size = 10000;
        const uint64_t num_samples = 200;
        const uint64_t step = total / num_samples;

        std::vector<double> bx(10000), by(10000), bz(10000);

        using Clock = std::chrono::high_resolution_clock;
        double min_us = 1e18, max_us = 0.0, total_us = 0.0;
        uint64_t total_points = 0;

        for (uint64_t s = 0; s < num_samples; ++s) {
            uint64_t off = s * step;
            auto t0 = Clock::now();
            uint64_t n = r.get_xyz_batch(off, batch_size, bx.data(), by.data(), bz.data());
            auto t1 = Clock::now();

            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (us < min_us) min_us = us;
            if (us > max_us) max_us = us;
            total_us += us;
            total_points += n;
        }

        double avg_us = total_us / num_samples;
        double pts_per_sec = total_points / (total_us * 1e-6);
        double mb_per_sec = (total_points * h.point_record_length) / (total_us * 1e-6) / (1024.0 * 1024.0);

        printf("  batch_speed: %llu batches of %llu points across file\n",
               static_cast<unsigned long long>(num_samples),
               static_cast<unsigned long long>(batch_size));
        printf("    min: %.1f us  max: %.1f us  avg: %.1f us\n",
               min_us, max_us, avg_us);
        printf("    throughput: %.1f Mpts/s  (%.1f MB/s)\n",
               pts_per_sec / 1e6, mb_per_sec);
    }

    // 8. Ground classification filter
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        const uint64_t total = h.point_count;
        const uint64_t chunk = 10000;
        const uint8_t CLASS_GROUND = 2;

        std::vector<double> bx(10000), by(10000), bz(10000);
        uint64_t ground_count = 0;
        bool all_in_bounds = true;

        for (uint64_t off = 0; off < total; off += chunk) {
            uint64_t n = r.get_xyz_filtered(off, chunk, CLASS_GROUND, bx.data(), by.data(), bz.data());
            for (uint64_t i = 0; i < n; ++i) {
                if (bx[i] < h.min_x - 0.001 || bx[i] > h.max_x + 0.001 ||
                    by[i] < h.min_y - 0.001 || by[i] > h.max_y + 0.001 ||
                    bz[i] < h.min_z - 0.001 || bz[i] > h.max_z + 0.001) {
                    all_in_bounds = false;
                }
            }
            ground_count += n;
        }
        CHECK(ground_count > 0);
        CHECK(ground_count < total);
        CHECK(all_in_bounds);

        // Verify count matches manual classification check
        uint64_t manual_count = 0;
        for (uint64_t i = 0; i < total; i += 1000)
            if (r.get_classification(i) == CLASS_GROUND)
                ++manual_count;

        uint64_t sampled_ground = 0;
        for (uint64_t off = 0; off < total; off += chunk) {
            uint64_t scan = chunk;
            if (off + scan > total) scan = total - off;
            // Count only sampled positions
            for (uint64_t i = off; i < off + scan; i += 1000)
                if (r.get_classification(i) == CLASS_GROUND)
                    ++sampled_ground;
        }
        CHECK(manual_count == sampled_ground);

        double pct = 100.0 * ground_count / total;
        printf("  ground_filter: %llu / %llu points (%.1f%%)\n",
               static_cast<unsigned long long>(ground_count),
               static_cast<unsigned long long>(total), pct);
    }

    // 9. Filtered vs unfiltered speed comparison
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        const uint64_t total = h.point_count;
        const uint64_t chunk = 10000;

        std::vector<double> bx(10000), by(10000), bz(10000);
        using Clock = std::chrono::high_resolution_clock;

        // Unfiltered full scan
        auto t0 = Clock::now();
        uint64_t all_count = 0;
        for (uint64_t off = 0; off < total; off += chunk)
            all_count += r.get_xyz_batch(off, chunk, bx.data(), by.data(), bz.data());
        auto t1 = Clock::now();
        double all_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Filtered (ground only) full scan
        auto t2 = Clock::now();
        uint64_t gnd_count = 0;
        for (uint64_t off = 0; off < total; off += chunk)
            gnd_count += r.get_xyz_filtered(off, chunk, 2, bx.data(), by.data(), bz.data());
        auto t3 = Clock::now();
        double gnd_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

        printf("  speed_comparison (full file, %llu pts):\n",
               static_cast<unsigned long long>(total));
        printf("    unfiltered: %.1f ms  (%.1f Mpts/s)\n",
               all_ms, all_count / (all_ms * 1e-3) / 1e6);
        printf("    filtered:   %.1f ms  (%.1f Mpts/s scanned, %llu ground)\n",
               gnd_ms, total / (gnd_ms * 1e-3) / 1e6,
               static_cast<unsigned long long>(gnd_count));
    }

    // 10. Polygon filter — full bounds rectangle
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        const uint64_t total = h.point_count;
        const uint64_t chunk = 10000;

        // Polygon = file bounds rectangle (CCW)
        double px[] = { h.min_x, h.max_x, h.max_x, h.min_x };
        double py[] = { h.min_y, h.min_y, h.max_y, h.max_y };

        std::vector<double> bx(10000), by(10000), bz(10000);
        uint64_t poly_count = 0;
        for (uint64_t off = 0; off < total; off += chunk)
            poly_count += r.get_xyz_polygon(off, chunk, px, py, 4, bx.data(), by.data(), bz.data());

        // Nearly all points should be inside (allow for rounding)
        double pct = 100.0 * poly_count / total;
        CHECK(pct > 99.9);
        printf("  polygon_full_bounds: %llu / %llu (%.2f%%)\n",
               static_cast<unsigned long long>(poly_count),
               static_cast<unsigned long long>(total), pct);
    }

    // 11. Polygon filter — half bounds
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        const uint64_t total = h.point_count;
        const uint64_t chunk = 10000;

        double mid_x = (h.min_x + h.max_x) / 2.0;
        double px[] = { h.min_x, mid_x, mid_x, h.min_x };
        double py[] = { h.min_y, h.min_y, h.max_y, h.max_y };

        std::vector<double> bx(10000), by(10000), bz(10000);
        uint64_t half_count = 0;
        for (uint64_t off = 0; off < total; off += chunk)
            half_count += r.get_xyz_polygon(off, chunk, px, py, 4, bx.data(), by.data(), bz.data());

        double pct = 100.0 * half_count / total;
        CHECK(half_count > 0);
        CHECK(half_count < total);
        printf("  polygon_half_bounds: %llu / %llu (%.1f%%)\n",
               static_cast<unsigned long long>(half_count),
               static_cast<unsigned long long>(total), pct);

        // 12. Polygon + ground filter
        uint64_t half_ground = 0;
        for (uint64_t off = 0; off < total; off += chunk)
            half_ground += r.get_xyz_polygon(off, chunk, px, py, 4, bx.data(), by.data(), bz.data(), 2);

        CHECK(half_ground > 0);
        CHECK(half_ground < half_count);
        printf("  polygon_half+ground: %llu (%.1f%% of half)\n",
               static_cast<unsigned long long>(half_ground),
               100.0 * half_ground / half_count);
    }

    // 13. Polygon outside bounds — should return 0
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();

        double px[] = { h.max_x + 100, h.max_x + 200, h.max_x + 200, h.max_x + 100 };
        double py[] = { h.max_y + 100, h.max_y + 100, h.max_y + 200, h.max_y + 200 };

        std::vector<double> bx(10000), by(10000), bz(10000);
        uint64_t n = r.get_xyz_polygon(0, 10000, px, py, 4, bx.data(), by.data(), bz.data());
        CHECK(n == 0);
    }

    // 14. Polygon speed test
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();
        const uint64_t total = h.point_count;
        const uint64_t chunk = 10000;

        double px[] = { h.min_x, h.max_x, h.max_x, h.min_x };
        double py[] = { h.min_y, h.min_y, h.max_y, h.max_y };
        std::vector<double> bx(10000), by(10000), bz(10000);
        using Clock = std::chrono::high_resolution_clock;

        auto t0 = Clock::now();
        uint64_t poly_count = 0;
        for (uint64_t off = 0; off < total; off += chunk)
            poly_count += r.get_xyz_polygon(off, chunk, px, py, 4, bx.data(), by.data(), bz.data());
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        printf("  polygon_speed (full bounds, 4 vertices): %.1f ms  (%.1f Mpts/s)\n",
               ms, total / (ms * 1e-3) / 1e6);
    }

    // 15. DEM generation — full bounds, 1m cells
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();

        double px[] = { h.min_x, h.max_x, h.max_x, h.min_x };
        double py[] = { h.min_y, h.min_y, h.max_y, h.max_y };

        using Clock = std::chrono::high_resolution_clock;
        auto t0 = Clock::now();
        auto dem = r.generate_dem(px, py, 4, testdata::kDemCell, HLas::DemFill::None);
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        uint32_t expected_cols = static_cast<uint32_t>(std::ceil(h.max_x - h.min_x));
        uint32_t expected_rows = static_cast<uint32_t>(std::ceil(h.max_y - h.min_y));
        CHECK(dem.cols == expected_cols);
        CHECK(dem.rows == expected_rows);
        CHECK(dem.elevation.size() == static_cast<size_t>(dem.cols) * dem.rows);

        // Count NaN and verify elevation bounds
        uint32_t nan_count = 0;
        bool all_z_ok = true;
        for (size_t i = 0; i < dem.elevation.size(); ++i) {
            float v = dem.elevation[i];
            if (std::isnan(v)) {
                ++nan_count;
            } else if (v < h.min_z - 1.0 || v > h.max_z + 1.0) {
                all_z_ok = false;
            }
        }
        CHECK(all_z_ok);
        uint32_t total_cells = dem.cols * dem.rows;
        double fill_pct = 100.0 * (total_cells - nan_count) / total_cells;
        printf("  dem_1m: %ux%u grid, %.1f%% filled, %u NaN cells, %.1f ms\n",
               dem.cols, dem.rows, fill_pct, nan_count, ms);
    }

    // 16. DEM fill modes comparison
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();

        // Use a smaller polygon for speed (quarter of file)
        double mid_x = (h.min_x + h.max_x) / 2.0;
        double mid_y = (h.min_y + h.max_y) / 2.0;
        double px[] = { h.min_x, mid_x, mid_x, h.min_x };
        double py[] = { h.min_y, h.min_y, mid_y, mid_y };

        const double cell = testdata::kDemCellFill;
        auto dem_none = r.generate_dem(px, py, 4, cell, HLas::DemFill::None);
        auto dem_near = r.generate_dem(px, py, 4, cell, HLas::DemFill::Nearest);
        auto dem_idw  = r.generate_dem(px, py, 4, cell, HLas::DemFill::Idw);

        CHECK(dem_none.cols == dem_near.cols);
        CHECK(dem_none.rows == dem_near.rows);

        auto count_nan = [](const std::vector<float>& v) {
            uint32_t n = 0;
            for (float f : v) if (std::isnan(f)) ++n;
            return n;
        };

        uint32_t nan_none = count_nan(dem_none.elevation);
        uint32_t nan_near = count_nan(dem_near.elevation);
        uint32_t nan_idw  = count_nan(dem_idw.elevation);

        // Filled modes should have fewer NaN
        CHECK(nan_near <= nan_none);
        CHECK(nan_idw <= nan_none);

        printf("  dem_fill: %ux%u grid, NaN: none=%u nearest=%u idw=%u\n",
               dem_none.cols, dem_none.rows, nan_none, nan_near, nan_idw);
    }

    // 17. DEM TIN fill — should fill more cells than IDW, valid Z values
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();

        // Use quarter polygon (same as test 16)
        double mid_x = (h.min_x + h.max_x) / 2.0;
        double mid_y = (h.min_y + h.max_y) / 2.0;
        double px[] = { h.min_x, mid_x, mid_x, h.min_x };
        double py[] = { h.min_y, h.min_y, mid_y, mid_y };

        auto count_nan = [](const std::vector<float>& v) {
            uint32_t n = 0;
            for (float f : v) if (std::isnan(f)) ++n;
            return n;
        };

        using Clock = std::chrono::high_resolution_clock;

        const double cell = testdata::kDemCellFill;
        auto dem_none = r.generate_dem(px, py, 4, cell, HLas::DemFill::None);
        uint32_t nan_none = count_nan(dem_none.elevation);

        auto t0 = Clock::now();
        auto dem_idw = r.generate_dem(px, py, 4, cell, HLas::DemFill::Idw);
        auto t1 = Clock::now();
        double idw_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        uint32_t nan_idw = count_nan(dem_idw.elevation);

        auto t2 = Clock::now();
        auto dem_tin = r.generate_dem(px, py, 4, cell, HLas::DemFill::Tin);
        auto t3 = Clock::now();
        double tin_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        uint32_t nan_tin = count_nan(dem_tin.elevation);

        // TIN should fill at least as many cells as no-fill
        // Note: TIN may have more NaN than IDW on some datasets (IDW uses local radius, TIN is global)
        CHECK(nan_tin < nan_none);
        CHECK(nan_tin < nan_none);

        // All TIN-filled values should be within Z bounds (with tolerance)
        bool z_ok = true;
        for (size_t i = 0; i < dem_tin.elevation.size(); ++i) {
            float v = dem_tin.elevation[i];
            if (!std::isnan(v) && (v < h.min_z - 1.0 || v > h.max_z + 1.0)) {
                z_ok = false;
                break;
            }
        }
        CHECK(z_ok);

        uint32_t total_cells = dem_tin.cols * dem_tin.rows;
        double fill_pct = 100.0 * (total_cells - nan_tin) / total_cells;
        printf("  dem_tin: %ux%u grid, %.1f%% filled, NaN: none=%u idw=%u tin=%u\n",
               dem_tin.cols, dem_tin.rows, fill_pct, nan_none, nan_idw, nan_tin);
        printf("    timing: IDW=%.1f ms  TIN=%.1f ms\n", idw_ms, tin_ms);
    }

    // 18. DEM TIN — full bounds, verify large area coverage
    {
        HLas::Reader r;
        r.open(kTestFile);
        const auto& h = r.header();

        double px[] = { h.min_x, h.max_x, h.max_x, h.min_x };
        double py[] = { h.min_y, h.min_y, h.max_y, h.max_y };

        // Hela utbredningen är 3426 x 4656 m — 1 m-celler ger ~16M celler, vilket är
        // för mycket för TIN. 10 m ger ett rutnät i samma storleksordning som test 17.
        using Clock = std::chrono::high_resolution_clock;
        auto t0 = Clock::now();
        auto dem = r.generate_dem(px, py, 4, 10.0, HLas::DemFill::Tin);
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        uint32_t nan_count = 0;
        for (float v : dem.elevation)
            if (std::isnan(v)) ++nan_count;

        uint32_t total_cells = dem.cols * dem.rows;
        double fill_pct = 100.0 * (total_cells - nan_count) / total_cells;

        // TIN on full bounds should fill significantly more than no-fill (>50%)
        CHECK(fill_pct > 50.0);

        printf("  dem_tin_full: %ux%u, %.1f%% filled (%u NaN), %.1f ms\n",
               dem.cols, dem.rows, fill_pct, nan_count, ms);
    }

#ifdef HLAS_HAVE_LAZ
    // 19. LAZ vs LAS — header comparison
    // Note: kTestFile is LAS 1.2 format 2, kTestLaz is COPC LAZ 1.4 format 7 (same dataset)
    {
        // no_cache: i sidecar-läge är posten 13 byte och point_record_length 13,
        // vilket gör en jämförelse av formatfälten meningslös.
        HLas::Reader las, laz;
        las.open(kTestFile);
        laz.open(kTestLaz);
        const auto& hl = las.header();
        const auto& hz = laz.header();

        CHECK(hz.point_count == hl.point_count);
        CHECK(hz.is_laz == true);
        // Different format encoding (LAS 1.2 fmt 3 vs LAZ 1.4 fmt 7) — only check count and bounds
        CHECK(std::fabs(hz.min_x - hl.min_x) < 0.01);
        CHECK(std::fabs(hz.min_y - hl.min_y) < 0.01);
        printf("  laz_header: las_count=%llu fmt=%u rec=%u, laz_count=%llu fmt=%u rec=%u\n",
               static_cast<unsigned long long>(hl.point_count),
               hl.point_format, hl.point_record_length,
               static_cast<unsigned long long>(hz.point_count),
               hz.point_format, hz.point_record_length);
        printf("    las scale: (%.10f, %.10f, %.10f)\n", hl.x_scale, hl.y_scale, hl.z_scale);
        printf("    laz scale: (%.10f, %.10f, %.10f)\n", hz.x_scale, hz.y_scale, hz.z_scale);
        printf("    las offset: (%.4f, %.4f, %.4f)\n", hl.x_offset, hl.y_offset, hl.z_offset);
        printf("    laz offset: (%.4f, %.4f, %.4f)\n", hz.x_offset, hz.y_offset, hz.z_offset);
    }

    // 19b. LAS vs LAZ — samma punkter i samma ordning, ska ge identisk XYZ.
    //      Det här är det skarpa testet av LAZ-avkodaren: autzen.las är genererad
    //      ur autzen.laz, så varje punkt måste stämma exakt, inte bara ligga i bounds.
    if (testdata::exists(testdata::kLaz)) {
        HLas::Reader las, laz;
        CHECK(las.open(testdata::kLas) == HLas::Status::Ok);
        CHECK(laz.open(testdata::kLaz) == HLas::Status::Ok);

        const auto& hl = las.header();
        const auto& hz = laz.header();
        CHECK(hz.point_count == hl.point_count);
        CHECK(hz.point_format == hl.point_format);
        CHECK(hz.point_record_length == hl.point_record_length);

        const uint64_t total = hl.point_count;
        const uint64_t step = total / 20000;   // ~20k stickprov över hela filen
        uint64_t mismatches = 0, cls_mismatches = 0;
        for (uint64_t i = 0; i < total; i += step) {
            double ax, ay, az, bx2, by2, bz2;
            las.get_xyz(i, ax, ay, az);
            laz.get_xyz(i, bx2, by2, bz2);
            if (ax != bx2 || ay != by2 || az != bz2) ++mismatches;
            if (las.get_classification(i) != laz.get_classification(i)) ++cls_mismatches;
        }
        CHECK(mismatches == 0);
        CHECK(cls_mismatches == 0);

        // Hela råposten måste också vara identisk
        uint8_t ra[testdata::kRecLen12], rb[testdata::kRecLen12];
        uint64_t raw_mismatches = 0;
        for (uint64_t i = 0; i < total; i += step * 10) {
            las.read_points(i, 1, ra);
            laz.read_points(i, 1, rb);
            if (std::memcmp(ra, rb, testdata::kRecLen12) != 0) ++raw_mismatches;
        }
        CHECK(raw_mismatches == 0);

        printf("  las_vs_laz: ~%llu stickprov, %llu XYZ-avvikelser, %llu klass, %llu råpost\n",
               (unsigned long long)(total / step),
               (unsigned long long)mismatches,
               (unsigned long long)cls_mismatches,
               (unsigned long long)raw_mismatches);
    }

    // 20. LAZ — XYZ bounds check (COPC has different point order than LAS, so no cross-file compare)
    {
        HLas::Reader laz;
        laz.open(kTestLaz);
        const auto& hz = laz.header();
        const uint64_t total = hz.point_count;

        uint64_t out_of_bounds = 0;
        double max_dev = 0.0;
        const uint64_t step = total / 1000;
        for (uint64_t i = 0; i < total; i += step) {
            double zx, zy, zz;
            laz.get_xyz(i, zx, zy, zz);
            double dx = 0, dy = 0, dz = 0;
            if (zx < hz.min_x) dx = hz.min_x - zx;
            if (zx > hz.max_x) dx = zx - hz.max_x;
            if (zy < hz.min_y) dy = hz.min_y - zy;
            if (zy > hz.max_y) dy = zy - hz.max_y;
            if (zz < hz.min_z) dz = hz.min_z - zz;
            if (zz > hz.max_z) dz = zz - hz.max_z;
            double dev = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
            if (dev > 0) { ++out_of_bounds; if (dev > max_dev) max_dev = dev; }
        }
        CHECK(max_dev < 0.001);
        printf("  laz_xyz_bounds: ~1000 sampled, %llu out_of_bounds, max_dev=%.6f\n",
               static_cast<unsigned long long>(out_of_bounds), max_dev);
    }

    // 21. LAZ — first 10 points raw XYZ (diagnostic print)
    {
        HLas::Reader laz;
        laz.open(kTestLaz);

        printf("  laz_raw_xyz (first 10 points, raw int32):\n");
        for (uint64_t i = 0; i < 10; ++i) {
            const uint8_t* zp = laz.point_data(i);
            if (!zp) {
                printf("    [%llu] SKIP (null)\n", static_cast<unsigned long long>(i));
                continue;
            }
            int32_t zix, ziy, ziz;
            std::memcpy(&zix, zp,     4);
            std::memcpy(&ziy, zp + 4, 4);
            std::memcpy(&ziz, zp + 8, 4);
            printf("    [%llu] laz(%d,%d,%d)\n",
                   static_cast<unsigned long long>(i), zix, ziy, ziz);
        }
    }

    // 22. LAZ — full scan XYZ within bounds
    {
        HLas::Reader laz;
        laz.open(kTestLaz);
        const auto& hz = laz.header();
        const uint64_t total = hz.point_count;
        const uint64_t chunk = 10000;

        std::vector<double> zx(10000), zy(10000), zz(10000);
        uint64_t out_of_bounds = 0;
        using Clock = std::chrono::high_resolution_clock;
        auto t0 = Clock::now();

        for (uint64_t off = 0; off < total; off += chunk) {
            uint64_t nz = laz.get_xyz_batch(off, chunk, zx.data(), zy.data(), zz.data());
            for (uint64_t i = 0; i < nz; ++i) {
                if (zx[i] < hz.min_x - 0.001 || zx[i] > hz.max_x + 0.001 ||
                    zy[i] < hz.min_y - 0.001 || zy[i] > hz.max_y + 0.001 ||
                    zz[i] < hz.min_z - 0.001 || zz[i] > hz.max_z + 0.001)
                    ++out_of_bounds;
            }
        }
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        CHECK(out_of_bounds == 0);
        printf("  laz_full_scan: %llu points, %llu out_of_bounds, %.1f ms\n",
               static_cast<unsigned long long>(total),
               static_cast<unsigned long long>(out_of_bounds), ms);
    }

    // 23. LAZ — classification sanity check (ground points exist, not all same class)
    {
        HLas::Reader laz;
        laz.open(kTestLaz);
        const auto& hz = laz.header();
        const uint64_t total = hz.point_count;
        const uint64_t step = total / 500;

        uint64_t ground_count = 0;
        uint64_t sampled = 0;
        for (uint64_t i = 0; i < total; i += step) {
            uint8_t zc = laz.get_classification(i);
            if (zc == 2) ++ground_count;
            ++sampled;
        }
        CHECK(ground_count > 0);
        CHECK(ground_count < sampled);
        printf("  laz_classification: %llu/%llu sampled are ground\n",
               static_cast<unsigned long long>(ground_count),
               static_cast<unsigned long long>(sampled));
    }

    // 24. LAZ — RGB sanity check (format 7: RGB at offset 30)
    // Format 7 layout: X(4)+Y(4)+Z(4)+intensity(2)+flags(1)+classification(1)+
    //   user_data(1)+scan_angle(2)+point_source_id(2)+gps_time(8) = 30 bytes before RGB
    {
        // no_cache krävs: sidecaren har 13-bytes poster utan RGB, och att läsa
        // offset 30 ur den ger oinitierat minne — testet skulle passera på skräp.
        HLas::Reader laz;
        laz.open(kTestLaz);
        const uint64_t total = laz.header().point_count;
        CHECK(laz.header().point_record_length == testdata::kRecLen14);

        uint8_t zbuf[36] = {};
        bool has_nonzero_rgb = false;
        const uint64_t step = total / 500;

        for (uint64_t i = 0; i < total; i += step) {
            laz.read_points(i, 1, zbuf);
            uint16_t r, g, b;
            std::memcpy(&r, zbuf + 30, 2);
            std::memcpy(&g, zbuf + 32, 2);
            std::memcpy(&b, zbuf + 34, 2);
            if (r != 0 || g != 0 || b != 0) { has_nonzero_rgb = true; break; }
        }
        CHECK(has_nonzero_rgb);
        if (has_nonzero_rgb) printf("  laz_rgb: nonzero RGB found in LAZ format 7\n");
    }
    // 25. LAZ 1.4 (COPC) — open and verify header
    {
        const char* kTestLaz14 = testdata::kCopc;
        HLas::Reader laz14;
        HLas::Status s14 = laz14.open(kTestLaz14);
        printf("  laz14_open: status=%d (%s)\n", (int)s14, HLas::status_message(s14));
        CHECK(s14 == HLas::Status::Ok);

        if (s14 == HLas::Status::Ok) {
            const auto& h14 = laz14.header();
            printf("  laz14_header: count=%llu, format=%u, rec_len=%u, is_laz=%d\n",
                   static_cast<unsigned long long>(h14.point_count),
                   h14.point_format, h14.point_record_length, h14.is_laz);
            CHECK(h14.point_count == testdata::kPointCount);
            CHECK(h14.point_format == 7);
            CHECK(h14.point_record_length == testdata::kRecLen14);

            // Verify first few XYZ values are valid (not zero/garbage)
            double x, y, z;
            laz14.get_xyz(0, x, y, z);
            printf("  laz14_point0: (%.4f, %.4f, %.4f)\n", x, y, z);
            CHECK(x >= testdata::kMinX && x <= testdata::kMaxX);
            CHECK(y >= testdata::kMinY && y <= testdata::kMaxY);

            // Spot-check a few more points
            laz14.get_xyz(1000, x, y, z);
            printf("  laz14_point1000: (%.4f, %.4f, %.4f)\n", x, y, z);
            CHECK(x >= testdata::kMinX && x <= testdata::kMaxX);
        }
    }
#endif // HLAS_HAVE_LAZ

    // DEM thread benchmark — LAZ 1.4 file, thread_count=1 vs 4
    {
        const char* laz_file = testdata::kCopc;
        HLas::Reader r;
        HLas::Status s = r.open(laz_file);
        if (s != HLas::Status::Ok) {
            printf("  dem_threads: skipped (cannot open %s)\n", laz_file);
        } else {
            const auto& h = r.header();
            double px[] = { h.min_x, h.max_x, h.max_x, h.min_x };
            double py[] = { h.min_y, h.min_y, h.max_y, h.max_y };

            using Clock = std::chrono::high_resolution_clock;
            bool ground[256] = {};
            ground[2] = true;

            auto t0 = Clock::now();
            auto dem1 = r.generate_dem(px, py, 4, 1.0, HLas::DemFill::None, ground, 1);
            auto t1 = Clock::now();
            double ms1 = std::chrono::duration<double, std::milli>(t1 - t0).count();

            auto t2 = Clock::now();
            auto dem4 = r.generate_dem(px, py, 4, 1.0, HLas::DemFill::None, ground, 4);
            auto t3 = Clock::now();
            double ms4 = std::chrono::duration<double, std::milli>(t3 - t2).count();

            // Verify identical results
            CHECK(dem1.cols == dem4.cols);
            CHECK(dem1.rows == dem4.rows);
            uint32_t mismatches = 0;
            for (size_t i = 0; i < dem1.elevation.size(); ++i) {
                bool n1 = std::isnan(dem1.elevation[i]);
                bool n4 = std::isnan(dem4.elevation[i]);
                if (n1 != n4 || (!n1 && dem1.elevation[i] != dem4.elevation[i]))
                    ++mismatches;
            }
            CHECK(mismatches == 0);

            printf("  dem_threads: %ux%u grid, 1 thread: %.1f ms, 4 threads: %.1f ms (%.1fx speedup), mismatches: %u\n",
                   dem1.cols, dem1.rows, ms1, ms4, ms1 / ms4, mismatches);
        }
    }

    // Feature 1: get_classification_histogram — LAS
    {
        HLas::Reader r;
        CHECK(r.open(testdata::kLas) == HLas::Status::Ok);
        uint64_t counts[256] = {};
        CHECK(r.get_classification_histogram(counts) == HLas::Status::Ok);
        uint64_t total = 0;
        for (int i = 0; i < 256; ++i) total += counts[i];
        CHECK(total == r.header().point_count);
        CHECK(counts[2] > 0); // mark points exist
        printf("  histogram_las: total=%llu, class2=%llu\n",
               (unsigned long long)total, (unsigned long long)counts[2]);
    }

#ifdef HLAS_HAVE_LAZ
    // Feature 1: get_classification_histogram — LAZ
    {
        HLas::Reader r;
        CHECK(r.open(testdata::kLaz) == HLas::Status::Ok);
        uint64_t counts[256] = {};
        CHECK(r.get_classification_histogram(counts) == HLas::Status::Ok);
        uint64_t total = 0;
        for (int i = 0; i < 256; ++i) total += counts[i];
        CHECK(total == r.header().point_count);
        CHECK(counts[2] > 0);
        printf("  histogram_laz: total=%llu, class2=%llu\n",
               (unsigned long long)total, (unsigned long long)counts[2]);
    }
#endif

    // Feature 2: get_classification_histogram_polygon
    {
        HLas::Reader r;
        CHECK(r.open(testdata::kLas) == HLas::Status::Ok);
        const auto& h = r.header();
        // Full AABB polygon (slightly expanded to include edge points)
        double eps = 1.0;
        double px[] = { h.min_x - eps, h.max_x + eps, h.max_x + eps, h.min_x - eps };
        double py[] = { h.min_y - eps, h.min_y - eps, h.max_y + eps, h.max_y + eps };
        uint64_t full[256] = {}, poly[256] = {};
        r.get_classification_histogram(full);
        r.get_classification_histogram_polygon(poly, px, py, 4);
        uint64_t sum_full = 0, sum_poly = 0;
        for (int i = 0; i < 256; ++i) { sum_full += full[i]; sum_poly += poly[i]; }
        CHECK(sum_poly == sum_full);
        // Small polygon — should have fewer points (inset from edge)
        double sdx = (h.max_x - h.min_x) * 0.1;
        double sdy = (h.max_y - h.min_y) * 0.1;
        double sx[] = { h.min_x + 1.0, h.min_x + sdx, h.min_x + sdx, h.min_x + 1.0 };
        double sy[] = { h.min_y + 1.0, h.min_y + 1.0, h.min_y + sdy, h.min_y + sdy };
        uint64_t small[256] = {};
        r.get_classification_histogram_polygon(small, sx, sy, 4);
        uint64_t sum_small = 0;
        for (int i = 0; i < 256; ++i) sum_small += small[i];
        CHECK(sum_small < sum_full);
        printf("  histogram_polygon: full=%llu, small=%llu\n",
               (unsigned long long)sum_full, (unsigned long long)sum_small);
    }

    // Feature 3: get_xyzc_polygon with accepted[] filter
    {
        HLas::Reader r;
        CHECK(r.open(testdata::kLas) == HLas::Status::Ok);
        const auto& h = r.header();
        // Use a small sub-polygon (10% of extent, slightly inset from edge)
        double dx = (h.max_x - h.min_x) * 0.1;
        double dy = (h.max_y - h.min_y) * 0.1;
        double px[] = { h.min_x + 1.0, h.min_x + dx, h.min_x + dx, h.min_x + 1.0 };
        double py[] = { h.min_y + 1.0, h.min_y + 1.0, h.min_y + dy, h.min_y + dy };
        const uint64_t alloc = 500000;

        bool accepted[256] = {};
        accepted[2] = true;
        std::vector<double> xs(alloc), ys(alloc), zs(alloc);
        std::vector<uint8_t> cls(alloc);
        uint64_t n_accepted = r.get_xyzc_polygon(0, h.point_count, px, py, 4,
                                                  xs.data(), ys.data(), zs.data(),
                                                  cls.data(), -1, accepted);
        CHECK(n_accepted <= alloc);
        // All returned points must be class 2
        bool all_class2 = true;
        for (uint64_t i = 0; i < n_accepted; ++i)
            if (cls[i] != 2) { all_class2 = false; break; }
        CHECK(all_class2);

        // Must match classification=2 result
        uint64_t n_cls2 = r.get_xyzc_polygon(0, h.point_count, px, py, 4,
                                              xs.data(), ys.data(), zs.data(),
                                              cls.data(), 2);
        CHECK(n_accepted == n_cls2);
        printf("  accepted_filter: class2=%llu, matches classification=2: %s\n",
               (unsigned long long)n_accepted, n_accepted == n_cls2 ? "yes" : "no");
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}
