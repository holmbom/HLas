// Vad är histogram-i-headern och LOD värda? — mätning inför 1.1.4.
//
// Ingen av funktionerna finns än. Det som GÅR att mäta nu är vad de skulle
// ersätta, och det räcker för att avgöra om de är värda att bygga:
//
//   1. HISTOGRAM. `get_classification_histogram()` är en full skanning idag.
//      Ligger räkningen i headern blir den ett minnesuppslag. Mätningen är
//      alltså "vad kostar den skanning vi vill bli av med".
//
//   2. LOD. En gallrad läsning slipper packa upp de punkter den hoppar över —
//      men huvudskanningen över BLOCKHUVUDENA är kvar oavsett stride. Vinsten
//      är därför sublinjär och planar ut. Golvet och lutningen går att mäta
//      genom att variera områdets storlek: en pytteliten ruta är nästan bara
//      blockskanning, en stor är nästan bara punktarbete.
//
//      ⚠️ Det ger en HÄRLEDNING av vad stride skulle ge, inte en mätning av
//      den. Skillnaden står utskriven i utdatan.
//
// Metodfällor från tidigare labb gäller: uppvärmning, kontrollsumma så att
// kompilatorn inte stryker loopen, och varianter i samma process konkurrerar
// om cachen.

#include "HLas.h"
#include "test_data.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
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

// Medianen av n körningar. Medelvärdet drar iväg på en enda störd körning.
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

struct HistResult { double ms; uint64_t total; uint64_t classes; };

HistResult time_histogram(HLas::Reader& r, int reps) {
    uint64_t counts[256];
    std::vector<double> times;
    for (int i = 0; i < reps + 1; ++i) {          // första är uppvärmning
        const auto t0 = Clock::now();
        const HLas::Status st = r.get_classification_histogram(counts);
        const double ms = ms_since(t0);
        if (st != HLas::Status::Ok) return { -1, 0, 0 };
        if (i > 0) times.push_back(ms);
    }
    HistResult res{ median(times), 0, 0 };
    for (int c = 0; c < 256; ++c) {
        res.total += counts[c];
        if (counts[c]) ++res.classes;
    }
    return res;
}

} // namespace

int main() {
    std::printf("Histogram i headern, och taket för LOD\n");
    std::printf("======================================\n\n");

    // ---------------------------------------------------------------- 1. HISTOGRAM
    struct File { const char* path; const char* what; };
    const File files[] = {
        { testdata::kLas,                     "autzen.las  (platt, 362 MB)" },
        { testdata::kLaz,                     "autzen.laz  (10,6 M punkter)" },
    };

    std::printf("1. HISTOGRAM — vad kostar den skanning vi vill bli av med?\n\n");
    std::printf("   %-32s %12s %12s\n", "fil", "utan cache", "med cache");
    std::printf("   %s\n", std::string(58, '-').c_str());

    std::vector<std::string> built;
    for (const File& f : files) {
        if (!exists(f.path)) { std::printf("   %-32s  SKIP (saknas)\n", f.what); continue; }

        double no_cache = -1, with_cache = -1;
        uint64_t total_a = 0, total_b = 0;
        int classes = 0;

        {
            HLas::Reader r;
            if (r.open(f.path, HLas::Cache::Off) == HLas::Status::Ok) {
                const HistResult h = time_histogram(r, 5);
                no_cache = h.ms; total_a = h.total; classes = int(h.classes);
            }
        }

        // Cachen finns bara för .laz idag — det är hela poängen med 1.1.4.
        const std::string cache = HLas::SidecarPathFor(f.path);
        if (!cache.empty()) {
            if (!exists(cache.c_str())) {
                const auto t0 = Clock::now();
                if (HLas::BuildSidecar(f.path) == HLas::Status::Ok) {
                    built.push_back(cache);
                    std::printf("   (byggde cache för %s på %.1f s)\n",
                                f.what, ms_since(t0) / 1000.0);
                }
            }
            HLas::Reader r;
            if (r.open(f.path, HLas::Cache::Fast) == HLas::Status::Ok) {
                const HistResult h = time_histogram(r, 5);
                with_cache = h.ms; total_b = h.total;
            }
        }

        std::printf("   %-32s %10.2f ms", f.what, no_cache);
        if (with_cache >= 0) std::printf(" %10.2f ms", with_cache);
        else                 std::printf("        (ingen)");
        std::printf("   %d klasser\n", classes);

        if (with_cache >= 0 && total_a != total_b)
            std::printf("      ⛔ FEL: %llu punkter utan cache, %llu med\n",
                        (unsigned long long)total_a, (unsigned long long)total_b);
    }

    std::printf("\n   Med räkningen i headern blir alla dessa ett minnesuppslag,\n");
    std::printf("   alltså i praktiken noll. Kolumnerna är vad vi sparar.\n");

    // ---------------------------------------------------------------- 2. LOD-TAKET
    std::printf("\n\n2. TAKET FÖR LOD — hur mycket av en fråga är blockskanning?\n\n");

    if (!exists(testdata::kLaz)) { std::printf("   SKIP\n"); return 0; }

    const std::string cache = HLas::SidecarPathFor(testdata::kLaz);
    if (!exists(cache.c_str())) {
        if (HLas::BuildSidecar(testdata::kLaz) != HLas::Status::Ok) return 1;
        built.push_back(cache);
    }
    HLas::Reader r;
    if (r.open(testdata::kLaz, HLas::Cache::Fast) != HLas::Status::Ok) return 1;
    const HLas::Header& h = r.header();

    const uint64_t cap = h.point_count;
    std::vector<double> bx(cap), by(cap), bz(cap);
    std::vector<uint8_t> bc(cap);

    // Rutor från pytteliten till nästan hela filen. Den minsta mäter golvet
    // (nästan bara blockskanning), den största lutningen (nästan bara punkter).
    const double sides[] = { 5, 20, 50, 100, 200, 400, 800, 1600, 3000 };
    const int kQueries = 12;

    std::printf("   %-12s %12s %12s %14s\n", "ruta", "punkter", "tid (ms)", "ns/punkt");
    std::printf("   %s\n", std::string(54, '-').c_str());

    double sink = 0;
    std::vector<std::pair<double, double>> fit;   // (punkter, ms)
    for (double side : sides) {
        std::mt19937 rng(999);
        std::uniform_real_distribution<double> ux(h.min_x, std::max(h.min_x, h.max_x - side));
        std::uniform_real_distribution<double> uy(h.min_y, std::max(h.min_y, h.max_y - side));

        double best = 1e18;
        double pts = 0;
        for (int rep = 0; rep < 3; ++rep) {       // första varvet värmer
            rng.seed(999);
            double total_ms = 0, total_pts = 0;
            for (int q = 0; q < kQueries; ++q) {
                const double x = ux(rng), y = uy(rng);
                const double px[4] = { x, x + side, x + side, x };
                const double py[4] = { y, y, y + side, y + side };
                const auto t0 = Clock::now();
                const uint64_t got = r.get_xyzc_polygon(0, cap, px, py, 4,
                                                        bx.data(), by.data(), bz.data(),
                                                        bc.data());
                total_ms += ms_since(t0);
                total_pts += double(got);
                for (uint64_t i = 0; i < got; i += 997) sink += bz[i];
            }
            if (rep > 0) {
                best = std::min(best, total_ms / kQueries);
                pts = total_pts / kQueries;
            }
        }
        std::printf("   %6.0f x %-4.0f %12.0f %12.3f %14.1f\n",
                    side, side, pts, best, pts > 0 ? best * 1e6 / pts : 0.0);
        fit.push_back({ pts, best });
    }

    // Rät linje genom minsta och största fallet: ms = golv + punkter * lutning.
    const double p0 = fit.front().first,  t0v = fit.front().second;
    const double p1 = fit.back().first,   t1v = fit.back().second;
    const double slope = (p1 > p0) ? (t1v - t0v) / (p1 - p0) : 0.0;
    const double floor_ms = t0v - slope * p0;

    std::printf("\n   Anpassad linje:  tid = %.3f ms + punkter x %.1f ns\n",
                floor_ms, slope * 1e6);
    std::printf("   Golvet %.3f ms är blockskanningen — den finns kvar oavsett stride.\n",
                floor_ms);

    std::printf("\n   HÄRLEDD effekt av stride (inte uppmätt — funktionen finns inte):\n\n");
    std::printf("   %-14s", "ruta");
    for (int s : { 1, 2, 4, 8, 16 }) std::printf(" %9d", s);
    std::printf("\n   %s\n", std::string(64, '-').c_str());
    for (size_t i = 0; i < fit.size(); i += 2) {
        std::printf("   %-14.0f", sides[i]);
        for (int s : { 1, 2, 4, 8, 16 })
            std::printf(" %9.3f", floor_ms + (fit[i].first / s) * slope);
        std::printf("\n");
    }
    std::printf("\n   Vinsten planar ut mot golvet. För små rutor finns ingen alls\n");
    std::printf("   att hämta — de ÄR redan blockskanning.\n");

    std::printf("\n   (kontrollsumma %.0f)\n", sink);

    for (const std::string& p : built) std::remove(p.c_str());
    return 0;
}
