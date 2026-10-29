#include "HLas.h"
#include "test_data.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <ctime>
#include <vector>
#include <fstream>

static const char* kFile   = testdata::kLaz;
static const char* kResult = "RES.md";

using Clock = std::chrono::high_resolution_clock;

static double ms_since(std::chrono::time_point<Clock> t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
    // 1a. Open, default (use sidecar if it exists; otherwise LAZ + skriv sidecar)
    HLas::Reader r;
    auto t0 = Clock::now();
    HLas::Status s = r.open(kFile);
    double t_open = ms_since(t0);
    if (s != HLas::Status::Ok) {
        printf("ERROR: kunde inte öppna %s\n", kFile);
        return 1;
    }

    const auto& h = r.header();
    const uint64_t n = h.point_count;
    printf("Fil: %s\n", kFile);
    printf("Version: %u.%u  Format: %u  Punkter: %llu\n",
           h.version_major, h.version_minor, h.point_format,
           (unsigned long long)n);
    printf("Bbox: X=[%.1f, %.1f]  Y=[%.1f, %.1f]\n",
           h.min_x, h.max_x, h.min_y, h.max_y);
    printf("[0a] open (default — sidecar om den finns): %.1f ms\n", t_open);

    // 1b. Open med no_cache=true för att tvinga LAZ-dekompression som baseline
    {
        HLas::Reader rb;
        auto tb0 = Clock::now();
        HLas::Status sb = rb.open(kFile);
        double t_open_no_cache = ms_since(tb0);
        if (sb == HLas::Status::Ok) {
            printf("[0b] open (no_cache=true, LAZ-dekomp): %.1f ms\n",
                   t_open_no_cache);
        }
    }
    printf("\n");

    // 2. Full read (raw point records)
    std::vector<uint8_t> raw(static_cast<size_t>(n) * h.point_record_length);
    t0 = Clock::now();
    uint64_t got_raw = r.read_points(0, n, raw.data());
    double t_read_raw = ms_since(t0);
    printf("[1] read_points (full): %.1f ms  (%.1f Mpts/s, %llu punkter)\n",
           t_read_raw, n / (t_read_raw * 1e3), (unsigned long long)got_raw);

    // 3. Full XYZ batch (scaled)
    std::vector<double> X(n), Y(n), Z(n);
    t0 = Clock::now();
    uint64_t got_xyz = r.get_xyz_batch(0, n, X.data(), Y.data(), Z.data());
    double t_xyz = ms_since(t0);
    printf("[2] get_xyz_batch (full): %.1f ms  (%.1f Mpts/s, %llu punkter)\n",
           t_xyz, n / (t_xyz * 1e3), (unsigned long long)got_xyz);

    // 4. Classification histogram (full file)
    uint64_t hist[256] = {};
    t0 = Clock::now();
    r.get_classification_histogram(hist);
    double t_hist = ms_since(t0);
    printf("[3] get_classification_histogram: %.1f ms\n", t_hist);

    // 5. DEM — full bbox, ground only, TIN, 1m cell, 8 trådar
    double px_full[] = { h.min_x, h.max_x, h.max_x, h.min_x };
    double py_full[] = { h.min_y, h.min_y, h.max_y, h.max_y };
    bool ground[256] = {};
    ground[2] = true;
    t0 = Clock::now();
    auto dem = r.generate_dem(px_full, py_full, 4, 1.0,
                              HLas::DemFill::Tin, ground, 8);
    double t_dem = ms_since(t0);
    uint32_t total_cells = dem.cols * dem.rows;
    uint32_t nan_cells = 0;
    for (float v : dem.elevation) if (std::isnan(v)) ++nan_cells;
    printf("[4] generate_dem (TIN, ground, 1m, 8 trådar): %.1f ms  "
           "(%ux%u celler, %.1f%% fyllt)\n",
           t_dem, dem.cols, dem.rows,
           100.0 * (total_cells - nan_cells) / total_cells);

    // 6. Polygon-extract — centrerad halv-bbox
    double cx = 0.5 * (h.min_x + h.max_x);
    double cy = 0.5 * (h.min_y + h.max_y);
    double dx = 0.25 * (h.max_x - h.min_x);
    double dy = 0.25 * (h.max_y - h.min_y);
    double px_half[] = { cx - dx, cx + dx, cx + dx, cx - dx };
    double py_half[] = { cy - dy, cy - dy, cy + dy, cy + dy };
    std::vector<double> ox(n), oy(n), oz(n);
    std::vector<uint8_t> oc(n);
    t0 = Clock::now();
    uint64_t got_poly = r.get_xyzc_polygon(0, n,
                                           px_half, py_half, 4,
                                           ox.data(), oy.data(), oz.data(), oc.data(),
                                           -1, nullptr);
    double t_poly = ms_since(t0);
    printf("[5] get_xyzc_polygon (1/4 bbox): %.1f ms  (%llu punkter inom)\n",
           t_poly, (unsigned long long)got_poly);

    // Append till RES.md
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    // Mät no_cache-open en gång till för rapporten (separat Reader så vi inte
    // påverkar resultaten ovan).
    double t_open_no_cache = -1.0;
    {
        HLas::Reader rb;
        auto tb0 = Clock::now();
        HLas::Status sb = rb.open(kFile);
        if (sb == HLas::Status::Ok)
            t_open_no_cache = ms_since(tb0);
    }

    std::ofstream out(kResult, std::ios::app);
    if (out) {
        out << "## " << ts << " — v" << HLAS_VERSION_STRING << "\n\n";
        out << "Fil: `" << kFile << "`  \n";
        out << "Punkter: " << n
            << "  | Format: " << (int)h.point_format
            << "  | LAS " << (int)h.version_major << "." << (int)h.version_minor << "  \n\n";
        out << "| Operation | Tid (ms) | Genomströmning |\n";
        out << "|---|---:|---|\n";
        char line[1024];
        std::snprintf(line, sizeof(line),
            "| open (default) | %.1f | sidecar om den finns |\n"
            "| open (no_cache, LAZ-dekomp) | %.1f | baseline |\n"
            "| read_points (full) | %.1f | %.1f Mpts/s |\n"
            "| get_xyz_batch (full) | %.1f | %.1f Mpts/s |\n"
            "| get_classification_histogram | %.1f | — |\n"
            "| generate_dem TIN ground 1m | %.1f | %ux%u, %.1f%% fyllt |\n"
            "| get_xyzc_polygon (1/4 bbox) | %.1f | %llu punkter |\n",
            t_open,
            t_open_no_cache,
            t_read_raw, n / (t_read_raw * 1e3),
            t_xyz, n / (t_xyz * 1e3),
            t_hist,
            t_dem, dem.cols, dem.rows,
            100.0 * (total_cells - nan_cells) / total_cells,
            t_poly, (unsigned long long)got_poly);
        out << line << "\n";
        printf("\nResultat sparat i %s\n", kResult);
    } else {
        printf("\nVARNING: kunde inte skriva till %s\n", kResult);
    }

    return 0;
}
