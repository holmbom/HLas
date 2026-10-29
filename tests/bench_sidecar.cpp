// LABORATORIUM för sidecar-formen. Inte ett test — inga påståenden, bara siffror.
//
// Frågan planen ska svara på: vilken form ska cachen ha? Se doc/plans/sidecar.md.
// Mätningarna där visade tre saker som styr allt annat:
//
//   * uppackning kostar ~400 ns per punkt, en skanning i rått minne under 1 ns
//   * cachen vann därför även på SMÅ frågor, trots att den saknar index
//   * men den är samtidigt större än filen den cachar
//
// Varianterna nedan krymper cachen utan att förlora det som gjorde den snabb:
// att den går att läsa rakt ur minnet.
//
// Kör:  ./build/tests/HLas_bench_sidecar las/autzen-classified.copc.laz

#include "HLas.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int bits_for(uint64_t range) { int b = 1; while (b < 64 && (range >> b)) ++b; return b; }

// ---- Bitströmmar ------------------------------------------------------------

struct BitWriter {
    std::vector<uint8_t>& out;
    uint64_t acc = 0;
    int      n = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint64_t v, int bits) {
        if (bits <= 0) return;
        acc |= (v & ((bits >= 64) ? ~0ull : ((1ull << bits) - 1))) << n;
        n += bits;
        while (n >= 8) { out.push_back(uint8_t(acc & 0xFF)); acc >>= 8; n -= 8; }
    }
    void flush() { if (n > 0) { out.push_back(uint8_t(acc & 0xFF)); acc = 0; n = 0; } }
};

// Läser upp till 57 bitar. Hämtar alltid 64 bitar, så bufferten måste ha slack
// på slutet — varje variant lägger till åtta nollbyte.
inline uint64_t bitread(const uint8_t* p, uint64_t bitpos, int bits) {
    if (bits <= 0) return 0;
    uint64_t v = 0;
    std::memcpy(&v, p + (bitpos >> 3), 8);
    v >>= (bitpos & 7);
    return v & ((bits >= 64) ? ~0ull : ((1ull << bits) - 1));
}

struct Point { int32_t x, y, z; uint8_t c; };

// ---- Morton ----------------------------------------------------------------

uint64_t morton3(uint32_t x, uint32_t y, uint32_t z) {
    auto spread = [](uint32_t v) -> uint64_t {
        uint64_t r = v & 0x1FFFFF;
        r = (r | r << 32) & 0x1F00000000FFFFull;
        r = (r | r << 16) & 0x1F0000FF0000FFull;
        r = (r | r << 8)  & 0x100F00F00F00F00Full;
        r = (r | r << 4)  & 0x10C30C30C30C30C3ull;
        r = (r | r << 2)  & 0x1249249249249249ull;
        return r;
    };
    return spread(x) | (spread(y) << 1) | (spread(z) << 2);
}

// Hilbert-kurvan, 3D, enligt Skillings algoritm.
//
// Morton hoppar långt i rummet vid sina sömmar; Hilbert har inga sådana hopp och
// borde ge tätare block. Priset är att den kostar mer att räkna — frågan är hur
// mycket tätare den faktiskt blir.
uint64_t hilbert3(uint32_t x, uint32_t y, uint32_t z, int bits) {
    uint32_t X[3] = { x, y, z };
    uint32_t M = 1u << (bits - 1), P, Q, t;

    for (Q = M; Q > 1; Q >>= 1) {
        P = Q - 1;
        for (int i = 0; i < 3; ++i) {
            if (X[i] & Q) X[0] ^= P;
            else { t = (X[0] ^ X[i]) & P; X[0] ^= t; X[i] ^= t; }
        }
    }
    for (int i = 1; i < 3; ++i) X[i] ^= X[i-1];
    t = 0;
    for (Q = M; Q > 1; Q >>= 1) if (X[2] & Q) t ^= Q - 1;
    for (int i = 0; i < 3; ++i) X[i] ^= t;

    // Väv ihop till ett tal, mest signifikanta biten först.
    uint64_t key = 0;
    for (int b = bits - 1; b >= 0; --b)
        for (int i = 0; i < 3; ++i)
            key = (key << 1) | ((X[i] >> b) & 1);
    return key;
}

// ---- Varianterna ------------------------------------------------------------

struct Flat {
    std::string          name;
    std::vector<uint8_t> data;
    double               build_ms = 0;
    uint64_t             count = 0;
    int                  layout = 0;   // 0 = postvis 13 B, 1 = postvis packat, 2 = kolumnvis
    int32_t              min_x = 0, min_y = 0, min_z = 0;
    int                  bx = 32, by = 32, bz = 32;
    uint64_t             col_x = 0, col_y = 0;
    int stride() const { return bx + by + bz + 8; }
    double mb() const { return double(data.size()) / 1048576.0; }
    double bpp() const { return double(data.size()) / double(count); }
};

// Morton-sorterat, frame-of-reference per block, med blockets bbox som index.
//
// Indexet byggs inte vid sidan om: FOR MÅSTE lagra blockets min för att kunna
// packa bitarna, så bbox:en faller ut av komprimeringen.
struct MortonFor {
    struct Block {
        int32_t  min_x, min_y, min_z;
        int32_t  max_x, max_y;          // för sållningen; max_z behövs inte i 2D
        uint8_t  bx, by, bz;
        uint64_t bitpos;
        uint32_t first, n;
    };
    std::string          name;
    std::vector<Block>   blocks;
    std::vector<uint8_t> data;
    double               build_ms = 0;
    uint64_t             count = 0;
    uint32_t             B = 256;

    // Vad ett blockhuvud kostar PÅ DISK, inte i minnet. `first` och `n` följer
    // av blockstorleken, och `bitpos` är en prefixsumma som räknas fram vid
    // öppning — inget av dem behöver lagras. Kvar blir min x/y/z, max x/y och
    // tre bitbredder.
    static constexpr size_t kDiskHeader = 5 * 4 + 3;
    double mb() const {
        return double(data.size() + blocks.size() * kDiskHeader) / 1048576.0;
    }
    double bpp() const { return mb() * 1048576.0 / double(count); }
};

Flat build_rows13(const std::vector<Point>& p) {
    Flat r; r.name = "postvis 13 B (dagens)"; r.layout = 0; r.count = p.size();
    auto t0 = Clock::now();
    r.data.resize(p.size() * 13 + 8);
    for (size_t i = 0; i < p.size(); ++i) {
        uint8_t* d = r.data.data() + i * 13;
        std::memcpy(d, &p[i].x, 4); std::memcpy(d + 4, &p[i].y, 4);
        std::memcpy(d + 8, &p[i].z, 4); d[12] = p[i].c;
    }
    r.build_ms = ms(t0, Clock::now());
    return r;
}

void global_quant(const std::vector<Point>& p, int32_t& mnx, int32_t& mny, int32_t& mnz,
                  int& bx, int& by, int& bz) {
    int32_t x0=INT32_MAX,y0=INT32_MAX,z0=INT32_MAX,x1=INT32_MIN,y1=INT32_MIN,z1=INT32_MIN;
    for (const Point& q : p) {
        x0=std::min(x0,q.x); x1=std::max(x1,q.x);
        y0=std::min(y0,q.y); y1=std::max(y1,q.y);
        z0=std::min(z0,q.z); z1=std::max(z1,q.z);
    }
    mnx=x0; mny=y0; mnz=z0;
    bx=bits_for(uint64_t(int64_t(x1)-x0));
    by=bits_for(uint64_t(int64_t(y1)-y0));
    bz=bits_for(uint64_t(int64_t(z1)-z0));
}

Flat build_rows_packed(const std::vector<Point>& p) {
    Flat r; r.name = "postvis bitpackat"; r.layout = 1; r.count = p.size();
    global_quant(p, r.min_x, r.min_y, r.min_z, r.bx, r.by, r.bz);
    auto t0 = Clock::now();
    r.data.reserve(p.size() * size_t((r.stride() + 7) / 8) + 16);
    BitWriter w(r.data);
    for (const Point& q : p) {
        w.put(uint64_t(int64_t(q.x) - r.min_x), r.bx);
        w.put(uint64_t(int64_t(q.y) - r.min_y), r.by);
        w.put(uint64_t(int64_t(q.z) - r.min_z), r.bz);
        w.put(q.c, 8);
    }
    w.flush();
    r.data.resize(r.data.size() + 8, 0);
    r.build_ms = ms(t0, Clock::now());
    return r;
}

Flat build_columns(const std::vector<Point>& p) {
    Flat r; r.name = "kolumnvis bitpackat"; r.layout = 2; r.count = p.size();
    global_quant(p, r.min_x, r.min_y, r.min_z, r.bx, r.by, r.bz);
    const uint64_t n = p.size();
    auto col_bytes = [n](int bits) { return size_t((n * bits + 7) / 8) + 8; };
    auto t0 = Clock::now();
    const size_t ox = 0, oy = col_bytes(r.bx), oz = oy + col_bytes(r.by),
                 oc = oz + col_bytes(r.bz);
    r.data.assign(oc + col_bytes(8), 0);
    r.col_x = uint64_t(ox) * 8; r.col_y = uint64_t(oy) * 8;
    auto col = [&](size_t off, int bits, auto get) {
        std::vector<uint8_t> tmp; tmp.reserve(col_bytes(bits));
        BitWriter w(tmp);
        for (uint64_t i = 0; i < n; ++i) w.put(get(i), bits);
        w.flush();
        std::memcpy(r.data.data() + off, tmp.data(), tmp.size());
    };
    col(ox, r.bx, [&](uint64_t i){ return uint64_t(int64_t(p[i].x) - r.min_x); });
    col(oy, r.by, [&](uint64_t i){ return uint64_t(int64_t(p[i].y) - r.min_y); });
    col(oz, r.bz, [&](uint64_t i){ return uint64_t(int64_t(p[i].z) - r.min_z); });
    col(oc, 8,    [&](uint64_t i){ return uint64_t(p[i].c); });
    r.build_ms = ms(t0, Clock::now());
    return r;
}

MortonFor build_sorted_for(std::vector<Point> p, uint32_t B, bool hilbert) {
    MortonFor r;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s FOR block %u", hilbert ? "hilbert" : "morton", B);
    r.name = buf; r.B = B; r.count = p.size();
    auto t0 = Clock::now();

    int32_t gx, gy, gz; int bx, by, bz;
    global_quant(p, gx, gy, gz, bx, by, bz);
    const int kb = std::max({bx, by, bz});
    std::vector<uint64_t> key(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        const uint32_t ux = uint32_t(int64_t(p[i].x)-gx), uy = uint32_t(int64_t(p[i].y)-gy),
                       uz = uint32_t(int64_t(p[i].z)-gz);
        key[i] = hilbert ? hilbert3(ux, uy, uz, kb) : morton3(ux, uy, uz);
    }
    std::vector<uint32_t> ord(p.size());
    for (size_t i = 0; i < p.size(); ++i) ord[i] = uint32_t(i);
    std::sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b){ return key[a] < key[b]; });
    std::vector<Point> s(p.size());
    for (size_t i = 0; i < p.size(); ++i) s[i] = p[ord[i]];

    BitWriter w(r.data);
    uint64_t bitpos = 0;
    for (uint64_t st = 0; st < s.size(); st += B) {
        const uint64_t e = std::min<uint64_t>(st + B, s.size());
        MortonFor::Block b{};
        int32_t x0=INT32_MAX,y0=INT32_MAX,z0=INT32_MAX,x1=INT32_MIN,y1=INT32_MIN,z1=INT32_MIN;
        for (uint64_t i = st; i < e; ++i) {
            x0=std::min(x0,s[i].x); x1=std::max(x1,s[i].x);
            y0=std::min(y0,s[i].y); y1=std::max(y1,s[i].y);
            z0=std::min(z0,s[i].z); z1=std::max(z1,s[i].z);
        }
        b.min_x=x0; b.min_y=y0; b.min_z=z0; b.max_x=x1; b.max_y=y1;
        b.bx=uint8_t(bits_for(uint64_t(int64_t(x1)-x0)));
        b.by=uint8_t(bits_for(uint64_t(int64_t(y1)-y0)));
        b.bz=uint8_t(bits_for(uint64_t(int64_t(z1)-z0)));
        b.bitpos = bitpos; b.first = uint32_t(st); b.n = uint32_t(e - st);
        for (uint64_t i = st; i < e; ++i) {
            w.put(uint64_t(int64_t(s[i].x)-x0), b.bx);
            w.put(uint64_t(int64_t(s[i].y)-y0), b.by);
            w.put(uint64_t(int64_t(s[i].z)-z0), b.bz);
            w.put(s[i].c, 8);
        }
        bitpos += uint64_t(b.n) * (b.bx + b.by + b.bz + 8);
        r.blocks.push_back(b);
    }
    w.flush();
    r.data.resize(r.data.size() + 8, 0);
    r.build_ms = ms(t0, Clock::now());
    return r;
}

// ---- Frågor ----------------------------------------------------------------

struct Rect { int64_t x0, y0, x1, y1; };

uint64_t query_flat(const Flat& r, const Rect& q, uint32_t step) {
    uint64_t hits = 0;
    const uint8_t* d = r.data.data();
    if (r.layout == 0) {
        for (uint64_t i = 0; i < r.count; i += step) {
            int32_t x, y;
            std::memcpy(&x, d + i*13, 4); std::memcpy(&y, d + i*13 + 4, 4);
            if (x >= q.x0 && x <= q.x1 && y >= q.y0 && y <= q.y1) ++hits;
        }
    } else if (r.layout == 1) {
        const int st = r.stride();
        for (uint64_t i = 0; i < r.count; i += step) {
            const uint64_t base = i * st;
            const int64_t x = int64_t(bitread(d, base, r.bx)) + r.min_x;
            if (x < q.x0 || x > q.x1) continue;                 // tidig utgång
            const int64_t y = int64_t(bitread(d, base + r.bx, r.by)) + r.min_y;
            if (y >= q.y0 && y <= q.y1) ++hits;
        }
    } else {
        for (uint64_t i = 0; i < r.count; i += step) {
            const int64_t x = int64_t(bitread(d, r.col_x + i*r.bx, r.bx)) + r.min_x;
            if (x < q.x0 || x > q.x1) continue;                 // tidig utgång
            const int64_t y = int64_t(bitread(d, r.col_y + i*r.by, r.by)) + r.min_y;
            if (y >= q.y0 && y <= q.y1) ++hits;
        }
    }
    return hits;
}

// Vilka block en fråga rör. Behövs för att mäta hur mycket två bildrutor i rad
// har gemensamt — alltså hur mycket en riktig vy skulle kunna återanvända.
void blocks_for(const MortonFor& r, const Rect& q, std::vector<uint32_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < r.blocks.size(); ++i) {
        const auto& b = r.blocks[i];
        if (b.max_x < q.x0 || b.min_x > q.x1 || b.max_y < q.y0 || b.min_y > q.y1)
            continue;
        out.push_back(i);
    }
}

uint64_t query_morton(const MortonFor& r, const Rect& q, uint32_t step,
                      uint64_t* blocks_touched, uint64_t* points_decoded) {
    uint64_t hits = 0;
    const uint8_t* d = r.data.data();
    for (const auto& b : r.blocks) {
        // Blockets bbox mot frågan. Det här ÄR indexet.
        if (b.max_x < q.x0 || b.min_x > q.x1 || b.max_y < q.y0 || b.min_y > q.y1)
            continue;
        ++*blocks_touched;
        const int stride = b.bx + b.by + b.bz + 8;
        for (uint32_t k = 0; k < b.n; k += step) {
            const uint64_t base = b.bitpos + uint64_t(k) * stride;
            const int64_t x = int64_t(bitread(d, base, b.bx)) + b.min_x;
            if (x < q.x0 || x > q.x1) continue;
            const int64_t y = int64_t(bitread(d, base + b.bx, b.by)) + b.min_y;
            if (y >= q.y0 && y <= q.y1) ++hits;
        }
        *points_decoded += (b.n + step - 1) / step;
    }
    return hits;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "bruk: %s <fil.laz|fil.copc.laz> [antal-frågor]\n", argv[0]);
        return 1;
    }
    const int K = (argc > 2) ? std::atoi(argv[2]) : 20;

    HLas::Reader rd;
    if (rd.open(argv[1]) != HLas::Status::Ok) {
        std::fprintf(stderr, "open: %s\n", rd.last_error().c_str());
        return 1;
    }
    const auto& h = rd.header();
    const auto v = rd.points();
    const uint64_t n = v.count();

    std::vector<Point> pts(n);
    for (uint64_t i = 0; i < n; ++i)
        pts[i] = { v.raw_x(i), v.raw_y(i), v.raw_z(i), v.classification(i) };
    // Läsaren håller hela filen uppackad — 30 byte per punkt. På en stor fil är
    // det flera gigabyte som inte behövs när posterna väl är kopierade.
    const HLas::Header hh = h;      // kopia, headern behövs efter close()
    rd.close();

    double src_mb = 0;
    if (std::FILE* f = std::fopen(argv[1], "rb")) {
        std::fseek(f, 0, SEEK_END);
        src_mb = double(std::ftell(f)) / 1048576.0;
        std::fclose(f);
    }
    std::printf("fil: %s\n  %llu punkter, punktformat %u, %u byte per post, källan %.1f MB\n\n",
                argv[1], (unsigned long long)n, hh.point_format, hh.point_record_length, src_mb);

    // argv[3]: "full" kör hela svepet, en kommaseparerad lista blockstorlekar
    // kör bara Hilbert med dem och hoppar över de platta varianterna — deras
    // frågor tar minuter på en stor fil och de är ändå bara referens.
    const std::string mode = (argc > 3) ? argv[3] : "";
    const bool full = (mode == "full");
    std::vector<uint32_t> want;
    if (!mode.empty() && !full) {
        size_t p0 = 0;
        while (p0 < mode.size()) {
            size_t p1 = mode.find(',', p0);
            if (p1 == std::string::npos) p1 = mode.size();
            want.push_back(uint32_t(std::stoul(mode.substr(p0, p1 - p0))));
            p0 = p1 + 1;
        }
    }

    std::vector<Flat> flats;
    if (want.empty()) {
        flats.push_back(build_rows13(pts));
        flats.push_back(build_rows_packed(pts));
        flats.push_back(build_columns(pts));
    }
    std::vector<MortonFor> sorted;
    if (!want.empty()) {
        for (uint32_t B : want) sorted.push_back(build_sorted_for(pts, B, true));
    } else if (full) {
        sorted.push_back(build_sorted_for(pts, 128, false));
        sorted.push_back(build_sorted_for(pts, 256, false));
        sorted.push_back(build_sorted_for(pts, 512, false));
        sorted.push_back(build_sorted_for(pts, 128, true));
        sorted.push_back(build_sorted_for(pts, 256, true));
        sorted.push_back(build_sorted_for(pts, 512, true));
        sorted.push_back(build_sorted_for(pts, 1024, true));
        sorted.push_back(build_sorted_for(pts, 2048, true));
        sorted.push_back(build_sorted_for(pts, 4096, true));
    } else {
        sorted.push_back(build_sorted_for(pts, 512, false));
        sorted.push_back(build_sorted_for(pts, 256, true));
        sorted.push_back(build_sorted_for(pts, 512, true));
        sorted.push_back(build_sorted_for(pts, 1024, true));
    }
    const MortonFor& mf = sorted[0];

    std::printf("  %-24s %10s %10s %10s\n", "variant", "bygg", "storlek", "per punkt");
    for (const auto& f : flats)
        std::printf("  %-24s %8.0f ms %8.1f MB %7.2f B\n",
                    f.name.c_str(), f.build_ms, f.mb(), f.bpp());
    for (const auto& s : sorted)
        std::printf("  %-24s %8.0f ms %8.1f MB %7.2f B   (%zu block)\n",
                    s.name.c_str(), s.build_ms, s.mb(), s.bpp(), s.blocks.size());
    std::printf("\n");

    // Samma rutor till alla varianter.
    const double fw = hh.max_x - hh.min_x, fh = hh.max_y - hh.min_y;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> u(0.05, 0.95);
    auto to_ix = [&](double w){ return int64_t((w - hh.x_offset) / hh.x_scale); };
    auto to_iy = [&](double w){ return int64_t((w - hh.y_offset) / hh.y_scale); };
    std::vector<Rect> qs;
    const double side = 0.02;
    for (int i = 0; i < K; ++i) {
        const double cx = hh.min_x + fw*u(rng), cy = hh.min_y + fh*u(rng);
        qs.push_back({ to_ix(cx - fw*side/2), to_iy(cy - fh*side/2),
                       to_ix(cx + fw*side/2), to_iy(cy + fh*side/2) });
    }

    // Uppvärmning FÖRE tidtagning. Utan den mäter den första varianten
    // sidfelen och de följande gör det inte — vilket gjorde tabellerna i den
    // första körningen ojämförbara.
    for (const auto& f : flats) (void)query_flat(f, qs[0], 1);
    for (const auto& s : sorted) { uint64_t b=0,p=0; (void)query_morton(s, qs[0], 1, &b, &p); }

    std::printf("  %d rutor, var och en %.2f %% av ytan, full täthet:\n", K, 100*side*side);
    std::printf("  %-24s %12s %12s %s\n", "variant", "per fråga", "träffar", "rört");
    for (const auto& f : flats) {
        uint64_t hits = 0;
        auto t0 = Clock::now();
        for (const Rect& q : qs) hits += query_flat(f, q, 1);
        std::printf("  %-24s %9.3f ms %12llu  alla %llu punkter\n",
                    f.name.c_str(), ms(t0, Clock::now())/K,
                    (unsigned long long)hits, (unsigned long long)f.count);
    }
    for (const auto& s : sorted) {
        uint64_t hits = 0, blocks = 0, decoded = 0;
        auto t0 = Clock::now();
        for (const Rect& q : qs) hits += query_morton(s, q, 1, &blocks, &decoded);
        std::printf("  %-24s %9.3f ms %12llu  %llu av %zu block, %llu punkter\n",
                    s.name.c_str(), ms(t0, Clock::now())/K, (unsigned long long)hits,
                    (unsigned long long)(blocks/K), s.blocks.size(),
                    (unsigned long long)(decoded/K));
    }
    std::printf("\n");

    std::printf("  gallrad läsning ur samma rutor (3D-vyns behov):\n");
    std::printf("  %-24s %11s %11s %11s\n", "variant", "steg 1", "steg 4", "steg 16");
    const uint32_t steps[3] = { 1, 4, 16 };
    for (const auto& f : flats) {
        double t[3]; uint64_t hh[3];
        for (int s = 0; s < 3; ++s) {
            // Summan MÅSTE användas. Kastas den med (void) kan kompilatorn
            // bevisa att anropet saknar sidoeffekter och stryka hela loopen —
            // vilket den gjorde, och tabellen visade 0,000 ms.
            uint64_t hits = 0;
            auto t0 = Clock::now();
            for (const Rect& q : qs) hits += query_flat(f, q, steps[s]);
            t[s] = ms(t0, Clock::now())/K; hh[s] = hits/K;
        }
        std::printf("  %-24s %8.3f ms %8.3f ms %8.3f ms   (träffar %llu / %llu / %llu)\n",
                    f.name.c_str(), t[0], t[1], t[2],
                    (unsigned long long)hh[0], (unsigned long long)hh[1],
                    (unsigned long long)hh[2]);
    }
    {
        double t[3]; uint64_t hh[3];
        for (int s = 0; s < 3; ++s) {
            uint64_t hits = 0, b = 0, p = 0;
            auto t0 = Clock::now();
            for (const Rect& q : qs) hits += query_morton(mf, q, steps[s], &b, &p);
            t[s] = ms(t0, Clock::now())/K; hh[s] = hits/K;
        }
        std::printf("  %-24s %8.3f ms %8.3f ms %8.3f ms   (träffar %llu / %llu / %llu)\n",
                    mf.name.c_str(), t[0], t[1], t[2],
                    (unsigned long long)hh[0], (unsigned long long)hh[1],
                    (unsigned long long)hh[2]);
    }
    // ---- Den vandrande vyn ---------------------------------------------------
    //
    // 3D-vyns verkliga mönster, som INTE är utspridda rutor: kameran går framåt
    // och ser ett område framför sig, tätt nära och glest långt bort. Två
    // bildrutor i rad överlappar kraftigt, vilket en riktig vy skulle utnyttja.
    //
    // Banden är rutor framför kameran, inte en riktig frustum — men de fångar
    // det som avgör kostnaden: hur mycket som måste läsas, och hur tätt.
    {
        struct Band { double ahead, half, step; };
        const Band bands[3] = {
            {  25.0,  25.0,  1 },   // nära, full täthet
            {  75.0,  50.0,  4 },   // mellan
            { 150.0, 100.0, 16 },   // fjärran, glest
        };
        const int frames = 200;
        // Går diagonalt tvärs över filen.
        const double x0 = hh.min_x + fw*0.2, y0 = hh.min_y + fh*0.2;
        const double x1 = hh.min_x + fw*0.8, y1 = hh.min_y + fh*0.8;
        const double len = std::sqrt((x1-x0)*(x1-x0) + (y1-y0)*(y1-y0));
        const double dx = (x1-x0)/len, dy = (y1-y0)/len;

        std::printf("\n  VANDRANDE VY: %d bildrutor, %.0f m färdväg, tre avståndsband\n",
                    frames, len);
        std::printf("    nära 0-50 m full täthet, mellan var 4:e, fjärran var 16:e\n\n");

        auto run = [&](auto&& query, const char* label, bool track_blocks) {
            double worst = 0, total = 0;
            uint64_t pts = 0;
            std::vector<uint32_t> prev, cur;
            uint64_t shared = 0, seen = 0;
            for (int f = 0; f < frames; ++f) {
                const double t = double(f) / (frames - 1);
                const double px = x0 + (x1-x0)*t, py = y0 + (y1-y0)*t;
                auto t0 = Clock::now();
                uint64_t hits = 0;
                for (const Band& b : bands) {
                    const double cx = px + dx*b.ahead, cy = py + dy*b.ahead;
                    Rect q{ to_ix(cx - b.half), to_iy(cy - b.half),
                            to_ix(cx + b.half), to_iy(cy + b.half) };
                    hits += query(q, uint32_t(b.step));
                }
                const double dt = ms(t0, Clock::now());
                total += dt; worst = std::max(worst, dt); pts += hits;
                if (track_blocks) {
                    // Överlappet mellan den här bildrutan och den förra.
                    const double cx = px + dx*bands[2].ahead, cy = py + dy*bands[2].ahead;
                    Rect q{ to_ix(cx - bands[2].half), to_iy(cy - bands[2].half),
                            to_ix(cx + bands[2].half), to_iy(cy + bands[2].half) };
                    blocks_for(mf, q, cur);
                    if (f > 0) {
                        uint64_t s = 0;
                        for (uint32_t b : cur)
                            if (std::binary_search(prev.begin(), prev.end(), b)) ++s;
                        shared += s; seen += cur.size();
                    }
                    prev = cur;
                }
            }
            std::printf("  %-24s medel %7.3f ms   värst %7.3f ms   %llu punkter/bildruta\n",
                        label, total/frames, worst, (unsigned long long)(pts/frames));
            if (track_blocks && seen)
                std::printf("  %-24s %.0f %% av blocken är samma som föregående bildruta\n",
                            "", 100.0*double(shared)/double(seen));
        };

        for (const auto& f : flats)
            run([&](const Rect& q, uint32_t st){ return query_flat(f, q, st); },
                f.name.c_str(), false);
        for (const auto& s : sorted)
            run([&](const Rect& q, uint32_t st){
                    uint64_t b = 0, p = 0; return query_morton(s, q, st, &b, &p);
                }, s.name.c_str(), &s == &sorted.back());

        std::printf("\n  (CopcReader mättes tidigare till 111,5 ms för EN ruta av den här\n"
                    "   storleken — alltså sju gånger en hel bildrutebudget.)\n");
    }

    return 0;
}
