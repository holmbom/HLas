// .hlz version 4 — formatet, prövat för sig innan det kopplas in någonstans.
//
// Två saker måste hålla, och de är olika sorters påstående:
//
//   1. VARJE PUNKT KOMMER TILLBAKA EXAKT. Bitpackningen är förlustfri — samma
//      heltal, bara utan de tomma höga bitarna — så en punkt in ska vara samma
//      punkt ut. Att jämföra summor eller stickprov räcker inte; en
//      bitbreddsmiss slår bara på vissa värden.
//   2. BLOCKENS BBOX MÅSTE OMSLUTA SINA PUNKTER. Det är hela indexet: sållar vi
//      bort ett block vars bbox ljuger, tappar vi punkter TYST ur varje fråga.
//
// Punkterna kommer ut i EN ANNAN ORDNING än de gick in — sorteringen är hela
// poängen. Testet jämför därför mängder, inte index.

#include "HLas.h"
#include "sidecar_lite.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <chrono>
#include <string>
#include <vector>

#include "test_data.h"

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

namespace {

// En tidsstämpel i namnet: två samtidiga körningar (ASan-bygget och det vanliga)
// delade annars fil, och den ena raderade den andras testdata mitt i. Inte
// processens id — det kräver unistd.h, och biblioteket byggs även för Windows.
//
// Stämpeln tas EN gång per process. Togs den per anrop hade två anrop för samma
// fil gett olika namn, och det som skrevs gick inte att öppna igen.
const std::string& unique_suffix() {
    static const std::string s = "." + std::to_string(
        (long long)std::chrono::steady_clock::now().time_since_epoch().count());
    return s;
}

std::string tmp_path(const char* name) {
    return testdata::tmp_path(std::string(name) + unique_suffix());
}

using HLas::detail::SidecarPoint;

struct Key {
    int32_t x, y, z; uint8_t c;
    bool operator<(const Key& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        if (z != o.z) return z < o.z;
        return c < o.c;
    }
    bool operator==(const Key& o) const {
        return x == o.x && y == o.y && z == o.z && c == o.c;
    }
};

HLas::detail::SidecarHeader meta_for(uint64_t n) {
    HLas::detail::SidecarHeader m;
    m.point_count = n;
    m.x_scale = m.y_scale = m.z_scale = 0.01;
    m.x_offset = 500000; m.y_offset = 6500000; m.z_offset = 0;
    m.source_size = 123456789;
    m.source_format = 6;
    return m;
}

// Ett fall: bygg, läs tillbaka, jämför mängderna och granska varje block.
int round_trip(const char* label, std::vector<SidecarPoint> in) {
    int fails = 0;
    const uint64_t n = in.size();
    const std::string path = tmp_path("hlas_lite_test.hlz");

    std::vector<Key> want;
    want.reserve(n);
    for (const auto& p : in) want.push_back({ p.x, p.y, p.z, p.c });
    std::sort(want.begin(), want.end());

    auto m = meta_for(n);
    m.point_count = n;
    CHECK(HLas::detail::write_sidecar(path, m, in) == HLas::detail::ErrorCode::Ok);

    HLas::detail::SidecarReader r;
    const auto ec = r.open(path.c_str(), m);
    CHECK(ec == HLas::detail::ErrorCode::Ok);
    if (ec != HLas::detail::ErrorCode::Ok) { testdata::remove_utf8(path); return fails; }

    CHECK(r.count() == n);

    // 1. Varje punkt tillbaka, exakt. Mängd mot mängd — ordningen är en annan.
    std::vector<Key> got;
    got.reserve(n);
    for (uint64_t i = 0; i < r.count(); ++i) {
        int32_t x, y, z; uint8_t c;
        if (!r.point(i, x, y, z, c)) { CHECK(false); break; }
        got.push_back({ x, y, z, c });
    }
    std::sort(got.begin(), got.end());
    CHECK(got.size() == want.size());
    CHECK(got == want);

    // 2. Varje punkt ligger i SIN cell. Ljuger en cellgräns sållas cellen bort
    //    ur frågor den borde träffa, och punkter försvinner tyst.
    uint64_t seen = 0, bad_box = 0, bad_class = 0;
    for (uint64_t ci = 0; ci < r.cell_count(); ++ci) {
        const auto cell = r.cell(ci);
        seen += cell.n;
        int64_t x0, y0, x1, y1;
        r.cell_bounds(cell, x0, y0, x1, y1);
        for (uint32_t k = 0; k < cell.n; ++k) {
            int32_t x, y, z; uint8_t c;
            r.point_in_cell(cell, k, x, y, z, c);
            // ⛔ Genvägen måste läsa SAMMA bitar. class_in_cell() hoppar över
            // koordinaterna för att en bortgallrad punkt inte ska kosta tre
            // uppackningar — och en avvikelse där ger tyst fel klass i varje
            // filtrerad fråga, utan att något annat prov märker det.
            if (r.class_in_cell(cell, k) != c) ++bad_class;
            if (x < x0 || x >= x1 || y < y0 || y >= y1) ++bad_box;
            // Cellens min_z är referensen bitarna packas mot, så en punkt under
            // den hade betytt ett negativt tal i ett osignerat fält.
            if (z < cell.min_z) ++bad_box;
        }
    }
    CHECK(seen == n);
    CHECK(bad_box == 0);
    CHECK(bad_class == 0);

    // 3. Samma punkt via cell-vägen och via index-vägen.
    {
        uint64_t mismatch = 0;
        for (uint64_t ci = 0; ci < r.cell_count(); ++ci) {
            const auto cell = r.cell(ci);
            for (uint32_t k = 0; k < cell.n; k += 7) {
                int32_t x1,y1,z1, x2,y2,z2; uint8_t c1, c2;
                r.point_in_cell(cell, k, x1, y1, z1, c1);
                if (!r.point(cell.first + k, x2, y2, z2, c2)) { ++mismatch; continue; }
                if (x1!=x2 || y1!=y2 || z1!=z2 || c1!=c2) ++mismatch;
            }
        }
        CHECK(mismatch == 0);
    }

    // 4. Histogrammet i headern måste stämma med punkterna. Det är LAGRAD data
    //    som kan ljuga utan att någon märker det — get_classification_histogram
    //    svarar ur den, och anroparen kan inte se vilken väg som svarade.
    {
        uint32_t want_hist[256] = {};
        for (const auto& k : want) ++want_hist[k.c];
        CHECK(std::memcmp(want_hist, r.histogram(), sizeof want_hist) == 0);
    }

    printf("  %-22s %8llu punkter, %llu celler, %.2f B/pt\n", label,
           (unsigned long long)n, (unsigned long long)r.cell_count(), [&]{
               std::FILE* f = testdata::fopen_utf8(path, "rb");
               if (!f) return 0.0;
               std::fseek(f, 0, SEEK_END);
               const double sz = double(std::ftell(f));
               std::fclose(f);
               return sz / double(n);
           }());
    r.close();
    testdata::remove_utf8(path);
    return fails;
}

} // namespace

int test_sidecar_lite() {
    int fails = 0;
    printf("test_sidecar_lite:\n");

    std::mt19937 rng(4711);

    // Spridda punkter över en kvadratkilometer.
    {
        std::uniform_int_distribution<int32_t> ux(0, 100000), uz(0, 20000);
        std::uniform_int_distribution<int>     uc(0, 18);
        std::vector<SidecarPoint> v;
        for (int i = 0; i < 200000; ++i)
            v.push_back({ ux(rng), ux(rng), uz(rng), uint8_t(uc(rng)) });
        fails += round_trip("slumpmässiga", std::move(v));
    }

    // Ett plan: Z konstant. Blocken får då bitbredd 1 för Z, alltså kantfallet
    // där utbredningen är noll.
    {
        std::vector<SidecarPoint> v;
        for (int i = 0; i < 50000; ++i)
            v.push_back({ i * 3, i * 7 % 50000, 1000, 2 });
        fails += round_trip("platt, konstant Z", std::move(v));
    }

    // Alla punkter identiska. Varje blocks utbredning är noll i alla tre led.
    {
        std::vector<SidecarPoint> v(10000, SidecarPoint{ 42, -17, 8, 9 });
        fails += round_trip("alla identiska", std::move(v));
    }

    // int32:ans ändar. Differensen max-min spiller över en signerad int32, och
    // det är precis det bits_for måste klara — den räknar i uint64 av det skälet.
    {
        std::vector<SidecarPoint> v;
        v.push_back({ INT32_MIN, INT32_MIN, INT32_MIN, 0 });
        v.push_back({ INT32_MAX, INT32_MAX, INT32_MAX, 255 });
        v.push_back({ 0, 0, 0, 128 });
        v.push_back({ -1, 1, -1, 31 });
        for (int i = 0; i < 4000; ++i)
            v.push_back({ (i % 2) ? INT32_MAX - i : INT32_MIN + i, i, -i, uint8_t(i & 0xFF) });
        fails += round_trip("int32:ans ändar", std::move(v));
    }

    // Färre punkter än ett block.
    {
        std::vector<SidecarPoint> v;
        for (int i = 0; i < 5; ++i) v.push_back({ i, i * 2, i * 3, uint8_t(i) });
        fails += round_trip("färre än ett block", std::move(v));
    }

    // Exakt en blockgräns plus en punkt — den partiella sista blocken.
    {
        std::vector<SidecarPoint> v;
        for (int i = 0; i < 257; ++i) v.push_back({ i * 11, i * 13, i * 17, uint8_t(i % 32) });
        fails += round_trip("partiellt sista block", std::move(v));
    }

    // Alla 256 klassvärden ska överleva. Klassen lagras i åtta bitar utan
    // maskning här — maskningen sker när punkterna PLOCKAS UR källan.
    {
        std::vector<SidecarPoint> v;
        for (int i = 0; i < 256 * 20; ++i)
            v.push_back({ i, i * 3, i * 5, uint8_t(i & 0xFF) });
        fails += round_trip("alla 256 klasser", std::move(v));
    }

    // En cache som inte hör till filen ska AVVISAS, inte användas. Det är den
    // kända buggen i den gamla sidecaren, och QGIS har den i samma form.
    {
        const std::string path = tmp_path("hlas_lite_stale.hlz");
        std::vector<SidecarPoint> v;
        for (int i = 0; i < 1000; ++i) v.push_back({ i, i, i, 2 });
        auto m = meta_for(v.size());
        CHECK(HLas::detail::write_sidecar(path, m, v) == HLas::detail::ErrorCode::Ok);

        HLas::detail::SidecarReader r;
        CHECK(r.open(path.c_str(), m) == HLas::detail::ErrorCode::Ok);
        r.close();

        struct Mut { const char* what; void (*apply)(HLas::detail::SidecarHeader&); };
        const Mut muts[] = {
            { "annan filstorlek",  [](HLas::detail::SidecarHeader& x){ x.source_size += 1; } },
            { "annat punktformat", [](HLas::detail::SidecarHeader& x){ x.source_format = 7; } },
            { "annan skala",       [](HLas::detail::SidecarHeader& x){ x.x_scale = 0.001; } },
            { "annan offset",      [](HLas::detail::SidecarHeader& x){ x.y_offset += 1000; } },
            { "annat punktantal",  [](HLas::detail::SidecarHeader& x){ x.point_count = 999; } },
        };
        for (const Mut& mu : muts) {
            auto bad = m; mu.apply(bad);
            HLas::detail::SidecarReader r2;
            if (r2.open(path.c_str(), bad) == HLas::detail::ErrorCode::Ok) {
                printf("  FAIL: en cache med %s accepterades\n", mu.what);
                ++fails;
            }
        }
        testdata::remove_utf8(path);
    }

    // En TRASIG cache ska avvisas, inte tolkas. Filen kommer utifrån och styr
    // både allokeringar och hur många bitar som läses, så varje muterat fält är
    // en väg in i minnet.
    {
        const std::string path = tmp_path("hlas_lite_broken.hlz");
        std::vector<SidecarPoint> v;
        for (int i = 0; i < 5000; ++i) v.push_back({ i * 3, i * 5, i % 700, uint8_t(i & 31) });
        auto m = meta_for(v.size());
        CHECK(HLas::detail::write_sidecar(path, m, v) == HLas::detail::ErrorCode::Ok);

        std::vector<uint8_t> good;
        {
            std::FILE* f = testdata::fopen_utf8(path, "rb");
            CHECK(f != nullptr);
            if (f) {
                std::fseek(f, 0, SEEK_END);
                good.resize(size_t(std::ftell(f)));
                std::fseek(f, 0, SEEK_SET);
                CHECK(std::fread(good.data(), 1, good.size(), f) == good.size());
                std::fclose(f);
            }
        }
        CHECK(good.size() > 200);

        auto try_bytes = [&](const char* what, const std::vector<uint8_t>& bytes) {
            const std::string bad = tmp_path("hlas_lite_broken2.hlz");
            std::FILE* f = testdata::fopen_utf8(bad, "wb");
            if (!f) { printf("  FAIL: kunde inte skriva %s\n", what); ++fails; return; }
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            HLas::detail::SidecarReader r;
            if (r.open(bad.c_str(), m) == HLas::detail::ErrorCode::Ok) {
                printf("  FAIL: %s accepterades\n", what);
                ++fails;
            }
            testdata::remove_utf8(bad);
        };

        // Trunkerad på flera ställen: mitt i blocktabellen, mitt i punktdatan,
        // och precis före de åtta slack-byten som bitread läser förbi.
        for (double frac : { 0.3, 0.7, 0.95 }) {
            std::vector<uint8_t> t(good.begin(),
                                   good.begin() + long(double(good.size()) * frac));
            try_bytes("en trunkerad cache", t);
        }
        { // slacken bortklippt — sista punktens läsning skulle gå utanför
            std::vector<uint8_t> t(good.begin(), good.end() - 8);
            try_bytes("en cache utan slack på slutet", t);
        }
        // Bitbredderna ligger sist i celltabellen. Layouten är
        //   header(1152) | row_start(ny+1)*4 | cx*4 | off*(cc+1)*8
        //                | min_z*4 | dx*4 | dy*4 | bx | by | bz | data
        uint64_t cc = 0; std::memcpy(&cc, good.data() + 16, 8);
        uint32_t ny = 0; std::memcpy(&ny, good.data() + 28, 4);
        const size_t bx_at = 1152 + (size_t(ny) + 1) * 4 + size_t(cc) * 4
                           + (size_t(cc) + 1) * 8 + size_t(cc) * 4
                           + size_t(cc) * 4 + size_t(cc) * 4;
        CHECK(cc > 0);
        CHECK(bx_at + 3 * cc <= good.size());

        { // bitbredd 0: ett fält utan bitar, och en stride som inte stämmer
            std::vector<uint8_t> t = good;
            t[bx_at] = 0;
            try_bytes("bitbredd noll", t);
        }
        { // bitbredd 255: bortom vad bitread klarar, och summan kan wrappa
            std::vector<uint8_t> t = good;
            t[bx_at] = 255;
            try_bytes("bitbredd 255", t);
        }
        { // en cell utanför rutnätet
            std::vector<uint8_t> t = good;
            const uint32_t huge = 0xFFFFFFFFu;
            std::memcpy(t.data() + 1152 + (size_t(ny) + 1) * 4, &huge, 4);
            try_bytes("cell utanför rutnätet", t);
        }
        { // bakåtgående bitposition
            std::vector<uint8_t> t = good;
            const size_t off_at = 1152 + (size_t(ny) + 1) * 4 + size_t(cc) * 4;
            const uint64_t bad = ~0ull;
            std::memcpy(t.data() + off_at + 8, &bad, 8);
            try_bytes("bakåtgående bitposition", t);
        }
        { // ett orimligt punktantal, som styr både blockantal och prefixsumma
            std::vector<uint8_t> t = good;
            const uint64_t huge = 0xFFFFFFFFFFFFFF00ull;
            std::memcpy(t.data() + 8, &huge, 8);
            try_bytes("ett orimligt punktantal", t);
        }
        { // blockantal som inte följer av punktantalet
            std::vector<uint8_t> t = good;
            uint64_t nb = 0; std::memcpy(&nb, t.data() + 16, 8);
            nb += 1000;
            std::memcpy(t.data() + 16, &nb, 8);
            try_bytes("fel antal block", t);
        }
        { // blockstorlek noll — division längre fram
            std::vector<uint8_t> t = good;
            const uint32_t z = 0;
            std::memcpy(t.data() + 24, &z, 4);
            try_bytes("blockstorlek noll", t);
        }
        testdata::remove_utf8(path);
    }

    if (fails == 0)
        printf("  punkterna exakta, bbox:arna sanna, och trasiga cachar avvisade\n");
    return fails;
}
