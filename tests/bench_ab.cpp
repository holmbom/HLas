// A/B: 1.1.3 mot 1.1.4, samma kod byggd i båda träden.
//
// Använder BARA API som finns i båda, så skillnaden som mäts är
// implementationen och inte anropet. Ingen stride här — den finns bara i 1.1.4
// och skulle göra jämförelsen ojämförbar.
//
// Frågeformen är en ORM: en buffrad polyline med 8-18 hörn, alltså den form en
// korridorfråga faktiskt har. En rak fyrhörning hade dolt både polygonsållningen och
// kostnaden för hörnrika polygoner.

#include "HLas.h"

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

uint64_t fsize(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long long n = std::ftell(f);
    std::fclose(f);
    return n > 0 ? uint64_t(n) : 0;
}

struct Poly { std::vector<double> x, y; };

Poly snake(double sx, double sy, double hd0, double seg, int segs,
           double bend, double w, uint32_t seed) {
    std::vector<double> px, py;
    double x = sx, y = sy, hd = hd0;
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "bruk: %s <fil.laz> [etikett]\n", argv[0]); return 1; }
    const char* src = argv[1];
    const char* tag = argc > 2 ? argv[2] : "?";
    if (!exists(src)) { std::printf("SKIP: saknar %s\n", src); return 0; }

    const std::string cache = HLas::SidecarPathFor(src);
    std::remove(cache.c_str());
    const auto tb = Clock::now();
    if (HLas::BuildSidecar(src) != HLas::Status::Ok) {
        std::printf("FAIL: BuildSidecar\n"); return 1;
    }
    const double build_s = ms_since(tb) / 1000.0;

    HLas::Reader r;
    if (r.open(src, HLas::Cache::Fast) != HLas::Status::Ok) return 1;
    const HLas::Header h = r.header();
    const uint64_t n = h.point_count;

    std::printf("### %s   %s\n", tag, src);
    std::printf("cache        %s\n", cache.c_str());
    std::printf("storlek      %.1f MB   %.2f B/pt\n",
                double(fsize(cache)) / 1048576.0, double(fsize(cache)) / double(n));
    std::printf("bygge        %.1f s\n", build_s);

    // Histogrammet: 1.1.3 skannar, 1.1.4 läser headern.
    {
        uint64_t cnt[256];
        double best = 1e18;
        for (int i = 0; i < 4; ++i) {
            const auto t0 = Clock::now();
            r.get_classification_histogram(cnt);
            const double t = ms_since(t0);
            if (i > 0) best = std::min(best, t);
        }
        std::printf("histogram    %.3f ms\n", best);
    }

    std::vector<double> bx(n), by(n), bz(n);
    std::vector<uint8_t> bc(n);
    bool ground[256] = {}; ground[2] = true;

    struct Spec { const char* what; double w, seg; int segs; double bend; };
    const Spec specs[] = {
        { "orm 12 m x ~300 m",   12, 100, 3, 0.35 },
        { "orm 50 m x ~1 km",    50, 200, 5, 0.30 },
        { "orm 50 m x ~3 km",    50, 400, 8, 0.25 },
        { "orm 200 m x ~1 km",  200, 200, 5, 0.30 },
    };
    const double X0 = h.min_x, Y0 = h.min_y, X1 = h.max_x, Y1 = h.max_y;
    constexpr int kReps = 12;

    std::printf("\n%-22s %12s %12s %12s\n", "fråga", "alla (ms)", "mark (ms)", "träffar");
    std::printf("%s\n", std::string(62, '-').c_str());

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

        auto run = [&](const bool* acc) {
            double best = 1e18; uint64_t hits = 0;
            for (int rep = 0; rep < 3; ++rep) {
                uint64_t hh = 0;
                const auto t0 = Clock::now();
                for (const auto& p : ps)
                    hh += r.get_xyzc_polygon(0, n, p.x.data(), p.y.data(),
                                             uint32_t(p.x.size()),
                                             bx.data(), by.data(), bz.data(),
                                             bc.data(), -1, acc);
                const double t = ms_since(t0);
                if (rep > 0 && t < best) { best = t; hits = hh; }
                for (uint64_t i = 0; i < hh && i < n; i += 9973) sink += bz[i];
            }
            return std::pair<double,uint64_t>{ best / double(kReps), hits / kReps };
        };

        const auto all = run(nullptr);
        const auto grd = run(ground);
        std::printf("%-22s %12.3f %12.3f %12llu\n",
                    s.what, all.first, grd.first, (unsigned long long)all.second);
    }
    std::printf("\n(kontrollsumma %.0f)\n\n", sink);
    std::remove(cache.c_str());
    return 0;
}
