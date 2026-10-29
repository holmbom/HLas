#include "dem.h"
#include "geometry.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <limits>
#include <thread>

namespace HLas::detail {

void fill_nearest(float* grid, uint32_t cols, uint32_t rows, int max_radius) {
    // Work on a copy so we don't use freshly-filled values as sources
    const uint32_t size = cols * rows;
    std::vector<float> src(grid, grid + size);

    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            if (!std::isnan(grid[r * cols + c]))
                continue;

            float best = NAN;
            int best_d2 = max_radius * max_radius + 1;

            for (int dr = -max_radius; dr <= max_radius; ++dr) {
                int rr = static_cast<int>(r) + dr;
                if (rr < 0 || rr >= static_cast<int>(rows)) continue;
                for (int dc = -max_radius; dc <= max_radius; ++dc) {
                    int cc = static_cast<int>(c) + dc;
                    if (cc < 0 || cc >= static_cast<int>(cols)) continue;
                    int d2 = dr * dr + dc * dc;
                    if (d2 >= best_d2) continue;
                    float v = src[rr * cols + cc];
                    if (!std::isnan(v)) {
                        best = v;
                        best_d2 = d2;
                    }
                }
            }
            grid[r * cols + c] = best;
        }
    }
}

void fill_idw(float* grid, uint32_t cols, uint32_t rows, int max_radius) {
    const uint32_t size = cols * rows;
    std::vector<float> src(grid, grid + size);

    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            if (!std::isnan(grid[r * cols + c]))
                continue;

            double sum_wz = 0.0;
            double sum_w = 0.0;

            for (int dr = -max_radius; dr <= max_radius; ++dr) {
                int rr = static_cast<int>(r) + dr;
                if (rr < 0 || rr >= static_cast<int>(rows)) continue;
                for (int dc = -max_radius; dc <= max_radius; ++dc) {
                    int cc = static_cast<int>(c) + dc;
                    if (cc < 0 || cc >= static_cast<int>(cols)) continue;
                    float v = src[rr * cols + cc];
                    if (std::isnan(v)) continue;
                    double d2 = static_cast<double>(dr * dr + dc * dc);
                    if (d2 < 1e-12) d2 = 1e-12;
                    double w = 1.0 / d2;
                    sum_wz += w * v;
                    sum_w += w;
                }
            }

            if (sum_w > 0.0)
                grid[r * cols + c] = static_cast<float>(sum_wz / sum_w);
        }
    }
}

// --- Adjacency-based Bowyer-Watson Delaunay + TIN interpolation ---

// Triangulates and interpolates a horizontal strip of the grid.
// row_lo..row_hi: source point rows (expanded with overlap).
// row_fill_start..row_fill_end: NaN cells to actually fill (subset of strip).
// Writes only to rows [row_fill_start, row_fill_end).
static void fill_tin_tile(float* grid, uint32_t cols, uint32_t rows,
                          uint32_t row_lo, uint32_t row_hi,
                          uint32_t row_fill_start, uint32_t row_fill_end) {
    struct Pt { double x, y; float z; };

    std::vector<Pt> pts;
    std::vector<uint32_t> nan_cells;
    for (uint32_t r = row_lo; r < row_hi; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            uint32_t idx = r * cols + c;
            if (!std::isnan(grid[idx]))
                pts.push_back({static_cast<double>(c), static_cast<double>(r), grid[idx]});
            else if (r >= row_fill_start && r < row_fill_end)
                nan_cells.push_back(idx);
        }
    }
    if (pts.size() < 3 || nan_cells.empty()) return;

    const uint32_t np = static_cast<uint32_t>(pts.size());

    // Deterministic shuffle for O(sqrt(n)) expected point location
    std::vector<uint32_t> order(np);
    for (uint32_t i = 0; i < np; ++i) order[i] = i;
    {
        uint32_t rng = np ^ 0x12345678u;
        for (uint32_t i = np - 1; i > 0; --i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            std::swap(order[i], order[rng % (i + 1)]);
        }
    }

    // Super-triangle enclosing all points in this tile
    double dmax = std::max(static_cast<double>(cols), static_cast<double>(row_hi - row_lo));
    double mid_x = (cols - 1) * 0.5;
    double mid_y = (row_lo + row_hi - 1) * 0.5;
    const uint32_t sp0 = np, sp1 = np + 1, sp2 = np + 2;
    pts.push_back({mid_x - 2.0 * dmax, mid_y - dmax,      0.0f});
    pts.push_back({mid_x + 2.0 * dmax, mid_y - dmax,      0.0f});
    pts.push_back({mid_x,              mid_y + 2.0 * dmax, 0.0f});

    static constexpr uint32_t NONE = UINT32_MAX;
    struct Tri {
        uint32_t v[3];
        uint32_t adj[3];
        bool dead;
    };
    struct CC { double cx, cy, r2; };

    auto compute_cc = [&](const Tri& t) -> CC {
        double ax = pts[t.v[0]].x, ay = pts[t.v[0]].y;
        double bx = pts[t.v[1]].x, by = pts[t.v[1]].y;
        double cx = pts[t.v[2]].x, cy = pts[t.v[2]].y;
        double D = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
        if (std::fabs(D) < 1e-30) return {0, 0, 1e30};
        double a2 = ax*ax + ay*ay, b2 = bx*bx + by*by, c2 = cx*cx + cy*cy;
        double ux = (a2*(by-cy) + b2*(cy-ay) + c2*(ay-by)) / D;
        double uy = (a2*(cx-bx) + b2*(ax-cx) + c2*(bx-ax)) / D;
        double dx = ax - ux, dy = ay - uy;
        return {ux, uy, dx*dx + dy*dy};
    };

    std::vector<Tri> tris;
    std::vector<CC> ccs;
    tris.reserve(np * 3);
    ccs.reserve(np * 3);
    tris.push_back({{sp0, sp1, sp2}, {NONE, NONE, NONE}, false});
    ccs.push_back(compute_cc(tris[0]));

    std::vector<uint32_t> bad;
    std::vector<uint8_t> bad_flag;
    struct BndEdge { uint32_t a, b, outer_tri; int outer_adj; };
    std::vector<BndEdge> boundary;
    uint32_t hint = 0;

    for (uint32_t ii = 0; ii < np; ++ii) {
        const uint32_t pi = order[ii];
        const double px = pts[pi].x, py = pts[pi].y;

        uint32_t cur = hint;
        if (cur >= tris.size() || tris[cur].dead) {
            for (cur = 0; cur < static_cast<uint32_t>(tris.size()); ++cur)
                if (!tris[cur].dead) break;
        }

        for (int iter = 0; iter < 500000; ++iter) {
            const Tri& t = tris[cur];
            double x0 = pts[t.v[0]].x, y0 = pts[t.v[0]].y;
            double x1 = pts[t.v[1]].x, y1 = pts[t.v[1]].y;
            double x2 = pts[t.v[2]].x, y2 = pts[t.v[2]].y;
            double d0 = (x2-x1)*(py-y1) - (y2-y1)*(px-x1);
            double d1 = (x0-x2)*(py-y2) - (y0-y2)*(px-x2);
            double d2 = (x1-x0)*(py-y0) - (y1-y0)*(px-x0);
            if (d0 >= -1e-10 && d1 >= -1e-10 && d2 >= -1e-10) break;
            double min_d = d0; int min_e = 0;
            if (d1 < min_d) { min_d = d1; min_e = 1; }
            if (d2 < min_d) { min_d = d2; min_e = 2; }
            uint32_t next = t.adj[min_e];
            if (next != NONE && !tris[next].dead) { cur = next; continue; }
            bool moved = false;
            for (int e = 0; e < 3; ++e) {
                if (e == min_e) continue;
                double de = (e == 0) ? d0 : (e == 1) ? d1 : d2;
                if (de < 0 && t.adj[e] != NONE && !tris[t.adj[e]].dead) {
                    cur = t.adj[e]; moved = true; break;
                }
            }
            if (!moved) break;
        }

        bad.clear();
        if (bad_flag.size() < tris.size() + 64)
            bad_flag.resize(tris.size() + 64, 0);
        {
            const CC& cc = ccs[cur];
            double dx = px - cc.cx, dy = py - cc.cy;
            if (dx*dx + dy*dy <= cc.r2 * (1.0 + 1e-8)) {
                bad.push_back(cur); bad_flag[cur] = 1;
            }
        }
        for (uint32_t qi = 0; qi < static_cast<uint32_t>(bad.size()); ++qi) {
            const Tri& t = tris[bad[qi]];
            for (int e = 0; e < 3; ++e) {
                uint32_t nb = t.adj[e];
                if (nb == NONE || tris[nb].dead) continue;
                if (nb < bad_flag.size() && bad_flag[nb]) continue;
                if (nb >= bad_flag.size()) bad_flag.resize(nb + 64, 0);
                const CC& cc = ccs[nb];
                double dx = px - cc.cx, dy = py - cc.cy;
                if (dx*dx + dy*dy <= cc.r2 * (1.0 + 1e-8)) {
                    bad.push_back(nb); bad_flag[nb] = 1;
                }
            }
        }
        if (bad.empty()) continue;

        boundary.clear();
        for (uint32_t bi : bad) {
            const Tri& t = tris[bi];
            for (int e = 0; e < 3; ++e) {
                uint32_t nb = t.adj[e];
                bool nb_bad = (nb != NONE && !tris[nb].dead &&
                               nb < bad_flag.size() && bad_flag[nb]);
                if (!nb_bad) {
                    uint32_t a = t.v[(e + 1) % 3], b = t.v[(e + 2) % 3];
                    int oadj = -1;
                    if (nb != NONE && !tris[nb].dead)
                        for (int k = 0; k < 3; ++k)
                            if (tris[nb].adj[k] == bi) { oadj = k; break; }
                    boundary.push_back({a, b, nb, oadj});
                }
            }
        }
        for (uint32_t i = 0; i + 1 < static_cast<uint32_t>(boundary.size()); ++i) {
            uint32_t target = boundary[i].b;
            for (uint32_t j = i + 1; j < static_cast<uint32_t>(boundary.size()); ++j) {
                if (boundary[j].a == target) { std::swap(boundary[i + 1], boundary[j]); break; }
            }
        }
        for (uint32_t bi : bad) { tris[bi].dead = true; bad_flag[bi] = 0; }

        uint32_t first_new = static_cast<uint32_t>(tris.size());
        uint32_t num_bnd   = static_cast<uint32_t>(boundary.size());
        for (uint32_t i = 0; i < num_bnd; ++i) {
            const auto& be = boundary[i];
            Tri nt{};
            nt.v[0] = pi; nt.v[1] = be.a; nt.v[2] = be.b;
            nt.adj[0] = be.outer_tri;
            nt.adj[1] = first_new + ((i + 1) % num_bnd);
            nt.adj[2] = first_new + ((i + num_bnd - 1) % num_bnd);
            nt.dead = false;
            uint32_t ti = static_cast<uint32_t>(tris.size());
            tris.push_back(nt);
            ccs.push_back(compute_cc(nt));
            if (be.outer_tri != NONE && be.outer_adj >= 0)
                tris[be.outer_tri].adj[be.outer_adj] = ti;
        }
        if (bad_flag.size() < tris.size()) bad_flag.resize(tris.size(), 0);
        hint = first_new;
    }

    // Collect final triangles
    struct FTri { uint32_t v[3]; };
    std::vector<FTri> final_tris;
    for (const auto& t : tris) {
        if (t.dead) continue;
        if (t.v[0] >= sp0 || t.v[1] >= sp0 || t.v[2] >= sp0) continue;
        final_tris.push_back({t.v[0], t.v[1], t.v[2]});
    }
    if (final_tris.empty()) return;

    // Spatial grid for fast lookup
    uint32_t tile_h = row_hi - row_lo;
    uint32_t gnx = std::max(1u, static_cast<uint32_t>(
        std::sqrt(static_cast<double>(final_tris.size()) * 0.5)));
    if (gnx > cols) gnx = cols;
    uint32_t gny = std::max(1u, static_cast<uint32_t>(
        std::sqrt(static_cast<double>(final_tris.size()) * 0.5)));
    if (gny > tile_h) gny = tile_h;
    double cw = static_cast<double>(cols) / gnx;
    double ch = static_cast<double>(tile_h) / gny;

    std::vector<std::vector<uint32_t>> spatial(static_cast<size_t>(gnx) * gny);
    for (uint32_t ti = 0; ti < static_cast<uint32_t>(final_tris.size()); ++ti) {
        const auto& ft = final_tris[ti];
        double txmin = std::min({pts[ft.v[0]].x, pts[ft.v[1]].x, pts[ft.v[2]].x});
        double txmax = std::max({pts[ft.v[0]].x, pts[ft.v[1]].x, pts[ft.v[2]].x});
        double tymin = std::min({pts[ft.v[0]].y, pts[ft.v[1]].y, pts[ft.v[2]].y}) - row_lo;
        double tymax = std::max({pts[ft.v[0]].y, pts[ft.v[1]].y, pts[ft.v[2]].y}) - row_lo;

        uint32_t cx0 = static_cast<uint32_t>(std::max(0.0, txmin / cw));
        uint32_t cx1 = static_cast<uint32_t>(std::min(static_cast<double>(gnx - 1), txmax / cw));
        uint32_t cy0 = static_cast<uint32_t>(std::max(0.0, tymin / ch));
        uint32_t cy1 = static_cast<uint32_t>(std::min(static_cast<double>(gny - 1), tymax / ch));
        for (uint32_t gy = cy0; gy <= cy1; ++gy)
            for (uint32_t gx = cx0; gx <= cx1; ++gx)
                spatial[gy * gnx + gx].push_back(ti);
    }

    // Interpolate NaN cells
    for (uint32_t idx : nan_cells) {
        uint32_t r = idx / cols;
        uint32_t c = idx % cols;
        double px = static_cast<double>(c);
        double py = static_cast<double>(r);

        double py_local = py - row_lo;
        uint32_t gx = static_cast<uint32_t>(std::min(px / cw,       static_cast<double>(gnx - 1)));
        uint32_t gy = static_cast<uint32_t>(std::min(py_local / ch, static_cast<double>(gny - 1)));

        for (uint32_t ti : spatial[gy * gnx + gx]) {
            const auto& ft = final_tris[ti];
            double x0 = pts[ft.v[0]].x, y0 = pts[ft.v[0]].y;
            double x1 = pts[ft.v[1]].x, y1 = pts[ft.v[1]].y;
            double x2 = pts[ft.v[2]].x, y2 = pts[ft.v[2]].y;
            double denom = (y1-y2)*(x0-x2) + (x2-x1)*(y0-y2);
            if (std::fabs(denom) < 1e-20) continue;
            double u = ((y1-y2)*(px-x2) + (x2-x1)*(py-y2)) / denom;
            double v = ((y2-y0)*(px-x2) + (x0-x2)*(py-y2)) / denom;
            double w = 1.0 - u - v;
            if (u >= -1e-6 && v >= -1e-6 && w >= -1e-6) {
                grid[idx] = static_cast<float>(
                    u * pts[ft.v[0]].z + v * pts[ft.v[1]].z + w * pts[ft.v[2]].z);
                break;
            }
        }
    }
}

void fill_tin(float* grid, uint32_t cols, uint32_t rows, int thread_count) {
    // Overlap in rows to avoid boundary artifacts between tiles.
    // With 1m cells and typical LiDAR ground triangles of <50m, 100 rows is ample.
    static constexpr uint32_t kOverlap = 100;

    if (thread_count <= 1) {
        fill_tin_tile(grid, cols, rows, 0, rows, 0, rows);
        return;
    }

    const int tc = thread_count;
    std::vector<std::thread> threads;
    threads.reserve(tc);
    for (int t = 0; t < tc; ++t) {
        uint32_t rs = static_cast<uint32_t>(t)     * (rows / tc);
        uint32_t re = (t == tc - 1) ? rows : static_cast<uint32_t>(t + 1) * (rows / tc);
        uint32_t lo = (rs > kOverlap) ? rs - kOverlap : 0;
        uint32_t hi = std::min(rows, re + kOverlap);
        threads.emplace_back(fill_tin_tile, grid, cols, rows, lo, hi, rs, re);
    }
    for (auto& th : threads) th.join();
}

DemResult dem_make_grid(const double* poly_x, const double* poly_y,
                        uint32_t num_vertices, double cell_size) {
    DemResult result{};
    if (num_vertices < 3 || cell_size <= 0.0)
        return result;

    double bbox_min_x = poly_x[0], bbox_max_x = poly_x[0];
    double bbox_min_y = poly_y[0], bbox_max_y = poly_y[0];
    for (uint32_t v = 1; v < num_vertices; ++v) {
        if (poly_x[v] < bbox_min_x) bbox_min_x = poly_x[v];
        if (poly_x[v] > bbox_max_x) bbox_max_x = poly_x[v];
        if (poly_y[v] < bbox_min_y) bbox_min_y = poly_y[v];
        if (poly_y[v] > bbox_max_y) bbox_max_y = poly_y[v];
    }

    result.origin_x = bbox_min_x;
    result.origin_y = bbox_min_y;
    result.cell_size = cell_size;
    // Räknas i double och kontrolleras INNAN castet: en polygon på tusen mil med
    // metercell ger ett tal som inte ryms i uint32, och castet är då odefinierat
    // beteende — inte ett stort värde.
    const double dcols = std::ceil((bbox_max_x - bbox_min_x) / cell_size);
    const double drows = std::ceil((bbox_max_y - bbox_min_y) / cell_size);
    if (!(dcols >= 1.0) || !(drows >= 1.0))       // fångar även NaN
        return result;
    if (dcols > kDemMaxCells || drows > kDemMaxCells ||
        dcols * drows > static_cast<double>(kDemMaxCells))
        return result;

    result.cols = static_cast<uint32_t>(dcols);
    result.rows = static_cast<uint32_t>(drows);

    result.elevation.assign(static_cast<size_t>(result.cols) * result.rows, NAN);
    return result;
}

void dem_accumulate(DemResult& result, const PointSource& src,
                    const double* poly_x, const double* poly_y,
                    uint32_t num_vertices,
                    const bool* accepted, int thread_count,
                    bool atomic_writes) {
    if (result.elevation.empty() || num_vertices < 3)
        return;
    if (!src.valid() || src.count == 0)
        return;

    const double bbox_min_x = result.origin_x;
    const double bbox_min_y = result.origin_y;
    const double cell_size  = result.cell_size;

    // Övre gränsen måste komma ur POLYGONEN, inte ur rutnätets kant. cols är
    // avrundat uppåt, så rutnätet sträcker sig en bit förbi polygonen — och
    // AABB-genvägen nedan jämför polygonens hörn mot gränsen. Härleds den ur
    // rutnätet matchar hörnen inte längre, genvägen slås av, och kantfallen
    // avgörs av point_in_polygon i stället. Skillnaden syns som några celler.
    double bbox_max_x = poly_x[0], bbox_max_y = poly_y[0];
    for (uint32_t v = 1; v < num_vertices; ++v) {
        if (poly_x[v] > bbox_max_x) bbox_max_x = poly_x[v];
        if (poly_y[v] > bbox_max_y) bbox_max_y = poly_y[v];
    }

    const uint64_t total = src.count;
    const uint16_t stride = src.stride;
    const uint16_t class_off = src.class_offset;
    const uint8_t  class_mask = src.class_mask;

    if (thread_count < 1) thread_count = 1;

    // Axelparallell rektangel — då kan point_in_polygon hoppas över helt
    bool is_aabb = (num_vertices == 4);
    for (uint32_t v = 0; v < num_vertices && is_aabb; ++v) {
        bool on_x = (poly_x[v] == bbox_min_x || poly_x[v] == bbox_max_x);
        bool on_y = (poly_y[v] == bbox_min_y || poly_y[v] == bbox_max_y);
        if (!on_x || !on_y) is_aabb = false;
    }

    // Atomisk min-skrivning på en cell.
    //
    // Tidigare gjordes det med reinterpret_cast<std::atomic<uint32_t>*> på en
    // float* — men det finns inget atomic-objekt på den adressen, så det bröt
    // mot objektmodellen och var odefinierat beteende. std::atomic_ref är
    // precis det verktyget: en atomisk vy över ett vanligt objekt.
    //
    // compare_exchange på float jämför objektrepresentationen, alltså bitarna,
    // vilket är vad vi vill ha — en NaN matchar samma NaN.
    static_assert(std::atomic_ref<float>::required_alignment <= alignof(float),
                  "atomic_ref<float> kräver starkare justering än vector<float> ger");

    auto atomic_min = [](float* target, float value) {
        std::atomic_ref<float> cell(*target);
        float current = cell.load(std::memory_order_relaxed);
        for (;;) {
            if (!std::isnan(current) && current <= value)
                return;                       // redan lägre — inget att göra
            if (cell.compare_exchange_weak(current, value,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
                return;
            // current har uppdaterats med det som faktiskt låg där
        }
    };

    auto scan_range = [&](uint64_t start, uint64_t end, float* grid, bool use_atomic) {
        const uint8_t* p = src.raw_point(start);
        if (!p) return;
        for (uint64_t i = start; i < end; ++i, p += stride) {
            if (accepted && !accepted[p[class_off] & class_mask])
                continue;

            int32_t ix, iy, iz;
            std::memcpy(&ix, p,     4);
            std::memcpy(&iy, p + 4, 4);
            std::memcpy(&iz, p + 8, 4);

            const double x = ix * src.x_scale + src.x_offset;
            const double y = iy * src.y_scale + src.y_offset;

            if (x >= bbox_min_x && x <= bbox_max_x &&
                y >= bbox_min_y && y <= bbox_max_y &&
                (is_aabb || point_in_polygon(x, y, poly_x, poly_y, num_vertices))) {

                const double z = iz * src.z_scale + src.z_offset;
                uint32_t col = static_cast<uint32_t>((x - bbox_min_x) / cell_size);
                uint32_t row = static_cast<uint32_t>((y - bbox_min_y) / cell_size);
                if (col >= result.cols) col = result.cols - 1;
                if (row >= result.rows) row = result.rows - 1;

                const uint32_t idx = row * result.cols + col;
                const float fz = static_cast<float>(z);
                if (use_atomic) {
                    atomic_min(&grid[idx], fz);
                } else {
                    if (std::isnan(grid[idx]) || fz < grid[idx])
                        grid[idx] = fz;
                }
            }
        }
    };

    if (thread_count <= 1) {
        scan_range(0, total, result.elevation.data(), atomic_writes);
    } else {
        const int tc = thread_count;
        std::vector<std::thread> threads;
        threads.reserve(tc);
        for (int t = 0; t < tc; ++t) {
            uint64_t s = t * (total / tc);
            uint64_t e = (t == tc - 1) ? total : (t + 1) * (total / tc);
            threads.emplace_back(scan_range, s, e, result.elevation.data(), true);
        }
        for (auto& th : threads) th.join();
    }
}

void dem_fill(DemResult& grid, DemFill fill, int thread_count) {
    if (grid.elevation.empty()) return;
    if (thread_count < 1) thread_count = 1;
    if (fill == DemFill::Nearest)
        fill_nearest(grid.elevation.data(), grid.cols, grid.rows);
    else if (fill == DemFill::Idw)
        fill_idw(grid.elevation.data(), grid.cols, grid.rows);
    else if (fill == DemFill::Tin)
        fill_tin(grid.elevation.data(), grid.cols, grid.rows, thread_count);
}

DemResult generate_dem(const PointSource& src,
                       const double* poly_x, const double* poly_y,
                       uint32_t num_vertices,
                       double cell_size,
                       DemFill fill,
                       const bool* accepted,
                       int thread_count) {
    DemResult result = dem_make_grid(poly_x, poly_y, num_vertices, cell_size);
    if (result.elevation.empty())
        return result;
    dem_accumulate(result, src, poly_x, poly_y, num_vertices, accepted, thread_count);
    dem_fill(result, fill, thread_count);
    return result;
}

} // namespace HLas::detail
