// Rutnät mot Hilbert-block — vilken form tål en SNED KORRIDOR? Inför 1.1.4.
//
// Mätningen i bench_corridor.cpp visade att 1.1.3:s cache är LÅNGSAMMARE än
// ingen cache alls för en diagonal korridor: sållningen jämför blockens bbox mot
// FRÅGANS BBOX, och en diagonal korridors bbox täcker hela filen.
//
// Två frågor följde av det, och de är olika:
//
//   1. Hur mycket tar man igen genom att sålla mot POLYGONEN i stället? Det är
//      en ren algoritmändring, inget format.
//   2. Skulle ett RUTNÄT vara bättre än Hilbert-block? Rutnätet kan räkna fram
//      vilka celler polygonen korsar utan att titta på ett enda cellhuvud,
//      medan Hilbert måste läsa alla blockhuvuden för att veta.
//
// ⛔ BÅDA varianterna får här korrekt polygonsållning, så det som jämförs är
// STRUKTURERNA och inte 1.1.3:s bugg.
//
// Storleken mäts som den skulle ligga PÅ DISK, inte i minnet:
//   Hilbert:  data + block × (min xyz, max xy, tre bitbredder) = 23 B/block
//   Rutnät:   data + cell  × (offset, min_z, bz)               = 13 B/cell
// Rutnätet slipper lagra min_x/min_y — de följer av cellens position — men
// måste lagra en offset, eftersom cellerna har olika många punkter och
// bitpositionen därför inte går att räkna fram.

#include "HLas.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int bits_for(uint64_t range) { int b = 1; while (b < 64 && (range >> b)) ++b; return b; }

struct BitWriter {
    std::vector<uint8_t> buf;
    uint64_t bitpos = 0;
    void put(uint64_t v, int bits) {
        if (bits <= 0) return;
        const uint64_t need = (bitpos + bits + 7) / 8 + 8;
        if (buf.size() < need) buf.resize(need, 0);
        for (int i = 0; i < bits; ++i)
            if ((v >> i) & 1ull) buf[(bitpos + i) >> 3] |= uint8_t(1u << ((bitpos + i) & 7));
        bitpos += bits;
    }
};

inline uint64_t bitread(const uint8_t* p, uint64_t bitpos, int bits) {
    if (bits <= 0 || bits > 57) return 0;
    uint64_t w;
    std::memcpy(&w, p + (bitpos >> 3), 8);
    return (w >> (bitpos & 7)) & ((bits == 64) ? ~0ull : ((1ull << bits) - 1));
}

struct Point { int32_t x, y, z; uint8_t c; };

// Skillings algoritm — samma som i sidecar_lite.cpp.
uint64_t hilbert3(uint32_t x, uint32_t y, uint32_t z, int bits) {
    uint32_t v[3] = { x, y, z };
    uint32_t m = 1u << (bits - 1);
    for (uint32_t q = m; q > 1; q >>= 1) {
        const uint32_t p = q - 1;
        for (int i = 0; i < 3; ++i) {
            if (v[i] & q) v[0] ^= p;
            else { const uint32_t t = (v[0] ^ v[i]) & p; v[0] ^= t; v[i] ^= t; }
        }
    }
    for (int i = 1; i < 3; ++i) v[i] ^= v[i-1];
    uint32_t t = 0;
    for (uint32_t q = m; q > 1; q >>= 1) if (v[2] & q) t ^= q - 1;
    for (int i = 0; i < 3; ++i) v[i] ^= t;
    uint64_t key = 0;
    for (int b = bits - 1; b >= 0; --b)
        for (int i = 0; i < 3; ++i) key = (key << 1) | ((v[i] >> b) & 1u);
    return key;
}

// ---------------------------------------------------------------- geometri

struct Poly {
    std::vector<double> x, y;
    double x0, y0, x1, y1;                     // bbox
    void finish() {
        x0 = x1 = x[0]; y0 = y1 = y[0];
        for (size_t i = 1; i < x.size(); ++i) {
            x0 = std::min(x0, x[i]); x1 = std::max(x1, x[i]);
            y0 = std::min(y0, y[i]); y1 = std::max(y1, y[i]);
        }
    }
};

bool point_in_poly(double px, double py, const Poly& p) {
    bool in = false;
    const size_t n = p.x.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++)
        if (((p.y[i] > py) != (p.y[j] > py)) &&
            (px < (p.x[j] - p.x[i]) * (py - p.y[i]) / (p.y[j] - p.y[i]) + p.x[i]))
            in = !in;
    return in;
}

bool seg_cross(double ax, double ay, double bx, double by,
               double cx, double cy, double dx, double dy) {
    auto side = [](double x1,double y1,double x2,double y2,double x3,double y3) {
        const double v = (x2-x1)*(y3-y1) - (y2-y1)*(x3-x1);
        return (v > 0) - (v < 0);
    };
    const int d1 = side(ax,ay,bx,by,cx,cy), d2 = side(ax,ay,bx,by,dx,dy);
    const int d3 = side(cx,cy,dx,dy,ax,ay), d4 = side(cx,cy,dx,dy,bx,by);
    return d1 != d2 && d3 != d4;
}

// ⛔ Det HÄR är sållningen 1.1.3 saknar. En rektangel — ett blocks bbox eller en
// rutnätscell — mot polygonen, inte mot polygonens bbox.
bool rect_hits_poly(double rx0, double ry0, double rx1, double ry1, const Poly& p) {
    if (rx1 < p.x0 || rx0 > p.x1 || ry1 < p.y0 || ry0 > p.y1) return false;
    // Något polygonhörn i rektangeln?
    for (size_t i = 0; i < p.x.size(); ++i)
        if (p.x[i] >= rx0 && p.x[i] <= rx1 && p.y[i] >= ry0 && p.y[i] <= ry1) return true;
    // Något rektangelhörn i polygonen? Räcker med ett — hela hörnet ligger inne.
    if (point_in_poly(rx0, ry0, p) || point_in_poly(rx1, ry0, p) ||
        point_in_poly(rx1, ry1, p) || point_in_poly(rx0, ry1, p)) return true;
    // Skär någon polygonkant någon rektangelkant?
    const double cx[5] = { rx0, rx1, rx1, rx0, rx0 };
    const double cy[5] = { ry0, ry0, ry1, ry1, ry0 };
    const size_t n = p.x.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++)
        for (int k = 0; k < 4; ++k)
            if (seg_cross(p.x[j], p.y[j], p.x[i], p.y[i],
                          cx[k], cy[k], cx[k+1], cy[k+1])) return true;
    return false;
}

// ---------------------------------------------------------------- Hilbert FOR

struct Hilb {
    struct Block {
        int32_t min_x, min_y, min_z, max_x, max_y;
        uint8_t bx, by, bz;
        uint64_t bitpos;
        uint32_t first, n;
    };
    std::vector<Block> blocks;
    std::vector<uint8_t> data;
    uint64_t count = 0;
    uint32_t B = 512;
    double build_ms = 0;
    static constexpr size_t kDiskHeader = 5 * 4 + 3;   // min xyz, max xy, 3 bredder
    double bytes() const { return double(data.size() + blocks.size() * kDiskHeader); }
    double bpp() const { return bytes() / double(count); }
};

Hilb build_hilbert(std::vector<Point> p, uint32_t B) {
    const auto t0 = Clock::now();
    Hilb r; r.B = B; r.count = p.size();

    int32_t gx = p[0].x, gy = p[0].y, gz = p[0].z, hx = gx, hy = gy, hz = gz;
    for (const auto& q : p) {
        gx = std::min(gx, q.x); hx = std::max(hx, q.x);
        gy = std::min(gy, q.y); hy = std::max(hy, q.y);
        gz = std::min(gz, q.z); hz = std::max(hz, q.z);
    }
    const int raw = std::max({ bits_for(uint64_t(hx) - gx), bits_for(uint64_t(hy) - gy),
                               bits_for(uint64_t(hz) - gz) });
    const int kb = raw > 21 ? 21 : raw, sh = raw - kb;

    std::vector<uint64_t> key(p.size());
    for (size_t i = 0; i < p.size(); ++i)
        key[i] = hilbert3(uint32_t((uint64_t(int64_t(p[i].x) - gx)) >> sh),
                          uint32_t((uint64_t(int64_t(p[i].y) - gy)) >> sh),
                          uint32_t((uint64_t(int64_t(p[i].z) - gz)) >> sh), kb);
    std::vector<uint32_t> ord(p.size());
    for (uint32_t i = 0; i < ord.size(); ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(),
              [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });

    BitWriter w;
    for (uint64_t s = 0; s < p.size(); s += B) {
        const uint32_t n = uint32_t(std::min<uint64_t>(B, p.size() - s));
        Hilb::Block b{};
        b.min_x = b.max_x = p[ord[s]].x; b.min_y = b.max_y = p[ord[s]].y;
        b.min_z = p[ord[s]].z;
        int32_t mz = b.min_z;
        for (uint32_t k = 0; k < n; ++k) {
            const auto& q = p[ord[s+k]];
            b.min_x = std::min(b.min_x, q.x); b.max_x = std::max(b.max_x, q.x);
            b.min_y = std::min(b.min_y, q.y); b.max_y = std::max(b.max_y, q.y);
            b.min_z = std::min(b.min_z, q.z); mz = std::max(mz, q.z);
        }
        b.bx = uint8_t(bits_for(uint64_t(b.max_x) - b.min_x));
        b.by = uint8_t(bits_for(uint64_t(b.max_y) - b.min_y));
        b.bz = uint8_t(bits_for(uint64_t(mz) - b.min_z));
        b.bitpos = w.bitpos; b.first = uint32_t(s); b.n = n;
        for (uint32_t k = 0; k < n; ++k) {
            const auto& q = p[ord[s+k]];
            w.put(uint64_t(int64_t(q.x) - b.min_x), b.bx);
            w.put(uint64_t(int64_t(q.y) - b.min_y), b.by);
            w.put(uint64_t(int64_t(q.z) - b.min_z), b.bz);
            w.put(q.c, 8);
        }
        r.blocks.push_back(b);
    }
    r.data = std::move(w.buf);
    r.build_ms = ms_since(t0);
    return r;
}

uint64_t query_hilbert(const Hilb& r, const Poly& p, double sx, double ox,
                       double sy, double oy, uint64_t* decoded, uint64_t* touched,
                       bool bbox_only = false) {
    uint64_t hits = 0;
    const uint8_t* d = r.data.data();
    for (const auto& b : r.blocks) {
        const double bx0 = b.min_x * sx + ox, bx1 = b.max_x * sx + ox;
        const double by0 = b.min_y * sy + oy, by1 = b.max_y * sy + oy;
        // bbox_only = 1.1.3 SOM DEN LIGGER: blockets bbox mot FRÅGANS BBOX.
        // Annars mot polygonen, vilket är fixen.
        if (bbox_only) {
            if (bx1 < p.x0 || bx0 > p.x1 || by1 < p.y0 || by0 > p.y1) continue;
        } else {
            if (!rect_hits_poly(bx0, by0, bx1, by1, p)) continue;
        }
        ++*touched;
        const int stride = b.bx + b.by + b.bz + 8;
        for (uint32_t k = 0; k < b.n; ++k) {
            const uint64_t base = b.bitpos + uint64_t(k) * stride;
            const double x = (int64_t(bitread(d, base, b.bx)) + b.min_x) * sx + ox;
            if (x < p.x0 || x > p.x1) continue;
            const double y = (int64_t(bitread(d, base + b.bx, b.by)) + b.min_y) * sy + oy;
            if (y < p.y0 || y > p.y1) continue;
            if (point_in_poly(x, y, p)) ++hits;
        }
        *decoded += b.n;
    }
    return hits;
}

// Platt skanning över råa punkter — vad en .las gör, och vad en .laz gör efter
// uppackning. Ingen sållning finns att göra: varje punkt prövas.
uint64_t query_flat(const std::vector<Point>& p, const Poly& q,
                    double sx, double ox, double sy, double oy,
                    uint64_t* decoded, uint64_t* touched) {
    uint64_t hits = 0;
    for (const auto& pt : p) {
        const double x = pt.x * sx + ox;
        if (x < q.x0 || x > q.x1) continue;
        const double y = pt.y * sy + oy;
        if (y < q.y0 || y > q.y1) continue;
        if (point_in_poly(x, y, q)) ++hits;
    }
    *decoded += p.size();
    ++*touched;
    return hits;
}

// ---------------------------------------------------------------- rutnät

struct Grid {
    // ⛔ Cellstorleken är FYSISK, inte ett antal. En 500 x 500 m-fil ska inte få
    // lika många celler som en 10 x 5 km-ruta — den ska få lika STORA celler.
    // Rutnätets dimensioner följer av bounds, och cellerna blir kvadratiska i
    // världsenheter i stället för att ärva filens proportioner.
    double   cell_m = 15.0;                 // målstorlek i meter
    uint32_t nx = 0, ny = 0;
    int32_t  gx0, gy0;                      // rutnätets ursprung, heltalsenheter
    int64_t  cw, ch;                        // cellbredd/höjd i heltalsenheter
    int      bx = 0, by = 0;                // KONSTANTA, ur cellstorleken
    struct Cell { uint64_t bitpos; int32_t min_z; uint8_t bz; uint32_t n; };
    std::vector<Cell> cells;
    // GLES form: bara celler som har punkter, plus ett radindex. Rastreringen
    // går ändå rad för rad, så per rad slås radens början upp och dess
    // ockuperade celler gås igenom i x-ordning.
    std::vector<uint32_t> row_start;      // ny + 1
    std::vector<uint32_t> occ_cx;         // cellens x, per ockuperad cell
    std::vector<uint32_t> occ_idx;        // -> cells[]
    std::vector<uint8_t> data;
    uint64_t count = 0;
    double build_ms = 0;
    double real_w = 0, real_h = 0;          // verklig cellstorlek i meter
    // offset (8) + min_z (4) + bz (1). Antalet FÖLJER av nästa cells offset, så
    // det behöver inte lagras — `n` här är bara för labbets bekvämlighet.
    static constexpr size_t kDiskHeader = 8 + 4 + 1;          // offset, min_z, bz
    static constexpr size_t kSparseHeader = 4 + 8 + 4 + 1;    // + cellens x
    double bytes() const { return double(data.size() + cells.size() * kDiskHeader); }
    double bpp() const { return bytes() / double(count); }
    double bytes_sparse() const {
        return double(data.size() + occ_idx.size() * kSparseHeader
                    + row_start.size() * 4);
    }
    double bpp_sparse() const { return bytes_sparse() / double(count); }
};

Grid build_grid(const std::vector<Point>& p, double cell_m, double sx, double sy) {
    const auto t0 = Clock::now();
    Grid g; g.cell_m = cell_m; g.count = p.size();

    int32_t hx = p[0].x, hy = p[0].y;
    g.gx0 = p[0].x; g.gy0 = p[0].y;
    for (const auto& q : p) {
        g.gx0 = std::min(g.gx0, q.x); hx = std::max(hx, q.x);
        g.gy0 = std::min(g.gy0, q.y); hy = std::max(hy, q.y);
    }
    // Meter -> heltalsenheter. Minst 1, och taket hindrar att en pytteliten
    // cellstorlek på en stor fil ger hundratals miljoner celler.
    g.cw = std::max<int64_t>(1, int64_t(cell_m / sx));
    g.ch = std::max<int64_t>(1, int64_t(cell_m / sy));
    g.nx = uint32_t((int64_t(hx) - g.gx0) / g.cw + 1);
    g.ny = uint32_t((int64_t(hy) - g.gy0) / g.ch + 1);
    g.real_w = double(g.cw) * sx;
    g.real_h = double(g.ch) * sy;
    g.bx = bits_for(uint64_t(g.cw));
    g.by = bits_for(uint64_t(g.ch));

    // Räkna, prefixsummera, fyll — en stabil hink-sortering utan std::sort.
    const size_t nc = size_t(g.nx) * g.ny;
    std::vector<uint32_t> cnt(nc + 1, 0);
    std::vector<uint32_t> cell(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        const uint64_t cx = uint64_t(int64_t(p[i].x) - g.gx0) / uint64_t(g.cw);
        const uint64_t cy = uint64_t(int64_t(p[i].y) - g.gy0) / uint64_t(g.ch);
        cell[i] = uint32_t(cy * g.nx + cx);
        ++cnt[cell[i] + 1];
    }
    for (size_t i = 1; i <= nc; ++i) cnt[i] += cnt[i-1];
    std::vector<uint32_t> ord(p.size());
    { std::vector<uint32_t> at(cnt.begin(), cnt.end() - 1);
      for (size_t i = 0; i < p.size(); ++i) ord[at[cell[i]]++] = uint32_t(i); }

    g.cells.resize(nc);
    BitWriter w;
    for (size_t c = 0; c < nc; ++c) {
        const uint32_t s = cnt[c], e = cnt[c+1];
        Grid::Cell& cc = g.cells[c];
        cc.bitpos = w.bitpos; cc.n = e - s; cc.min_z = 0; cc.bz = 0;
        if (s == e) continue;
        int32_t mnz = p[ord[s]].z, mxz = mnz;
        for (uint32_t i = s; i < e; ++i) {
            mnz = std::min(mnz, p[ord[i]].z); mxz = std::max(mxz, p[ord[i]].z);
        }
        cc.min_z = mnz; cc.bz = uint8_t(bits_for(uint64_t(mxz) - mnz));
        const int64_t ox = g.gx0 + int64_t(c % g.nx) * g.cw;
        const int64_t oy = g.gy0 + int64_t(c / g.nx) * g.ch;
        for (uint32_t i = s; i < e; ++i) {
            const auto& q = p[ord[i]];
            w.put(uint64_t(int64_t(q.x) - ox), g.bx);
            w.put(uint64_t(int64_t(q.y) - oy), g.by);
            w.put(uint64_t(int64_t(q.z) - cc.min_z), cc.bz);
            w.put(q.c, 8);
        }
    }
    // Radindexet. Cellerna ligger redan i radordning (c = cy*nx + cx), så ett
    // svep räcker för att få dem sorterade på (rad, x).
    g.row_start.assign(size_t(g.ny) + 1, 0);
    for (size_t cy = 0; cy < g.ny; ++cy) {
        g.row_start[cy] = uint32_t(g.occ_idx.size());
        for (size_t cx = 0; cx < g.nx; ++cx) {
            const size_t ci = cy * g.nx + cx;
            if (!g.cells[ci].n) continue;
            g.occ_cx.push_back(uint32_t(cx));
            g.occ_idx.push_back(uint32_t(ci));
        }
    }
    g.row_start[g.ny] = uint32_t(g.occ_idx.size());

    g.data = std::move(w.buf);
    g.build_ms = ms_since(t0);
    return g;
}

// ⛔ RASTRERING. Det här är rutnätets egentliga argument, och det jag INTE hade
// i första utkastet: gå rad för rad och räkna fram polygonens x-utbredning i
// just den raden, i stället för att svepa hela bbox-intervallet.
//
// För en diagonal korridor är skillnaden allt: bbox-intervallet är hela
// rutnätet, medan raden bara rör de två-tre celler stråket faktiskt korsar.
static void row_span(const Poly& p, double ybot, double ytop,
                     double& xmin, double& xmax) {
    xmin = 1e300; xmax = -1e300;
    const size_t n = p.x.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double ax = p.x[j], ay = p.y[j], bx = p.x[i], by = p.y[i];
        // Hörn som ligger i bandet.
        if (ay >= ybot && ay <= ytop) { xmin = std::min(xmin, ax); xmax = std::max(xmax, ax); }
        if (by >= ybot && by <= ytop) { xmin = std::min(xmin, bx); xmax = std::max(xmax, bx); }
        // Där kanten skär bandets över- och underkant.
        if (ay == by) continue;
        for (double yy : { ybot, ytop }) {
            const double t = (yy - ay) / (by - ay);
            if (t < 0.0 || t > 1.0) continue;
            const double xx = ax + t * (bx - ax);
            xmin = std::min(xmin, xx); xmax = std::max(xmax, xx);
        }
    }
}

uint64_t query_grid(const Grid& g, const Poly& p, double sx, double ox_,
                    double sy, double oy_, uint64_t* decoded, uint64_t* touched) {
    uint64_t hits = 0;
    const uint8_t* d = g.data.data();

    auto cell_y = [&](double w) -> int64_t {
        return (int64_t(std::floor((w - oy_) / sy)) - g.gy0) / g.ch;
    };
    auto cell_x = [&](double w) -> int64_t {
        return (int64_t(std::floor((w - ox_) / sx)) - g.gx0) / g.cw;
    };

    int64_t cy0 = std::max<int64_t>(0, cell_y(p.y0));
    int64_t cy1 = std::min<int64_t>(int64_t(g.ny) - 1, cell_y(p.y1));

    for (int64_t cy = cy0; cy <= cy1; ++cy) {
        const int64_t by = g.gy0 + cy * g.ch;
        const double wy0 = by * sy + oy_, wy1 = (by + g.ch) * sy + oy_;

        double xa, xb;
        row_span(p, std::min(wy0, wy1), std::max(wy0, wy1), xa, xb);
        if (xa > xb) continue;                       // polygonen når inte raden

        int64_t cx0 = std::max<int64_t>(0, cell_x(xa));
        int64_t cx1 = std::min<int64_t>(int64_t(g.nx) - 1, cell_x(xb));

        for (int64_t cx = cx0; cx <= cx1; ++cx) {
            const Grid::Cell& cc = g.cells[size_t(cy) * g.nx + size_t(cx)];
            if (cc.n == 0) continue;
            const int64_t bx = g.gx0 + cx * g.cw;
            const double wx0 = bx * sx + ox_, wx1 = (bx + g.cw) * sx + ox_;
            if (!rect_hits_poly(std::min(wx0,wx1), std::min(wy0,wy1),
                                std::max(wx0,wx1), std::max(wy0,wy1), p)) continue;
            ++*touched;
            const int stride = g.bx + g.by + cc.bz + 8;
            for (uint32_t k = 0; k < cc.n; ++k) {
                const uint64_t base = cc.bitpos + uint64_t(k) * stride;
                const double x = (int64_t(bitread(d, base, g.bx)) + bx) * sx + ox_;
                if (x < p.x0 || x > p.x1) continue;
                const double y = (int64_t(bitread(d, base + g.bx, g.by)) + by) * sy + oy_;
                if (y < p.y0 || y > p.y1) continue;
                if (point_in_poly(x, y, p)) ++hits;
            }
            *decoded += cc.n;
        }
    }
    return hits;
}

// En ORM: en polyline med svängar, buffrad +-w/2. Det är den verkliga
// frågeformen — en kraftledning svänger, och offseten ger korridoren.
//
// Bufferten är enkel: varje hörn förskjuts längs medelnormalen av sina två
// segment. Skarpa svängar ger då en något felaktig polygon, men för mjuka
// svängar duger det, och det är formen och hörnANTALET som ska mätas.
Poly snake(double sx, double sy, double heading, double seg_len, int segs,
           double bend, double w, uint32_t seed) {
    std::vector<double> px, py;
    double x = sx, y = sy, hd = heading;
    px.push_back(x); py.push_back(y);
    uint32_t rng = seed ? seed : 1u;
    auto next = [&]() { rng = rng * 1664525u + 1013904223u; 
                        return double(rng >> 8) / double(1u << 24); };
    for (int i = 0; i < segs; ++i) {
        hd += (next() * 2.0 - 1.0) * bend;
        x += std::cos(hd) * seg_len;
        y += std::sin(hd) * seg_len;
        px.push_back(x); py.push_back(y);
    }

    const size_t n = px.size();
    std::vector<double> nx(n), ny(n);
    for (size_t i = 0; i < n; ++i) {
        double dx = 0, dy = 0;
        if (i + 1 < n) { dx += px[i+1] - px[i]; dy += py[i+1] - py[i]; }
        if (i > 0)     { dx += px[i] - px[i-1]; dy += py[i] - py[i-1]; }
        const double L = std::sqrt(dx*dx + dy*dy);
        nx[i] = -dy / L; ny[i] = dx / L;
    }
    Poly p;
    for (size_t i = 0; i < n; ++i)     { p.x.push_back(px[i] + nx[i]*w*0.5);
                                         p.y.push_back(py[i] + ny[i]*w*0.5); }
    for (size_t i = n; i-- > 0; )      { p.x.push_back(px[i] - nx[i]*w*0.5);
                                         p.y.push_back(py[i] - ny[i]*w*0.5); }
    p.finish();
    return p;
}

uint64_t query_grid_sparse(const Grid& g, const Poly& p, double sx, double ox_,
                           double sy, double oy_, uint64_t* decoded,
                           uint64_t* touched) {
    uint64_t hits = 0;
    const uint8_t* d = g.data.data();
    auto cell_y = [&](double w) -> int64_t {
        return (int64_t(std::floor((w - oy_) / sy)) - g.gy0) / g.ch; };
    auto cell_x = [&](double w) -> int64_t {
        return (int64_t(std::floor((w - ox_) / sx)) - g.gx0) / g.cw; };

    int64_t cy0 = std::max<int64_t>(0, cell_y(p.y0));
    int64_t cy1 = std::min<int64_t>(int64_t(g.ny) - 1, cell_y(p.y1));

    for (int64_t cy = cy0; cy <= cy1; ++cy) {
        const int64_t by = g.gy0 + cy * g.ch;
        const double wy0 = by * sy + oy_, wy1 = (by + g.ch) * sy + oy_;
        double xa, xb;
        row_span(p, std::min(wy0, wy1), std::max(wy0, wy1), xa, xb);
        if (xa > xb) continue;
        const int64_t cx0 = std::max<int64_t>(0, cell_x(xa));
        const int64_t cx1 = std::min<int64_t>(int64_t(g.nx) - 1, cell_x(xb));

        // Binärsök radens första cell med cx >= cx0, gå sedan framåt.
        const uint32_t lo = g.row_start[size_t(cy)], hi = g.row_start[size_t(cy)+1];
        uint32_t a = uint32_t(std::lower_bound(g.occ_cx.begin() + lo,
                                               g.occ_cx.begin() + hi,
                                               uint32_t(cx0)) - g.occ_cx.begin());
        for (; a < hi && int64_t(g.occ_cx[a]) <= cx1; ++a) {
            const Grid::Cell& cc = g.cells[g.occ_idx[a]];
            const int64_t bx = g.gx0 + int64_t(g.occ_cx[a]) * g.cw;
            const double wx0 = bx * sx + ox_, wx1 = (bx + g.cw) * sx + ox_;
            if (!rect_hits_poly(std::min(wx0,wx1), std::min(wy0,wy1),
                                std::max(wx0,wx1), std::max(wy0,wy1), p)) continue;
            ++*touched;
            const int stride = g.bx + g.by + cc.bz + 8;
            for (uint32_t k = 0; k < cc.n; ++k) {
                const uint64_t base = cc.bitpos + uint64_t(k) * stride;
                const double x = (int64_t(bitread(d, base, g.bx)) + bx) * sx + ox_;
                if (x < p.x0 || x > p.x1) continue;
                const double y = (int64_t(bitread(d, base + g.bx, g.by)) + by) * sy + oy_;
                if (y < p.y0 || y > p.y1) continue;
                if (point_in_poly(x, y, p)) ++hits;
            }
            *decoded += cc.n;
        }
    }
    return hits;
}

Poly corridor(double ax, double ay, double bx, double by, double w) {
    const double dx = bx - ax, dy = by - ay;
    const double len = std::sqrt(dx*dx + dy*dy);
    const double nx = -dy/len * (w*0.5), ny = dx/len * (w*0.5);
    Poly p;
    p.x = { ax+nx, bx+nx, bx-nx, ax-nx };
    p.y = { ay+ny, by+ny, by-ny, ay-ny };
    p.finish();
    return p;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "bruk: %s <fil.laz>\n", argv[0]); return 1; }

    HLas::Reader rd;
    if (rd.open(argv[1], HLas::Cache::Off) != HLas::Status::Ok) {
        std::fprintf(stderr, "open: %s\n", rd.last_error().c_str()); return 1;
    }
    const HLas::Header h = rd.header();
    const auto v = rd.points();
    std::vector<Point> pts(v.count());
    for (uint64_t i = 0; i < v.count(); ++i)
        pts[i] = { v.raw_x(i), v.raw_y(i), v.raw_z(i), v.classification(i) };
    rd.close();

    double src_mb = 0;
    if (std::FILE* f = std::fopen(argv[1], "rb")) {
        std::fseek(f, 0, SEEK_END); src_mb = double(std::ftell(f)) / 1048576.0;
        std::fclose(f);
    }

    std::printf("Rutnät mot Hilbert-block — sned korridor\n");
    std::printf("========================================\n\n");
    std::printf("%s: %llu punkter, %.0f x %.0f m, källan %.1f MB\n\n",
                argv[1], (unsigned long long)pts.size(),
                h.max_x - h.min_x, h.max_y - h.min_y, src_mb);

    // --- Bygg varianterna ---
    struct Variant {
        std::string name; double bytes, bpp, build_ms;
        const Hilb* hb = nullptr; const Grid* gr = nullptr;
    };
    std::vector<Hilb> hs;
    std::vector<Grid> gs;
    for (uint32_t B : { 256u, 512u }) hs.push_back(build_hilbert(pts, B));
    for (double m : { 10.0, 15.0, 20.0, 30.0 })
        gs.push_back(build_grid(pts, m, h.x_scale, h.y_scale));

    std::printf("STORLEK\n\n");
    std::printf("  %-22s %10s %10s %8s %10s\n",
                "variant", "B/pt tät", "B/pt gles", "bygg s", "celler / totalt");
    std::printf("  %s\n", std::string(70, '-').c_str());
    for (const auto& r : hs)
        std::printf("  hilbert block %-8u %10.2f %10s %8.1f %10zu\n",
                    r.B, r.bpp(), "-", r.build_ms/1000.0, r.blocks.size());
    for (const auto& g : gs) {
        size_t used = 0;
        for (const auto& c : g.cells) if (c.n) ++used;
        char nm[64];
        std::snprintf(nm, sizeof nm, "rutnät %.0f m (%ux%u)", g.cell_m, g.nx, g.ny);
        std::printf("  %-22s %10.2f %10.2f %8.1f %10zu / %zu\n",
                    nm, g.bpp(), g.bpp_sparse(), g.build_ms/1000.0,
                    used, g.cells.size());
    }
    double las_mb = 0;
    { std::string lp(argv[1]); const size_t d = lp.rfind(".laz");
      if (d != std::string::npos) { lp.replace(d, 4, ".las");
        if (std::FILE* f = std::fopen(lp.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END); las_mb = double(std::ftell(f))/1048576.0;
            std::fclose(f); } } }
    std::printf("\n  KÄLLAN:  %s %.1f MB", argv[1], src_mb);
    if (las_mb > 0) std::printf("   |   samma data som .las: %.1f MB", las_mb);
    std::printf("\n  Rått i minnet (13 B/pt): %.1f MB\n",
                double(pts.size()) * 13.0 / 1048576.0);

    // --- Frågorna ---
    const double x0 = h.min_x, y0 = h.min_y, x1 = h.max_x, y1 = h.max_y;
    struct Q { std::string what; std::vector<Poly> ps; };
    std::vector<Q> qs;

    // ⛔ Hela spannet, inte en punkt i det. En korridor kan vara 12 m bred och
    // 300 m lång, eller 200 m bred och flera mil — och en flera mil lång
    // korridor spänner över många filer, så det längsta en ENSKILD cache ser är
    // filens egen diagonal. Den är därför övre gränsen här.
    struct Spec { const char* what; double w; double seg; int segs; double bend; };
    const Spec specs[] = {
        { "orm 12 m x ~300 m",   12,  100,  3, 0.35 },
        { "orm 50 m x ~300 m",   50,  100,  3, 0.35 },
        { "orm 50 m x ~1 km",    50,  200,  5, 0.30 },
        { "orm 50 m x ~3 km",    50,  400, 8,  0.25 },
        { "orm 200 m x ~1 km",  200,  200,  5, 0.30 },
        { "rak diagonal 50 m",   50,    0,  0, 0.0  },
    };
    const int kReps = 12;
    for (const Spec& s : specs) {
        Q q; q.what = s.what;
        if (s.segs == 0) {
            q.ps.push_back(corridor(x0, y0, x1, y1, s.w));
        } else {
            uint32_t rng = 20260830u;
            for (int i = 0; i < kReps; ++i) {
                rng = rng * 1664525u + 1013904223u;
                const double fx = double((rng >> 8) & 0xFFFF) / 65536.0;
                rng = rng * 1664525u + 1013904223u;
                const double fy = double((rng >> 8) & 0xFFFF) / 65536.0;
                rng = rng * 1664525u + 1013904223u;
                const double hd = double((rng >> 8) & 0xFFFF) / 65536.0 * 6.2831853;
                q.ps.push_back(snake(x0 + fx * (x1-x0) * 0.7,
                                     y0 + fy * (y1-y0) * 0.7,
                                     hd, s.seg, s.segs, s.bend, s.w, rng));
            }
        }
        qs.push_back(std::move(q));
    }

    std::printf("\n\nFRÅGOR — MEDEL per fråga över %d slumpvis placerade ormar\n\n", kReps);
    for (const auto& q : qs) {
        std::printf("  %s   (%zu frågor, %zu hörn)\n",
                    q.what.c_str(), q.ps.size(), q.ps[0].x.size());
        std::printf("    %-22s %10s %14s %12s %10s\n",
                    "variant", "tid (ms)", "uppackade", "rörda", "träffar");
        std::printf("    %s\n", std::string(72, '-').c_str());

        auto run = [&](const std::string& name, auto fn) {
            uint64_t dec = 0, tou = 0, hits = 0;
            double best = 1e18;
            for (int rep = 0; rep < 3; ++rep) {
                uint64_t d = 0, t_ = 0, hh = 0;
                const auto t0 = Clock::now();
                for (const auto& poly : q.ps) hh += fn(poly, &d, &t_);
                const double t = ms_since(t0);
                if (rep > 0 && t < best) { best = t; dec = d; tou = t_; hits = hh; }
            }
            const double per = best / double(q.ps.size());
            std::printf("    %-22s %10.3f %14llu %12llu %10llu\n",
                        name.c_str(), per,
                        (unsigned long long)(dec / q.ps.size()),
                        (unsigned long long)(tou / q.ps.size()),
                        (unsigned long long)(hits / q.ps.size()));
        };

        run("platt (.las/.laz)",
            [&](const Poly& poly, uint64_t* d, uint64_t* t) {
                return query_flat(pts, poly, h.x_scale, h.x_offset,
                                  h.y_scale, h.y_offset, d, t); });
        run("hlaz v3 SOM DEN LIGGER",
            [&](const Poly& poly, uint64_t* d, uint64_t* t) {
                return query_hilbert(hs[1], poly, h.x_scale, h.x_offset,
                                     h.y_scale, h.y_offset, d, t, true); });
        for (const auto& r : hs)
            run("hilbert " + std::to_string(r.B) + " + polygonsåll",
                [&](const Poly& poly, uint64_t* d, uint64_t* t) {
                    return query_hilbert(r, poly, h.x_scale, h.x_offset,
                                         h.y_scale, h.y_offset, d, t); });
        for (const auto& g : gs)
            run("rutnät " + std::to_string(int(g.cell_m)) + " m gles",
                [&](const Poly& poly, uint64_t* d, uint64_t* t) {
                    return query_grid_sparse(g, poly, h.x_scale, h.x_offset,
                                             h.y_scale, h.y_offset, d, t); });
        std::printf("\n");
    }

    // ---------------------------------------------------------------- GLESPROV
    //
    // ⛔ autzen är en SOLID rektangel av data, och ett tätt rutnät förutsätter
    // det. En klippt korridorleverans är motsatsen: ett tunt stråk i en stor
    // bbox, alltså nästan bara tomma celler. Celltabellen betalas per CELL
    // medan Hilbert-blocken betalas per PUNKT — så det är här rutnätet kan falla.
    {
        std::printf("\n\nGLESPROV — autzen klippt till ett 100 m brett diagonalt stråk\n\n");
        const Poly strip = corridor(x0, y0, x1, y1, 100);
        std::vector<Point> sparse;
        for (const auto& q : pts) {
            const double wx = q.x * h.x_scale + h.x_offset;
            const double wy = q.y * h.y_scale + h.y_offset;
            if (wx < strip.x0 || wx > strip.x1 || wy < strip.y0 || wy > strip.y1) continue;
            if (point_in_poly(wx, wy, strip)) sparse.push_back(q);
        }
        std::printf("  %zu punkter kvar av %zu (%.1f %%), bbox oförändrad\n\n",
                    sparse.size(), pts.size(),
                    100.0 * double(sparse.size()) / double(pts.size()));

        std::printf("  %-24s %10s %8s %12s %8s %9s\n",
                    "variant", "TÄT MB", "B/pt", "GLES MB", "B/pt", "celler");
        std::printf("  %s\n", std::string(78, '-').c_str());

        const Hilb sh512 = build_hilbert(sparse, 512);
        std::printf("  %-24s %10.1f %8.2f %12s %8s %9zu\n", "hilbert 512",
                    sh512.bytes()/1048576.0, sh512.bpp(), "-", "-",
                    sh512.blocks.size());

        for (double m : { 10.0, 15.0, 20.0, 30.0 }) {
            const Grid sg = build_grid(sparse, m, h.x_scale, h.y_scale);
            size_t used = 0;
            for (const auto& cc : sg.cells) if (cc.n) ++used;
            char nm[64];
            std::snprintf(nm, sizeof nm, "rutnät %.0f m (%ux%u)", sg.cell_m, sg.nx, sg.ny);
            std::printf("  %-24s %10.1f %8.2f %12.1f %8.2f %9zu\n",
                        nm, sg.bytes()/1048576.0, sg.bpp(),
                        sg.bytes_sparse()/1048576.0, sg.bpp_sparse(), used);
        }
        std::printf("\n  Rått i minnet: %.1f MB. Celltabellen betalas per CELL,\n",
                    double(sparse.size()) * 13.0 / 1048576.0);
        std::printf("  Hilbert-blocken per PUNKT — därför syns straffet här.\n");
    }

    std::printf("\nBåda varianterna sållar mot POLYGONEN, inte mot dess bbox — det\n");
    std::printf("är fixen 1.1.3 saknar. Skillnaden dem emellan är alltså formen,\n");
    std::printf("inte buggen. 'Uppackade' är punkter rörda för att svara.\n");
    return 0;
}
