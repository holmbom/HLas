// Håller budget-uppskattningen för en SNED KORRIDOR? — mätning inför 1.1.4.
//
// Budget-läget måste välja gallringen FÖRE passet, och det enda den har att gå
// på är blocksållningen. Men sållningen är BBOX-baserad medan frågan är en
// POLYGON — och för en sned korridor är bboxen enormt mycket större än
// korridoren. Uppskattningen blir då för hög, striden för aggressiv, och
// anroparen får en bråkdel av sin budget.
//
// Två fel mäts här, och de är olika saker:
//
//   1. TÄCKNINGSFELET. Hur många fler punkter ligger i korridorens bbox än i
//      korridoren? Det är storleksordningen på hur mycket blocksållningen
//      överskattar, och det drabbar GLOBAL budget.
//
//   2. PROXYFELET. Budget per klass skattar klassens antal i området som
//      "andelen i hela filen". Håller den andelen inne i en korridor? Det mäts
//      genom att jämföra helfilshistogrammet med polygonhistogrammet — bägge
//      finns i API:t, inga internddelar behövs.
//
// Metodfällor: uppvärmning, kontrollsumma, varianter i samma process
// konkurrerar om cachen.

#include "HLas.h"
#include "test_data.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool exists(const char* p) {
    std::FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

struct Poly { std::vector<double> x, y; };

// En ORM: buffrad polyline, alltså den verkliga frågeformen — en kraftledning
// svänger och offseten ger korridoren. 8-18 hörn, inte fyra.
Poly snake(double sx, double sy, double heading, double seg, int segs,
           double bend, double w, uint32_t seed) {
    std::vector<double> px, py;
    double x = sx, y = sy, hd = heading;
    px.push_back(x); py.push_back(y);
    uint32_t rng = seed ? seed : 1u;
    auto nxt = [&]{ rng = rng*1664525u + 1013904223u;
                    return double(rng >> 8) / double(1u << 24); };
    for (int i = 0; i < segs; ++i) {
        hd += (nxt()*2.0 - 1.0) * bend;
        x += std::cos(hd)*seg; y += std::sin(hd)*seg;
        px.push_back(x); py.push_back(y);
    }
    const size_t n = px.size();
    std::vector<double> nx(n), ny(n);
    for (size_t i = 0; i < n; ++i) {
        double dx = 0, dy = 0;
        if (i + 1 < n) { dx += px[i+1]-px[i]; dy += py[i+1]-py[i]; }
        if (i > 0)     { dx += px[i]-px[i-1]; dy += py[i]-py[i-1]; }
        const double L = std::sqrt(dx*dx + dy*dy);
        nx[i] = -dy/L; ny[i] = dx/L;
    }
    Poly p;
    for (size_t i = 0; i < n; ++i) { p.x.push_back(px[i]+nx[i]*w*0.5);
                                     p.y.push_back(py[i]+ny[i]*w*0.5); }
    for (size_t i = n; i-- > 0; )  { p.x.push_back(px[i]-nx[i]*w*0.5);
                                     p.y.push_back(py[i]-ny[i]*w*0.5); }
    return p;
}

// En korridor: rektangel av bredd `w` längs linjen (ax,ay)-(bx,by).
// Är linjen sned blir polygonen sned, och bboxen mycket större än polygonen.
Poly corridor(double ax, double ay, double bx, double by, double w) {
    const double dx = bx - ax, dy = by - ay;
    const double len = std::sqrt(dx * dx + dy * dy);
    const double nx = -dy / len * (w * 0.5);
    const double ny =  dx / len * (w * 0.5);
    Poly p;
    p.x = { ax + nx, bx + nx, bx - nx, ax - nx };
    p.y = { ay + ny, by + ny, by - ny, ay - ny };
    return p;
}

Poly bbox_of(const Poly& p) {
    double x0 = p.x[0], x1 = p.x[0], y0 = p.y[0], y1 = p.y[0];
    for (int i = 1; i < 4; ++i) {
        x0 = std::min(x0, p.x[i]); x1 = std::max(x1, p.x[i]);
        y0 = std::min(y0, p.y[i]); y1 = std::max(y1, p.y[i]);
    }
    return Poly{ { x0, x1, x1, x0 }, { y0, y0, y1, y1 } };
}

} // namespace

int main(int argc, char** argv) {
    std::printf("Sned korridor — håller budget-uppskattningen?\n");
    std::printf("=============================================\n\n");

    const char* src = (argc > 1) ? argv[1] : testdata::kLaz;
    if (!exists(src)) { std::printf("SKIP: saknar %s\n", src); return 0; }
    std::printf("fil: %s\n\n", src);

    const std::string cache = HLas::SidecarPathFor(src);
    bool built = false;
    if (!exists(cache.c_str())) {
        std::printf("bygger cache ...\n");
        if (HLas::BuildSidecar(src) != HLas::Status::Ok) return 1;
        built = true;
    }

    HLas::Reader r;
    if (r.open(src, HLas::Cache::Fast) != HLas::Status::Ok) return 1;
    const HLas::Header& h = r.header();

    // Samma korridor mot den OCACHADE vägen. För en .laz betyder det full
    // uppackning; finns en .las-syskonfil används den i stället, eftersom den
    // är mmap:bar och inte kräver gigabyte i minnet.
    std::string flat_path = src;
    { const size_t d = flat_path.rfind(".laz");
      if (d != std::string::npos) flat_path.replace(d, 4, ".las"); }
    HLas::Reader r_flat;
    bool have_flat = false;
    if (exists(flat_path.c_str())) {
        have_flat = r_flat.open(flat_path.c_str(), HLas::Cache::Off) == HLas::Status::Ok;
    } else {
        // Ingen .las-syskonfil — packa upp källan i stället. Dyrt i både tid och
        // minne, men det är just den kostnaden jämförelsen ska visa.
        std::printf("(ingen .las — packar upp %s för jämförelsen)\n", src);
        const auto t0 = Clock::now();
        have_flat = r_flat.open(src, HLas::Cache::Off) == HLas::Status::Ok;
        if (have_flat) std::printf("  uppackning: %.1f s\n\n", ms_since(t0) / 1000.0);
    }

    const uint64_t cap = h.point_count;
    std::vector<double> bx(cap), by(cap), bz(cap);
    std::vector<uint8_t> bc(cap);

    uint64_t whole[256] = {};
    if (r.get_classification_histogram(whole) != HLas::Status::Ok) return 1;
    uint64_t whole_total = 0;
    for (int c = 0; c < 256; ++c) whole_total += whole[c];

    std::printf("autzen: %llu punkter, %.0f x %.0f m\n\n",
                (unsigned long long)whole_total,
                h.max_x - h.min_x, h.max_y - h.min_y);

    // Korridorer: diagonalen genom filen, plus en axelparallell som referens.
    struct Case { const char* what; double ax, ay, bx, by, w; };
    const double x0 = h.min_x, y0 = h.min_y, x1 = h.max_x, y1 = h.max_y;
    const Case cases[] = {
        { "diagonal, 20 m",   x0, y0, x1, y1,  20 },
        { "diagonal, 50 m",   x0, y0, x1, y1,  50 },
        { "diagonal, 100 m",  x0, y0, x1, y1, 100 },
        { "diagonal, 200 m",  x0, y0, x1, y1, 200 },
        { "30 grader, 50 m",  x0, y0, x1, y0 + (y1-y0)*0.55, 50 },
        { "axelparallell, 50 m", x0, (y0+y1)/2, x1, (y0+y1)/2, 50 },
    };

    struct Spec { const char* what; double w, seg; int segs; double bend; };
    const Spec specs[] = {
        { "orm 12 m x ~300 m",   12, 100, 3, 0.35 },
        { "orm 50 m x ~300 m",   50, 100, 3, 0.35 },
        { "orm 50 m x ~1 km",    50, 200, 5, 0.30 },
        { "orm 50 m x ~3 km",    50, 400, 8, 0.25 },
        { "orm 200 m x ~1 km",  200, 200, 5, 0.30 },
    };
    const double X0 = h.min_x, Y0 = h.min_y, X1 = h.max_x, Y1 = h.max_y;
    constexpr int kReps = 12;

    std::printf("MEDEL PER FRÅGA — %d slumpvis placerade ormar\n\n", kReps);
    std::printf("  %-22s %12s %12s %10s %12s\n",
                "fråga", "platt (ms)", "v4 (ms)", "kvot", "träffar");
    std::printf("  %s\n", std::string(74, '-').c_str());

    double sink = 0;
    for (const Spec& s : specs) {
        std::vector<Poly> ps;
        uint32_t rng = 20260830u;
        for (int i = 0; i < kReps; ++i) {
            rng = rng*1664525u + 1013904223u;
            const double fx = double((rng >> 8) & 0xFFFF) / 65536.0;
            rng = rng*1664525u + 1013904223u;
            const double fy = double((rng >> 8) & 0xFFFF) / 65536.0;
            rng = rng*1664525u + 1013904223u;
            const double hd = double((rng >> 8) & 0xFFFF) / 65536.0 * 6.2831853;
            ps.push_back(snake(X0 + fx*(X1-X0)*0.7, Y0 + fy*(Y1-Y0)*0.7,
                               hd, s.seg, s.segs, s.bend, s.w, rng));
        }

        auto run = [&](HLas::Reader& rd) {
            double best = 1e18; uint64_t hits = 0;
            for (int rep = 0; rep < 3; ++rep) {
                uint64_t hh = 0;
                const auto t0 = Clock::now();
                for (const auto& p : ps)
                    hh += rd.get_xyzc_polygon(0, cap, p.x.data(), p.y.data(),
                                              uint32_t(p.x.size()),
                                              bx.data(), by.data(), bz.data(),
                                              bc.data());
                const double t = ms_since(t0);
                if (rep > 0 && t < best) { best = t; hits = hh; }
                for (uint64_t i = 0; i < hh && i < cap; i += 9973) sink += bz[i];
            }
            return std::pair<double,uint64_t>{ best / double(kReps), hits / kReps };
        };

        const auto flat  = have_flat ? run(r_flat) : std::pair<double,uint64_t>{-1,0};
        const auto cached = run(r);
        if (have_flat && flat.second != cached.second)
            std::printf("  ⛔ %s: platt gav %llu, cachen %llu\n", s.what,
                        (unsigned long long)flat.second,
                        (unsigned long long)cached.second);
        std::printf("  %-22s %12.3f %12.3f %9.1fx %12llu\n",
                    s.what, flat.first, cached.first,
                    cached.first > 0 ? flat.first / cached.first : 0.0,
                    (unsigned long long)cached.second);
    }
    std::printf("\n  (kontrollsumma %.0f)\n", sink);

    if (built) std::remove(cache.c_str());
    return 0;
}
