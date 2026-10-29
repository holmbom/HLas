// Cachen genom det PUBLIKA API:t — bygg den, öppna med den, och kontrollera att
// den svarar samma sak som källan.
//
// Två saker skiljer det här testet från test_sidecar_lite.cpp, som prövar
// formatet för sig:
//
//   * här går allt genom Reader, alltså den väg ett program använder
//   * och punkterna kommer ut i EN ANNAN ORDNING än i filen, eftersom cachen är
//     spatialt sorterad. Jämförelserna är därför ordningsoberoende: mängder och
//     histogram, aldrig index mot index
//
// Det är också vad cachen LOVAR. Den lovar inte indexmotsvarighet, och de tre
// API:er som lämnar ut råa poster ska säga ifrån i stället för att svara med
// rimliga värden ur fel punkt.

#include "HLas.h"
#include "test_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

namespace {

struct XYZC {
    double x, y, z; uint8_t c;
    bool operator<(const XYZC& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        if (z != o.z) return z < o.z;
        return c < o.c;
    }
    bool operator==(const XYZC& o) const {
        return x == o.x && y == o.y && z == o.z && c == o.c;
    }
};

// En tidsstämpel i namnet, tagen EN gång: två samtidiga körningar (ASan-bygget
// och det vanliga) delade annars fil. Inte processens id — det kräver unistd.h,
// och biblioteket byggs även för Windows.
const std::string& unique_suffix() {
    static const std::string s = "." + std::to_string(
        (long long)std::chrono::steady_clock::now().time_since_epoch().count());
    return s;
}

// Stämpeln läggs FÖRE ändelsen. Efter den hade filen inte längre hetat .laz,
// och SidecarPathFor() svarar tomt på en sökväg som inte gör det.
std::string tmp_path(const char* stem, const char* ext) {
    return testdata::tmp_path(std::string(stem) + unique_suffix() + ext);
}

bool copy_file(const char* from, const std::string& to) {
    std::FILE* in = testdata::fopen_utf8(from, "rb");
    if (!in) return false;
    std::FILE* out = testdata::fopen_utf8(to, "wb");
    if (!out) { std::fclose(in); return false; }
    std::vector<uint8_t> buf(1 << 20);
    bool ok = true;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
        if (std::fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    if (std::fclose(out) != 0) ok = false;
    std::fclose(in);
    return ok;
}

} // namespace

int test_sidecar() {
    int fails = 0;
    printf("test_sidecar:\n");
#ifndef HLAS_HAVE_LAZ
    // Källan är komprimerad, och utan LAZ går den inte att öppna alls. Cachen
    // byggs ur en uppackning, så det finns ingenting att pröva.
    printf("  SKIP (byggd utan LAZ)\n");
    return 0;
#else
    SKIP_IF_MISSING(testdata::kCopc);

    // ⛔ Testet arbetar på en KOPIA, inte på användarens testdata.
    //
    // Cachen måste ligga bredvid sin källa, så ett test som bygger och raderar
    // den bredvid testfilen gör två saker fel: det raderar en cache användaren
    // kan ha byggt själv, och två samtidiga testkörningar rycker filen ur
    // varandras händer mitt i. Det senare gav verkliga FAIL i bygget med
    // BUILD_SHARED_LIBS, av ett skäl som inte hade med delade bibliotek att göra.
    const std::string owned = tmp_path("hlas_sidecar_src", ".copc.laz");
    if (!copy_file(testdata::kCopc, owned)) {
        printf("  SKIP (kunde inte kopiera testdata till %s)\n", owned.c_str());
        testdata::remove_utf8(owned);
        return 0;
    }
    const char* src = owned.c_str();

    const std::string cache_path = HLas::SidecarPathFor(src);
    CHECK(!cache_path.empty());
    if (cache_path.empty()) { testdata::remove_utf8(owned); return fails; }

    // --- 1. Cachen skapas INTE av att läsa ---
    testdata::remove_utf8(cache_path);
    {
        HLas::Reader r;
        CHECK(r.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        std::FILE* f = testdata::fopen_utf8(cache_path, "rb");
        CHECK(f == nullptr);        // ingen cache skriven som bieffekt
        if (f) std::fclose(f);
        // Och läsningen fungerar ändå — den föll tillbaka på full uppackning.
        CHECK(r.header().point_count > 0);
    }

    // --- 2. BuildSidecar skapar den ---
    CHECK(HLas::BuildSidecar(src) == HLas::Status::Ok);
    uint64_t cache_bytes = 0;
    if (std::FILE* f = testdata::fopen_utf8(cache_path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        cache_bytes = uint64_t(std::ftell(f));
        std::fclose(f);
    }
    CHECK(cache_bytes > 0);

    // --- 3. Samma punkter ur cachen som ur källan, som MÄNGD ---
    {
        HLas::Reader a, b;
        CHECK(a.open(src, HLas::Cache::Off)  == HLas::Status::Ok);
        CHECK(b.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        const uint64_t n = a.header().point_count;
        CHECK(b.header().point_count == n);

        // En polygon som täcker hela filen ger både koordinater och klass i ett
        // svep, och är ordningsoberoende på båda sidor.
        const auto& h = a.header();
        const double pad = 1.0;
        const double px[4] = { h.min_x - pad, h.max_x + pad, h.max_x + pad, h.min_x - pad };
        const double py[4] = { h.min_y - pad, h.min_y - pad, h.max_y + pad, h.max_y + pad };

        auto gather = [&](HLas::Reader& r) {
            std::vector<double> x(n), y(n), z(n);
            std::vector<uint8_t> c(n);
            const uint64_t got = r.get_xyzc_polygon(0, n, px, py, 4,
                                                    x.data(), y.data(), z.data(),
                                                    c.data());
            std::vector<XYZC> v(got);
            for (uint64_t i = 0; i < got; ++i) v[i] = { x[i], y[i], z[i], c[i] };
            std::sort(v.begin(), v.end());
            return v;
        };
        const auto va = gather(a);
        const auto vb = gather(b);
        CHECK(va.size() == n);
        CHECK(va.size() == vb.size());
        CHECK(va == vb);
        printf("  %llu punkter, cache %.1f MB (%.2f B/pt), identisk mängd som källan\n",
               (unsigned long long)n, double(cache_bytes) / 1048576.0,
               double(cache_bytes) / double(n));
    }

    // --- 3b. get_classification(i) hör ihop med get_xyz(i) ---
    //
    // Båda är index-tagande och båda fungerar över cachen, men de måste avse
    // SAMMA punkt. Läser de ur olika ordningar hade varje punkt fått en
    // grannes klass — tyst, och med rimliga värden.
    {
        HLas::Reader a, b;
        CHECK(a.open(src, HLas::Cache::Off)  == HLas::Status::Ok);
        CHECK(b.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        const uint64_t n = b.header().point_count;

        // Bygg källans (punkt -> klass)-par och slå upp cachens punkter i det.
        // Ordningen är en annan, så jämförelsen måste gå via koordinaten.
        std::vector<std::pair<std::array<int64_t, 3>, uint8_t>> from_src;
        from_src.reserve(size_t(n));
        for (uint64_t i = 0; i < n; ++i) {
            double x, y, z;
            a.get_xyz(i, x, y, z);
            from_src.push_back({ { int64_t(std::llround(x * 1000.0)),
                                   int64_t(std::llround(y * 1000.0)),
                                   int64_t(std::llround(z * 1000.0)) },
                                 a.get_classification(i) });
        }
        std::sort(from_src.begin(), from_src.end());

        uint64_t checked = 0, wrong = 0;
        for (uint64_t i = 0; i < n; i += 997) {          // stickprov, inte hela
            double x, y, z;
            b.get_xyz(i, x, y, z);
            const std::array<int64_t, 3> key = {
                int64_t(std::llround(x * 1000.0)), int64_t(std::llround(y * 1000.0)),
                int64_t(std::llround(z * 1000.0)) };
            const uint8_t cls = b.get_classification(i);
            // Samma koordinat kan finnas flera gånger; det räcker att NÅGON av
            // dem bär klassen cachen påstår.
            auto lo = std::lower_bound(from_src.begin(), from_src.end(),
                                       std::make_pair(key, uint8_t(0)));
            bool found = false;
            for (auto it = lo; it != from_src.end() && it->first == key; ++it)
                if (it->second == cls) { found = true; break; }
            ++checked;
            if (!found) ++wrong;
        }
        CHECK(checked > 0);
        CHECK(wrong == 0);
        printf("  get_classification ur cachen: %llu stickprov, %llu fel\n",
               (unsigned long long)checked, (unsigned long long)wrong);
    }

    // --- 3c. `count` är ett TAK på antalet skrivna punkter, i båda vägarna ---
    //
    // Den platta vägen får taket gratis: den går bara igenom `count` punkter.
    // Cachen går igenom hela block och måste räkna själv — utan det skrev en
    // polygonfråga så många punkter som ryms i POLYGONEN, inte så många som ryms
    // i anroparens buffert. En överskrivning av högen, styrd av filens innehåll.
    {
        HLas::Reader a, b;
        CHECK(a.open(src, HLas::Cache::Off)  == HLas::Status::Ok);
        CHECK(b.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        const auto& h = a.header();

        // En polygon över HELA filen, så att taket är det enda som stoppar.
        const double px[4] = { h.min_x - 1, h.max_x + 1, h.max_x + 1, h.min_x - 1 };
        const double py[4] = { h.min_y - 1, h.min_y - 1, h.max_y + 1, h.max_y + 1 };

        constexpr uint64_t kCap = 100;
        std::vector<double> xs(kCap + 8), ys(kCap + 8), zs(kCap + 8);
        std::vector<uint8_t> cs(kCap + 8);
        const uint8_t guard = 0xAB;
        std::fill(cs.begin(), cs.end(), guard);

        for (HLas::Reader* r : { &a, &b }) {
            const uint64_t n1 = r->get_xyz_polygon(0, kCap, px, py, 4,
                                                   xs.data(), ys.data(), zs.data());
            CHECK(n1 <= kCap);
            const uint64_t n2 = r->get_xyzc_polygon(0, kCap, px, py, 4,
                                                    xs.data(), ys.data(), zs.data(),
                                                    cs.data());
            CHECK(n2 <= kCap);
        }
        // Ingen skrev förbi taket.
        for (size_t i = kCap; i < cs.size(); ++i) CHECK(cs[i] == guard);
        printf("  count är ett tak även ur cachen\n");
    }

    // --- 3d. `start` gäller ÄVEN ur cachen ---
    //
    // Läser man i omgångar med växande `start` ska omgångarna tillsammans ge
    // samma mängd som ett enda anrop, och ingen punkt två gånger. En sållad väg
    // som struntar i `start` söker hela cachen varje gång och lämnar tillbaka
    // samma första träffar om och om igen — anroparen får då dubbletter och
    // ser aldrig slutet av filen.
    {
        HLas::Reader r;
        CHECK(r.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        const auto& h = r.header();
        const uint64_t n = h.point_count;

        // Halva filen i x-led, så att sållningen verkligen kastar block.
        const double mx = h.min_x + (h.max_x - h.min_x) * 0.5;
        const double px[4] = { h.min_x, mx, mx, h.min_x };
        const double py[4] = { h.min_y, h.min_y, h.max_y, h.max_y };

        std::vector<double> ax(n), ay(n), az(n);
        const uint64_t all = r.get_xyz_polygon(0, n, px, py, 4,
                                               ax.data(), ay.data(), az.data());
        CHECK(all > 0);
        CHECK(all < n);                     // annars sållar inget och provet är tomt

        // Samma fråga i omgångar.
        std::vector<double> bx, by, bz;
        const uint64_t step = n / 7 + 1;
        std::vector<double> tx(step), ty(step), tz(step);
        for (uint64_t s = 0; s < n; s += step) {
            const uint64_t got = r.get_xyz_polygon(s, step, px, py, 4,
                                                   tx.data(), ty.data(), tz.data());
            CHECK(got <= step);
            bx.insert(bx.end(), tx.begin(), tx.begin() + long(got));
            by.insert(by.end(), ty.begin(), ty.begin() + long(got));
            bz.insert(bz.end(), tz.begin(), tz.begin() + long(got));
        }
        CHECK(bx.size() == all);

        // Samma MÄNGD, inte bara samma antal.
        auto as_set = [](const std::vector<double>& x, const std::vector<double>& y,
                         const std::vector<double>& z, size_t cnt) {
            std::vector<std::array<double, 3>> v;
            v.reserve(cnt);
            for (size_t i = 0; i < cnt; ++i) v.push_back({ x[i], y[i], z[i] });
            std::sort(v.begin(), v.end());
            return v;
        };
        const auto sa = as_set(ax, ay, az, size_t(all));
        const auto sb = as_set(bx, by, bz, bx.size());
        CHECK(sa == sb);
        printf("  start gäller ur cachen: %llu punkter i sju omgångar, samma mängd\n",
               (unsigned long long)all);
    }

    // --- 3e. LOD: stride ger SAMMA punkter oavsett hur vyn frågar ---
    //
    // ⛔ Det HÄR är hela poängen med att gallra per CELL i stället för med en
    // räknare. Urvalet styrs av punktens plats i cellen, så det beror inte på
    // frågan — en vy som zoomar eller panorerar får samma punkter, och flimrar
    // därför inte. En global räknare hade gett olika urval för olika frågor.
    {
        HLas::Reader r;
        CHECK(r.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        const auto& h = r.header();
        const uint64_t n = h.point_count;

        uint8_t stride[256];
        std::fill(std::begin(stride), std::end(stride), uint8_t(10));

        // En liten ruta mitt i filen — "inzoomad".
        const double cx = (h.min_x + h.max_x) * 0.5, cy = (h.min_y + h.max_y) * 0.5;
        const double w = (h.max_x - h.min_x) * 0.08;
        const double px[4] = { cx - w, cx + w, cx + w, cx - w };
        const double py[4] = { cy - w, cy - w, cy + w, cy + w };

        // Samma ruta, men frågad som en del av HELA filen — "utzoomad".
        const double qx[4] = { h.min_x, h.max_x, h.max_x, h.min_x };
        const double qy[4] = { h.min_y, h.min_y, h.max_y, h.max_y };

        std::vector<double> ax(n), ay(n), az(n), bx2(n), by2(n), bz2(n);

        const uint64_t na = r.get_xyz_polygon(0, n, px, py, 4,
                                              ax.data(), ay.data(), az.data(),
                                              -1, stride);
        const uint64_t nb = r.get_xyz_polygon(0, n, qx, qy, 4,
                                              bx2.data(), by2.data(), bz2.data(),
                                              -1, stride);
        CHECK(na > 0);
        CHECK(nb > na);                       // den stora rutan ger fler

        // Gallringen ska ge ungefär en tiondel.
        const uint64_t full = r.get_xyz_polygon(0, n, qx, qy, 4,
                                                bx2.data(), by2.data(), bz2.data());
        CHECK(full > 0);
        const double frac = double(nb) / double(full);
        CHECK(frac > 0.07 && frac < 0.14);    // ~1/10, med kantvariation

        // ⛔ OCH DET AVGÖRANDE: varje punkt den lilla frågan gav måste finnas i
        // den stora. Samma cell, samma k, samma urval — oavsett frågan.
        const uint64_t nb2 = r.get_xyz_polygon(0, n, qx, qy, 4,
                                               bx2.data(), by2.data(), bz2.data(),
                                               -1, stride);
        CHECK(nb2 == nb);
        std::vector<std::array<double,3>> big;
        big.reserve(size_t(nb2));
        for (uint64_t i = 0; i < nb2; ++i) big.push_back({ bx2[i], by2[i], bz2[i] });
        std::sort(big.begin(), big.end());

        uint64_t missing = 0;
        for (uint64_t i = 0; i < na; ++i) {
            const std::array<double,3> k = { ax[i], ay[i], az[i] };
            if (!std::binary_search(big.begin(), big.end(), k)) ++missing;
        }
        CHECK(missing == 0);
        printf("  stride 10: %llu punkter inzoomad, alla finns i utzoomad (%.1f %% av full)\n",
               (unsigned long long)na, frac * 100.0);
    }

    // --- 3f. Gallringen i kombination: per klass, med accepted, och över
    // flera start/count-omgångar. Zoom-stabiliteten ovan är värdelös om urvalet
    // ändras när anroparen läser i omgångar eller lägger på ett klassfilter.
    {
        HLas::Reader r;
        CHECK(r.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        const auto& h = r.header();
        const uint64_t n = h.point_count;
        const double qx[4] = { h.min_x, h.max_x, h.max_x, h.min_x };
        const double qy[4] = { h.min_y, h.min_y, h.max_y, h.max_y };

        std::vector<double> ax(n), ay(n), az(n);
        std::vector<uint8_t> ac(n);

        // stride 0 och 1 betyder FULL TÄTHET, inte "ta ingen" och inte division
        // med noll. De når aldrig modulooperatorn.
        const uint64_t full = r.get_xyzc_polygon(0, n, qx, qy, 4, ax.data(),
                                                 ay.data(), az.data(), ac.data());
        CHECK(full > 0);
        for (uint8_t v : { uint8_t(0), uint8_t(1) }) {
            uint8_t st[256];
            std::fill(std::begin(st), std::end(st), v);
            const uint64_t got = r.get_xyzc_polygon(0, n, qx, qy, 4, ax.data(),
                                                    ay.data(), az.data(),
                                                    ac.data(), -1, nullptr, st);
            CHECK(got == full);
        }

        // Klassvis stride: klass 2 gallras, allt annat lämnas orört. Räkna per
        // klass och jämför med den ogallrade fördelningen.
        uint64_t full_cls[256] = {};
        for (uint64_t i = 0; i < full; ++i) ++full_cls[ac[i]];

        uint8_t st[256];
        std::fill(std::begin(st), std::end(st), uint8_t(1));
        st[2] = 4;
        const uint64_t mixed = r.get_xyzc_polygon(0, n, qx, qy, 4, ax.data(),
                                                  ay.data(), az.data(),
                                                  ac.data(), -1, nullptr, st);
        uint64_t mix_cls[256] = {};
        for (uint64_t i = 0; i < mixed; ++i) ++mix_cls[ac[i]];
        for (int c = 0; c < 256; ++c) {
            if (c == 2) continue;
            CHECK(mix_cls[c] == full_cls[c]);        // orörda klasser är orörda
        }
        CHECK(full_cls[2] > 0);
        const double q = double(mix_cls[2]) / double(full_cls[2]);
        CHECK(q > 0.18 && q < 0.32);                 // ~1/4

        // accepted OCH stride samtidigt: samma punkter som klassvis stride, men
        // begränsat till klass 2. Att de två filtren skulle störa varandra är
        // just den sortens fel ett kombinerat prov finns för.
        bool acc[256] = {};
        acc[2] = true;
        std::fill(std::begin(st), std::end(st), uint8_t(1));
        st[2] = 4;
        const uint64_t both = r.get_xyzc_polygon(0, n, qx, qy, 4, ax.data(),
                                                 ay.data(), az.data(),
                                                 ac.data(), -1, acc, st);
        CHECK(both == mix_cls[2]);
        for (uint64_t i = 0; i < both; ++i) CHECK(ac[i] == 2);

        // Och i omgångar: samma mängd som i ett svep. Fasen får inte följa
        // `start`, för då hade en pannande vy fått nya punkter varje omgång.
        std::vector<std::array<double,3>> one, many;
        std::fill(std::begin(st), std::end(st), uint8_t(7));
        const uint64_t sweep = r.get_xyzc_polygon(0, n, qx, qy, 4, ax.data(),
                                                  ay.data(), az.data(),
                                                  ac.data(), -1, nullptr, st);
        for (uint64_t i = 0; i < sweep; ++i) one.push_back({ ax[i], ay[i], az[i] });
        const uint64_t chunk = n / 5 + 1;
        for (uint64_t s0 = 0; s0 < n; s0 += chunk) {
            const uint64_t got = r.get_xyzc_polygon(s0, chunk, qx, qy, 4,
                                                    ax.data(), ay.data(), az.data(),
                                                    ac.data(), -1, nullptr, st);
            for (uint64_t i = 0; i < got; ++i) many.push_back({ ax[i], ay[i], az[i] });
        }
        std::sort(one.begin(), one.end());
        std::sort(many.begin(), many.end());
        CHECK(one == many);
        printf("  stride: 0/1 = full täthet, klassvis %.1f %% av klass 2, "
               "accepted+stride enigt, %zu punkter lika i fem omgångar\n",
               q * 100.0, one.size());
    }

    // --- 3g. Gallringen gäller ÄVEN utan cache. Att stride tyst ignoreras när
    // ingen cache finns vore värre än ett fel: anroparen ber om en tiondel och
    // får allt, och märker det först när minnet tar slut.
    //
    // Urvalet är däremot ett ANNAT än cachens — utan celler finns ingen plats i
    // en cell att räkna, så fasen går på det globala indexet och följer filens
    // ordning. Samma ANDEL, inte samma punkter.
    {
        HLas::Reader r;
        CHECK(r.open(src, HLas::Cache::Off) == HLas::Status::Ok);
        const auto& h = r.header();
        const uint64_t n = h.point_count;
        const double qx[4] = { h.min_x, h.max_x, h.max_x, h.min_x };
        const double qy[4] = { h.min_y, h.min_y, h.max_y, h.max_y };

        std::vector<double> ax(n), ay(n), az(n);
        std::vector<uint8_t> ac(n);
        const uint64_t full = r.get_xyzc_polygon(0, n, qx, qy, 4, ax.data(),
                                                 ay.data(), az.data(), ac.data());
        uint8_t st[256];
        std::fill(std::begin(st), std::end(st), uint8_t(5));
        const uint64_t thin = r.get_xyzc_polygon(0, n, qx, qy, 4, ax.data(),
                                                 ay.data(), az.data(),
                                                 ac.data(), -1, nullptr, st);
        CHECK(full > 0);
        CHECK(thin > 0 && thin < full);
        const double frac = double(thin) / double(full);
        CHECK(frac > 0.15 && frac < 0.25);          // ~1/5
        printf("  stride utan cache: %.1f %% av full täthet (andelen, inte samma punkter)\n",
               frac * 100.0);
    }

    // --- 4. Histogrammet är identiskt. Ordningsoberoende av naturen. ---
    {
        HLas::Reader a, b;
        CHECK(a.open(src, HLas::Cache::Off)  == HLas::Status::Ok);
        CHECK(b.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        uint64_t ca[256] = {}, cb[256] = {};
        CHECK(a.get_classification_histogram(ca) == HLas::Status::Ok);
        CHECK(b.get_classification_histogram(cb) == HLas::Status::Ok);
        CHECK(std::memcmp(ca, cb, sizeof ca) == 0);
    }

    // --- 5. En DEM ur cachen ska vara cell för cell densamma ---
    {
        HLas::Reader a, b;
        CHECK(a.open(src, HLas::Cache::Off)  == HLas::Status::Ok);
        CHECK(b.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
        const auto& h = a.header();
        const double px[4] = { h.min_x, h.max_x, h.max_x, h.min_x };
        const double py[4] = { h.min_y, h.min_y, h.max_y, h.max_y };
        const double cell = (h.max_x - h.min_x) / 64.0;
        const auto da = a.generate_dem(px, py, 4, cell, HLas::DemFill::None, nullptr, 1);
        const auto db = b.generate_dem(px, py, 4, cell, HLas::DemFill::None, nullptr, 1);
        CHECK(da.cols == db.cols && da.rows == db.rows);
        CHECK(da.elevation.size() == db.elevation.size());
        uint64_t diff = 0;
        for (size_t i = 0; i < da.elevation.size() && i < db.elevation.size(); ++i) {
            const float p = da.elevation[i], q = db.elevation[i];
            const bool pn = p != p, qn = q != q;      // NaN
            if (pn != qn || (!pn && p != q)) ++diff;
        }
        CHECK(diff == 0);
        printf("  DEM %ux%u ur cachen: %llu avvikande celler\n",
               da.cols, da.rows, (unsigned long long)diff);
    }

    // --- 6. De råa vägarna SÄGER IFRÅN i stället för att svara fel ---
    {
        HLas::Reader r;
        CHECK(r.open(src, HLas::Cache::Fast) == HLas::Status::Ok);

        CHECK(!r.points().valid());
        CHECK(!r.last_error().empty());

        CHECK(r.point_data(0) == nullptr);
        CHECK(!r.last_error().empty());

        uint8_t buf[64];
        CHECK(r.read_points(0, 1, buf) == 0);
        CHECK(!r.last_error().empty());

        // Index-tagande API:er FUNGERAR däremot — gränsen går vid råa poster,
        // inte vid index. Ett index är en position i den här läsaren.
        double x = 0, y = 0, z = 0;
        r.get_xyz(0, x, y, z);
        CHECK(x != 0.0 || y != 0.0 || z != 0.0);

        // Och samma punkt via batchvägen, som är samma operation.
        double bx = 0, by = 0, bz = 0;
        CHECK(r.get_xyz_batch(0, 1, &bx, &by, &bz) == 1);
        CHECK(bx == x && by == y && bz == z);

        // Postlängden i headern är KÄLLANS, inte noll.
        CHECK(r.header().point_record_length > 13);
    }

    // --- 7. Cache::Off rör den inte, och Smart hoppar över en COPC ---
    {
        HLas::Reader r;
        CHECK(r.open(src, HLas::Cache::Off) == HLas::Status::Ok);
        CHECK(r.points().valid());              // råa poster finns när cachen inte används

        HLas::Reader s;
        CHECK(s.open(src, HLas::Cache::Smart) == HLas::Status::Ok);
        // Filen ÄR COPC, så Smart ska inte ta cachen — och då finns posterna.
        CHECK(s.points().valid());
    }

    // --- 8. En cache för en ANNAN fil ska avvisas, inte användas ---
    {
        // Skriv om cachen med fel storlek i källfältet genom att bygga den för
        // en annan fil och lägga den på den här filens plats.
        SKIP_IF_MISSING(testdata::kLaz);
        // Också en kopia, av samma skäl som ovan.
        const std::string other_src = tmp_path("hlas_other_src", ".laz");
        const std::string other = copy_file(testdata::kLaz, other_src)
                                ? HLas::SidecarPathFor(other_src.c_str())
                                : std::string();
        if (!other.empty() && HLas::BuildSidecar(other_src.c_str()) == HLas::Status::Ok) {
            testdata::remove_utf8(cache_path);
            // Utan den här kontrollen kunde provet passera bara för att ingen
            // cache fanns — alltså av fel skäl.
            CHECK(HLas::detail::rename_utf8(other, cache_path));
            std::FILE* chk = testdata::fopen_utf8(cache_path, "rb");
            CHECK(chk != nullptr);
            if (chk) std::fclose(chk);
            HLas::Reader r;
            CHECK(r.open(src, HLas::Cache::Fast) == HLas::Status::Ok);
            // Fel fil: cachen ska ha förkastats, alltså finns råa poster.
            CHECK(r.points().valid());
            printf("  en cache byggd för en annan fil förkastades\n");
        }
        testdata::remove_utf8(other);
        testdata::remove_utf8(other_src);
    }

    // --- 9. Källan ändras UTAN att storleken gör det ---
    auto size_of = [](const std::string& p) -> long {
        std::FILE* f = testdata::fopen_utf8(p, "rb");
        if (!f) return -1;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fclose(f);
        return n;
    };
    //
    // Punktantal, format, skala, offset och storlek är alla oförändrade, så de
    // fem identitetsfälten säger "samma fil". Bara stickprovet ur källans byte
    // skiljer dem åt. Provet görs på en KOPIA — källfilen rörs inte.
    {
        SKIP_IF_MISSING(src);
        const std::string copy = tmp_path("hlas_fingerprint", ".laz");
        const bool copied = copy_file(src, copy);
        CHECK(copied);
        if (copied) {
            CHECK(HLas::BuildSidecar(copy.c_str()) == HLas::Status::Ok);
            const std::string cc = HLas::SidecarPathFor(copy.c_str());

            // Cachen gäller för kopian som den ser ut nu.
            {
                HLas::Reader r;
                CHECK(r.open(copy.c_str(), HLas::Cache::Fast) == HLas::Status::Ok);
                CHECK(!r.points().valid());        // cachen togs, alltså inga poster
            }

            // Vänd EN bit mitt i punktdatan. Filens längd är densamma.
            const long before = size_of(copy);
            CHECK(before > 0);
            {
                std::FILE* f = testdata::fopen_utf8(copy, "r+b");
                CHECK(f != nullptr);
                if (f) {
                    CHECK(std::fseek(f, before / 2, SEEK_SET) == 0);
                    int b = std::fgetc(f);
                    CHECK(b != EOF);
                    CHECK(std::fseek(f, before / 2, SEEK_SET) == 0);
                    CHECK(std::fputc(b ^ 0x5A, f) != EOF);
                    CHECK(std::fclose(f) == 0);
                }
            }
            CHECK(size_of(copy) == before);

            HLas::Reader r;
            CHECK(r.open(copy.c_str(), HLas::Cache::Fast) == HLas::Status::Ok);
            // Cachen ska ha förkastats trots att alla fem identitetsfälten
            // stämmer — och då finns de råa posterna ur källan.
            CHECK(r.points().valid());
            printf("  en källa som ändrats utan att växa förkastade sin cache\n");

            testdata::remove_utf8(cc);
            testdata::remove_utf8(copy);
        }
    }

    // --- 10. En .las får också en cache, och den beter sig som en .laz:s ---
    //
    // ⛔ Det HÄR provet saknades, och därför överlevde en bugg: ett utkast lät
    // en .las behålla sin mmap:ade källa för points() MEDAN frågorna gick genom
    // cachen. Två ordningar bakom ett index — get_xyz(i) ur cellordningen,
    // points().x(i) ur filens — och vyn märkt som cacheformat fast bufferten
    // bar riktiga LAS-poster. Ingenting felade, svaren blev bara fel.
    {
        SKIP_IF_MISSING(testdata::kLas);
        const std::string lc = tmp_path("hlas_las_src", ".las");
        if (copy_file(testdata::kLas, lc)) {
            const std::string lcc = HLas::SidecarPathFor(lc.c_str());
            CHECK(!lcc.empty());                     // .las ska ge en sökväg
            CHECK(lcc == lc + ".hlz");               // TILLAGD ändelse, inte ersatt
            CHECK(HLas::BuildSidecar(lc.c_str()) == HLas::Status::Ok);

            HLas::Reader a, b;
            CHECK(a.open(lc.c_str(), HLas::Cache::Off)  == HLas::Status::Ok);
            CHECK(b.open(lc.c_str(), HLas::Cache::Fast) == HLas::Status::Ok);

            // Cachen togs: inga råa poster, precis som för en .laz.
            CHECK(!b.points().valid());
            CHECK(!b.last_error().empty());
            CHECK(b.point_data(0) == nullptr);

            // Men index-tagande anrop fungerar, och hör ihop med varandra.
            CHECK(b.header().point_count == a.header().point_count);
            double x = 0, y = 0, z = 0;
            b.get_xyz(0, x, y, z);
            CHECK(x != 0.0 || y != 0.0 || z != 0.0);

            // Histogrammet ur headern måste stämma med källans skanning.
            uint64_t ha[256] = {}, hb[256] = {};
            CHECK(a.get_classification_histogram(ha) == HLas::Status::Ok);
            CHECK(b.get_classification_histogram(hb) == HLas::Status::Ok);
            CHECK(std::memcmp(ha, hb, sizeof ha) == 0);

            printf("  en .las får cache, och beter sig som en .laz:s\n");
            testdata::remove_utf8(lcc);
            testdata::remove_utf8(lc);
        }
    }

    testdata::remove_utf8(cache_path);
    testdata::remove_utf8(owned);
    if (fails == 0) printf("  cachen svarar som källan, och säger ifrån där den inte kan\n");
    return fails;
#endif
}
