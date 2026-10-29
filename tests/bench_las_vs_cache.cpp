// Lönar sig en sidecar för en .las? — mätning inför 1.1.4.
//
// Frågan planen ställer: en .las packas inte upp, så cachen sparar ingen
// uppackning. Men sedan 1.1.3 bär cachen ett INDEX, och det hjälper oavsett om
// källan är komprimerad. Är den vinsten mätbar, eller äter sidcachen upp den?
//
// ⛔ Cachens innehåll beror INTE på vad källan var. En .hlaz byggd ur autzen.laz
// är identisk med en byggd ur autzen.las — samma punkter, samma sortering. Därför
// går det att mäta vad en .las MED cache skulle ge, utan att bygga funktionen:
// cachevägen mäts genom .laz-filen, och den är samma kod.
//
// Metodfällor från förra labbet, som gäller här också:
//   * uppvärmning krävs, annars mäter första körningen sidfelen
//   * en loop vars resultat kastas stryks HELT av kompilatorn — därför summeras
//     allt till en kontrollsumma som skrivs ut
//   * varianter i samma process konkurrerar om cachen; siffror finare än ~30 %
//     är inte meningsfulla

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

struct Rect { double x0, y0, x1, y1; };

// N slumpmässiga rutor av en given sida, jämnt spridda över filens utbredning.
// Samma frö varje gång, så alla varianter får EXAKT samma frågor.
std::vector<Rect> make_rects(const HLas::Header& h, double side, int n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> ux(h.min_x, std::max(h.min_x, h.max_x - side));
    std::uniform_real_distribution<double> uy(h.min_y, std::max(h.min_y, h.max_y - side));
    std::vector<Rect> out;
    out.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const double x = ux(rng), y = uy(rng);
        out.push_back({ x, y, x + side, y + side });
    }
    return out;
}

struct Result {
    double total_ms = 0;
    uint64_t found = 0;
    double checksum = 0;
};

// En omgång frågor.
//
// ⛔ `count` är BÅDE skanningsintervallet och utdatataket. Ett första utkast här
// satte det till 2 000 000 för att slippa allokera — och mätte då bara filens
// första två miljoner punkter i VARDERA ordningen, alltså två olika punktmängder.
// Alla fyra rader rapporterade olika antal, och tiderna var meningslösa.
// Därför: count = hela filen, och buffertarna rymmer hela filen.
struct Buffers {
    std::vector<double> x, y, z;
    std::vector<uint8_t> c;
    explicit Buffers(uint64_t n) : x(n), y(n), z(n), c(n) {}
};

Result run(HLas::Reader& r, const std::vector<Rect>& rects, Buffers& b, uint64_t cap) {
    auto& bx = b.x; auto& by = b.y; auto& bz = b.z; auto& bc = b.c;
    Result res;
    const auto t0 = Clock::now();
    for (const Rect& q : rects) {
        const double px[4] = { q.x0, q.x1, q.x1, q.x0 };
        const double py[4] = { q.y0, q.y0, q.y1, q.y1 };
        const uint64_t got = r.get_xyzc_polygon(0, cap, px, py, 4,
                                                bx.data(), by.data(), bz.data(),
                                                bc.data());
        res.found += got;
        // Kontrollsumma: utan den stryker kompilatorn hela loopen.
        for (uint64_t i = 0; i < got; ++i) res.checksum += bz[i] + double(bc[i]);
    }
    res.total_ms = ms_since(t0);
    return res;
}

const char* human(double ms) {
    static char buf[32];
    std::snprintf(buf, sizeof buf, "%8.3f", ms);
    return buf;
}

} // namespace

int main() {
    std::printf("Lönar sig en sidecar för en .las?\n");
    std::printf("=================================\n\n");

    const char* las = testdata::kLas;   // autzen.las  — okomprimerad, 362 MB
    const char* laz = testdata::kLaz;   // autzen.laz  — samma data, 56 MB

    for (const char* p : { las, laz }) {
        std::FILE* f = std::fopen(p, "rb");
        if (!f) { std::printf("SKIP: saknar %s\n", p); return 0; }
        std::fclose(f);
    }

    // --- Cachen. Byggs ur .laz men innehållet är källoberoende. ---
    const std::string cache = HLas::SidecarPathFor(laz);
    bool had_cache = false;
    if (std::FILE* f = std::fopen(cache.c_str(), "rb")) { had_cache = true; std::fclose(f); }
    if (!had_cache) {
        std::printf("bygger cache för %s ...\n", laz);
        const auto t0 = Clock::now();
        const HLas::Status st = HLas::BuildSidecar(laz);
        if (st != HLas::Status::Ok) {
            std::printf("FAIL: BuildSidecar: %s\n", HLas::status_message(st));
            return 1;
        }
        std::printf("  klar på %.1f s\n\n", ms_since(t0) / 1000.0);
    }

    // --- Öppningstiden, som är den halva där .las redan är gratis ---
    std::printf("ÖPPNING\n");
    double open_las = 0, open_laz_cache = 0, open_laz_raw = 0;
    {
        HLas::Reader r; const auto t0 = Clock::now();
        if (r.open(las, HLas::Cache::Off) != HLas::Status::Ok) return 1;
        open_las = ms_since(t0);
    }
    {
        HLas::Reader r; const auto t0 = Clock::now();
        if (r.open(laz, HLas::Cache::Fast) != HLas::Status::Ok) return 1;
        open_laz_cache = ms_since(t0);
    }
    {
        HLas::Reader r; const auto t0 = Clock::now();
        if (r.open(laz, HLas::Cache::Off) != HLas::Status::Ok) return 1;
        open_laz_raw = ms_since(t0);
    }
    std::printf("  .las platt (mmap)      %s ms\n", human(open_las));
    std::printf("  .laz + cache           %s ms\n", human(open_laz_cache));
    std::printf("  .laz uppackning        %s ms   <- det cachen sparar för LAZ\n\n",
                human(open_laz_raw));

    // --- Frågorna ---
    HLas::Reader r_las, r_cache;
    if (r_las.open(las, HLas::Cache::Off) != HLas::Status::Ok) return 1;
    if (r_cache.open(laz, HLas::Cache::Fast) != HLas::Status::Ok) return 1;
    if (r_cache.points().valid()) {
        std::printf("FAIL: cachen togs inte — mätningen hade blivit meningslös\n");
        return 1;
    }

    const HLas::Header& h = r_las.header();
    std::printf("autzen: %llu punkter, %.0f x %.0f m, .las %u B/post\n\n",
                (unsigned long long)h.point_count,
                h.max_x - h.min_x, h.max_y - h.min_y,
                unsigned(h.point_record_length));

    // Hela filen: både som skanningsintervall och som buffertstorlek. ~266 MB.
    const uint64_t kCap = h.point_count;
    Buffers buf(kCap);
    struct Case { double side; int n; const char* what; };
    const Case cases[] = {
        {   10.0, 200, "10 x 10 m" },
        {   50.0, 100, "50 x 50 m" },
        {  200.0,  50, "200 x 200 m" },
        { 1000.0,  20, "1000 x 1000 m" },
    };

    std::printf("SLUMPMÄSSIGA OMRÅDESFRÅGOR — medeltid per fråga\n\n");
    std::printf("  %-14s %6s  %10s  %10s  %8s  %s\n",
                "ruta", "frågor", ".las (ms)", "cache (ms)", "kvot", "punkter/fråga");
    std::printf("  %s\n", std::string(72, '-').c_str());

    double sink = 0;
    for (const Case& c : cases) {
        const auto rects = make_rects(h, c.side, c.n, 12345u);

        // Uppvärmning: en omgång av varje som INTE mäts.
        sink += run(r_las,   rects, buf, kCap).checksum;
        sink += run(r_cache, rects, buf, kCap).checksum;

        const Result a = run(r_las,   rects, buf, kCap);
        const Result b = run(r_cache, rects, buf, kCap);
        sink += a.checksum + b.checksum;

        const double per_a = a.total_ms / double(c.n);
        const double per_b = b.total_ms / double(c.n);

        // Mängderna MÅSTE stämma, annars mäter vi två olika saker och tiderna
        // betyder ingenting. Det här är inte en fotnot — det är ett fel.
        if (a.found != b.found) {
            std::printf("\n  ⛔ FEL: %s gav %llu punkter ur .las men %llu ur cachen.\n"
                        "     Mätningen är ogiltig — de två vägarna ser olika mängder.\n",
                        c.what, (unsigned long long)a.found, (unsigned long long)b.found);
            return 1;
        }
        const char* warn = "";

        std::printf("  %-14s %6d  %10.3f  %10.3f  %7.0fx  %10.0f%s\n",
                    c.what, c.n, per_a, per_b,
                    per_b > 0 ? per_a / per_b : 0.0,
                    double(a.found) / double(c.n), warn);
    }

    std::printf("\n  (kontrollsumma %.0f — finns för att hindra kompilatorn från att\n"
                "   stryka loopen, se kommentaren överst)\n", sink);

    std::printf("\nTOLKNING\n");
    std::printf("  .las-kolumnen är vad en .las ger IDAG: full skanning, O(N),\n");
    std::printf("  genom %u-bytesposter.\n", unsigned(h.point_record_length));
    std::printf("  cache-kolumnen är vad en .las MED sidecar skulle ge, eftersom\n");
    std::printf("  cachens innehåll och kod är oberoende av vad källan var.\n");

    if (!had_cache) std::remove(cache.c_str());
    return 0;
}
