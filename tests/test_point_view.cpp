// PointView — den vy läsningen lämnar ut data i.
//
// Två sorters test, och båda behövs:
//
//   1. Syntetiska poster med kända byten, för att bevisa att fältoffsetarna
//      stämmer för VARJE punktformat — även de vi inte har testdata för.
//      Format 4, 5, 9 och 10 finns det ingen fil till, men en post går att
//      bygga för hand, och då finns det inget skäl att lämna dem oprövade.
//   2. Riktiga filer, för att bevisa att fälten faktiskt tar sig hela vägen ut
//      genom läsningen. Det var precis det som INTE gjorde det före 1.1.2.

#include "HLas.h"
#include "test_data.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

namespace {

// Bygger en post där varje fält får ett eget igenkännbart värde, så att ett
// felläst offset ger fel VÄRDE och inte bara fel byte — en förväxling mellan
// två fält som råkar innehålla samma sak hade annars gått igenom.
void put32(std::vector<uint8_t>& r, size_t off, int32_t v) {
    std::memcpy(r.data() + off, &v, 4);
}
void put16(std::vector<uint8_t>& r, size_t off, uint16_t v) {
    std::memcpy(r.data() + off, &v, 2);
}
void put64(std::vector<uint8_t>& r, size_t off, double v) {
    std::memcpy(r.data() + off, &v, 8);
}

int check_legacy_format(uint8_t fmt, bool want_gps, bool want_rgb) {
    int fails = 0;
    const uint16_t len = HLas::PointView::base_length(fmt);
    std::vector<uint8_t> rec(len, 0);

    put32(rec, 0, 1000);
    put32(rec, 4, -2000);
    put32(rec, 8, 300);
    put16(rec, 12, 4321);          // intensity
    // returnummer 5 av 6, scanriktning satt, edge inte
    rec[14] = static_cast<uint8_t>(5 | (6 << 3) | (1 << 6));
    rec[15] = static_cast<uint8_t>(2 | (1 << 5));   // klass 2 + synthetic
    rec[16] = static_cast<uint8_t>(static_cast<int8_t>(-13));  // scanvinkel
    rec[17] = 77;                  // user data
    put16(rec, 18, 909);           // point source
    if (want_gps) put64(rec, 20, 123456.789);
    if (want_rgb) {
        const size_t off = (fmt == 2) ? 20 : 28;
        put16(rec, off,     111);
        put16(rec, off + 2, 222);
        put16(rec, off + 4, 333);
    }

    const HLas::PointView v(rec.data(), 1, len, fmt, 0.01, 0.01, 0.01, 0, 0, 0);
    CHECK(v.point_format() == fmt);
    CHECK(v.record_length() == len);
    CHECK(v.extra_bytes() == 0);
    CHECK(v.raw_x(0) == 1000 && v.raw_y(0) == -2000 && v.raw_z(0) == 300);
    CHECK(std::fabs(v.x(0) - 10.0) < 1e-9);
    CHECK(v.intensity(0) == 4321);
    CHECK(v.return_number(0) == 5);
    CHECK(v.number_of_returns(0) == 6);
    CHECK(v.scan_direction(0));
    CHECK(!v.edge_of_flight_line(0));
    // Klassen är bit 0–4; bit 5 är synthetic och ska INTE ingå i talet.
    // Rå byte hade gett 34.
    CHECK(v.classification(0) == 2);
    CHECK(v.synthetic(0));
    CHECK(!v.key_point(0));
    CHECK(std::fabs(v.scan_angle(0) - (-13.0)) < 1e-9);
    CHECK(v.user_data(0) == 77);
    CHECK(v.point_source_id(0) == 909);

    // Format 0–5 har ingen scannerkanal och ingen overlap-flagga.
    CHECK(v.scanner_channel(0) == 0);
    CHECK(!v.overlap(0));

    CHECK(v.has_gps_time() == want_gps);
    if (want_gps) CHECK(std::fabs(v.gps_time(0) - 123456.789) < 1e-6);
    else          CHECK(v.gps_time(0) == 0.0);

    CHECK(v.has_rgb() == want_rgb);
    CHECK(!v.has_nir());
    uint16_t r = 0, g = 0, b = 0;
    CHECK(v.rgb(0, r, g, b) == want_rgb);
    if (want_rgb) CHECK(r == 111 && g == 222 && b == 333);
    return fails;
}

int check_new_format(uint8_t fmt, bool want_rgb, bool want_nir) {
    int fails = 0;
    const uint16_t len = HLas::PointView::base_length(fmt);
    std::vector<uint8_t> rec(len, 0);

    put32(rec, 0, -5);
    put32(rec, 4, 7);
    put32(rec, 8, 9);
    put16(rec, 12, 65535);                          // intensity, maxvärde
    rec[14] = static_cast<uint8_t>(12 | (15 << 4)); // retur 12 av 15 — kräver 4 bitar
    // klassflaggor 0b1011 (synthetic, key_point, overlap), scannerkanal 2, edge satt
    rec[15] = static_cast<uint8_t>(0x0B | (2 << 4) | (1 << 7));
    rec[16] = 200;                                  // klass > 31, omöjlig i format 0–5
    rec[17] = 88;                                   // user data
    put16(rec, 18, static_cast<uint16_t>(static_cast<int16_t>(-5000))); // scanvinkel
    put16(rec, 20, 4711);                           // point source
    put64(rec, 22, -98765.4321);                    // gps time
    if (want_rgb) { put16(rec, 30, 10); put16(rec, 32, 20); put16(rec, 34, 30); }
    if (want_nir) put16(rec, 36, 40);

    const HLas::PointView v(rec.data(), 1, len, fmt, 1.0, 1.0, 1.0, 0, 0, 0);
    CHECK(v.raw_x(0) == -5 && v.raw_y(0) == 7 && v.raw_z(0) == 9);
    CHECK(v.intensity(0) == 65535);
    CHECK(v.return_number(0) == 12);        // 3 bitar hade gett 4
    CHECK(v.number_of_returns(0) == 15);
    CHECK(v.scanner_channel(0) == 2);
    CHECK(!v.scan_direction(0));
    CHECK(v.edge_of_flight_line(0));
    CHECK(v.synthetic(0) && v.key_point(0) && !v.withheld(0) && v.overlap(0));
    CHECK(v.classification(0) == 200);      // egen byte, inga flaggbitar i vägen
    CHECK(v.user_data(0) == 88);
    // int16 om 0,006 grader: -5000 * 0,006 = -30 grader
    CHECK(std::fabs(v.scan_angle(0) - (-30.0)) < 1e-9);
    CHECK(v.point_source_id(0) == 4711);
    CHECK(v.has_gps_time());
    CHECK(std::fabs(v.gps_time(0) - (-98765.4321)) < 1e-9);

    CHECK(v.has_rgb() == want_rgb);
    CHECK(v.has_nir() == want_nir);
    uint16_t r = 0, g = 0, b = 0;
    CHECK(v.rgb(0, r, g, b) == want_rgb);
    if (want_rgb) CHECK(r == 10 && g == 20 && b == 30);
    CHECK(v.nir(0) == (want_nir ? 40 : 0));
    return fails;
}

} // namespace

int test_point_view() {
    int fails = 0;
    printf("test_point_view:\n");

    // --- Tom och ogiltig vy ---
    {
        HLas::PointView v;
        CHECK(!v.valid());
        CHECK(v.empty() && v.count() == 0);
        CHECK(v.record(0) == nullptr);
        // Varje accessor måste tåla en tom vy utan att dereferera null.
        CHECK(v.x(0) == 0.0 && v.classification(0) == 0 && v.intensity(0) == 0);
        CHECK(v.gps_time(0) == 0.0 && v.nir(0) == 0);
        uint16_t r, g, b;
        CHECK(!v.rgb(0, r, g, b));
        CHECK(v.xyz(0, 10, nullptr, nullptr, nullptr) == 0);
    }

    // Index utanför vyn ska ge noll, inte läsa förbi bufferten.
    {
        std::vector<uint8_t> rec(30, 0xAB);
        const HLas::PointView v(rec.data(), 1, 30, 6, 1, 1, 1, 0, 0, 0);
        CHECK(v.record(0) != nullptr);
        CHECK(v.record(1) == nullptr);
        CHECK(v.classification(1) == 0);
        CHECK(v.intensity(1) == 0);
        CHECK(v.x(1) == 0.0);
    }

    // --- Alla elva punktformat, syntetiskt ---
    fails += check_legacy_format(0, false, false);
    fails += check_legacy_format(1, true,  false);
    fails += check_legacy_format(2, false, true);
    fails += check_legacy_format(3, true,  true);
    fails += check_legacy_format(4, true,  false);   // + wave packet, ej exponerad
    fails += check_legacy_format(5, true,  true);    // + wave packet
    fails += check_new_format(6,  false, false);
    fails += check_new_format(7,  true,  false);
    fails += check_new_format(8,  true,  true);
    fails += check_new_format(9,  false, false);     // + wave packet
    fails += check_new_format(10, true,  true);      // + wave packet
    printf("  format 0-10: fältoffsetar verifierade mot handbyggda poster\n");

    // --- Baslängder ---
    {
        const uint16_t want[11] = {20,28,26,34,57,63,30,36,38,59,67};
        for (uint8_t f = 0; f <= 10; ++f)
            CHECK(HLas::PointView::base_length(f) == want[f]);
        CHECK(HLas::PointView::base_length(HLas::PointView::kSidecarFormat) == 13);
        CHECK(HLas::PointView::base_length(11) == 0);   // okänt format
    }

    // Ett okänt format får inte gissa fram fält ur bytena.
    {
        std::vector<uint8_t> rec(64, 0xFF);
        const HLas::PointView v(rec.data(), 1, 64, 42, 1, 1, 1, 0, 0, 0);
        CHECK(!v.has_intensity() && !v.has_gps_time() && !v.has_rgb());
        CHECK(v.classification(0) == 0);
        CHECK(v.intensity(0) == 0);
        CHECK(v.scan_angle(0) == 0.0);
        // Koordinaterna ligger alltid först och läses ändå.
        CHECK(v.raw_x(0) == -1);
    }

    // --- För kort post: fälten ska bli OSYNLIGA, inte farliga ---
    //
    // En fil kan deklarera format 7 och samtidigt ange en post som inte rymmer
    // det. Vyn får då inte läsa förbi posten — den ska svara "finns inte".
    {
        std::vector<uint8_t> rec(20, 0xEE);
        const HLas::PointView v(rec.data(), 1, 20, 7, 1, 1, 1, 0, 0, 0);
        CHECK(v.valid());                       // koordinaterna ryms
        CHECK(v.has_intensity());               // offset 12, två byte — ryms
        CHECK(v.has_returns());
        CHECK(v.has_scan_angle());              // offset 18 + 2 = 20, ryms precis
        CHECK(!v.has_point_source());           // offset 20 + 2 = 22, ryms inte
        CHECK(!v.has_gps_time());
        CHECK(!v.has_rgb());
        CHECK(v.gps_time(0) == 0.0);
        CHECK(v.point_source_id(0) == 0);
        uint16_t r, g, b;
        CHECK(!v.rgb(0, r, g, b));
    }

    // En post som inte ens rymmer koordinaterna är inte giltig alls, och
    // koordinataccessorerna får INTE läsa de fyra byten på offset 8 ändå —
    // de går inte via offsettabellen och skyddas därför bara av valid().
    {
        std::vector<uint8_t> rec(8, 0);
        const HLas::PointView v(rec.data(), 1, 8, 6, 1, 1, 1, 0, 0, 0);
        CHECK(!v.valid());
        CHECK(v.record(0) == nullptr);
        CHECK(v.raw_x(0) == 0 && v.raw_y(0) == 0 && v.raw_z(0) == 0);
        CHECK(v.x(0) == 0.0 && v.y(0) == 0.0 && v.z(0) == 0.0);
        double bx[4], by[4], bz[4];
        CHECK(v.xyz(0, 1, bx, by, bz) == 0);
    }

    // rgb() ska lämna utparametrarna ORÖRDA när formatet saknar färg — inte
    // nolla dem, för då går det inte att skilja "saknas" från "svart".
    {
        std::vector<uint8_t> rec(30, 0);
        const HLas::PointView v(rec.data(), 1, 30, 6, 1, 1, 1, 0, 0, 0);
        uint16_t r = 4242, g = 4243, b = 4244;
        CHECK(!v.rgb(0, r, g, b));
        CHECK(r == 4242 && g == 4243 && b == 4244);
    }

    // Nullbas med påstått antal får inte ge pekararitmetik på null.
    {
        const HLas::PointView v(nullptr, 100, 30, 6, 1, 1, 1, 0, 0, 0);
        CHECK(!v.valid());
        CHECK(v.record(0) == nullptr);
        CHECK(v.classification(0) == 0);
        CHECK(v.x(0) == 0.0);
    }

    // --- Extra bytes ---
    {
        std::vector<uint8_t> rec(36 + 5, 0);
        rec[16] = 9;
        const HLas::PointView v(rec.data(), 1, 41, 7, 1, 1, 1, 0, 0, 0);
        CHECK(v.extra_bytes() == 5);
        CHECK(v.record_length() == 41);
        CHECK(v.classification(0) == 9);   // fälten ligger kvar, posten är bara längre
    }

    // --- Sidecar ---
    {
        std::vector<uint8_t> rec(13, 0);
        put32(rec, 0, 11); put32(rec, 4, 22); put32(rec, 8, 33);
        rec[12] = 6;
        const HLas::PointView v(rec.data(), 1, 13, HLas::PointView::kSidecarFormat,
                                0.01, 0.01, 0.01, 100.0, 200.0, 0.0);
        CHECK(v.raw_x(0) == 11);
        CHECK(std::fabs(v.x(0) - 100.11) < 1e-9);
        CHECK(v.classification(0) == 6);
        // Sidecaren BÄR inte de andra fälten, och vyn ska säga det rakt ut
        // i stället för att låtsas att de är noll.
        CHECK(!v.has_intensity() && !v.has_gps_time() && !v.has_rgb());
        CHECK(!v.has_returns() && !v.has_scan_angle() && !v.has_user_data());
        CHECK(!v.has_point_source() && !v.has_nir());
    }

    // --- Batch mot enstaka ---
    {
        std::vector<uint8_t> recs(30 * 4, 0);
        for (int i = 0; i < 4; ++i) {
            put32(recs, i * 30 + 0, i * 10);
            put32(recs, i * 30 + 4, i * 20);
            put32(recs, i * 30 + 8, i * 30);
        }
        const HLas::PointView v(recs.data(), 4, 30, 6, 0.5, 0.5, 0.5, 1.0, 2.0, 3.0);
        double bx[8], by[8], bz[8];
        CHECK(v.xyz(0, 4, bx, by, bz) == 4);
        for (uint64_t i = 0; i < 4; ++i) {
            CHECK(bx[i] == v.x(i));
            CHECK(by[i] == v.y(i));
            CHECK(bz[i] == v.z(i));
        }
        // Kortare än begärt vid slutet, aldrig längre.
        CHECK(v.xyz(2, 10, bx, by, bz) == 2);
        CHECK(bx[0] == v.x(2));
        CHECK(v.xyz(4, 1, bx, by, bz) == 0);
    }

    printf("  syntetiska poster: %d fel\n", fails);
    return fails;
}

// Klassbyten i punktformat 0–5 delas med tre flaggbitar. En markpunkt med
// synthetic satt lagras som 34, och läses den råa byten blir den osynlig för
// accepted[2] och hamnar i fel hink i histogrammet.
//
// Testet bygger en egen LAS 1.2-fil eftersom ingen av testfilerna har flaggorna
// satta — autzen har noll sådana punkter, mätt. Utan en fil som faktiskt
// använder dem hade rättningen inte gått att bevisa.
int test_classification_flags() {
    int fails = 0;
    printf("test_classification_flags:\n");

    const std::string path = testdata::tmp_path("hlas_classflags.las");

    // LAS 1.2, punktformat 0, 20 byte per post.
    constexpr uint32_t kOffset = 227;
    constexpr uint16_t kRecLen = 20;
    constexpr uint32_t kCount  = 6;

    std::vector<uint8_t> file(kOffset + kRecLen * kCount, 0);
    std::memcpy(file.data(), "LASF", 4);
    file[24] = 1; file[25] = 2;                                  // version 1.2
    { uint16_t hs = 227; std::memcpy(file.data() + 94, &hs, 2); }
    std::memcpy(file.data() + 96, &kOffset, 4);
    file[104] = 0;                                               // punktformat 0
    { uint16_t rl = kRecLen; std::memcpy(file.data() + 105, &rl, 2); }
    std::memcpy(file.data() + 107, &kCount, 4);
    { double v = 0.01; for (int i = 0; i < 3; ++i) std::memcpy(file.data() + 131 + i*8, &v, 8); }
    { double v = 0.0;  for (int i = 0; i < 3; ++i) std::memcpy(file.data() + 155 + i*8, &v, 8); }
    { uint32_t by_return = kCount; std::memcpy(file.data() + 111, &by_return, 4); }
    // Utbredningen ska beskriva de LAGRADE koordinaterna: 0..500 med skala 0,01.
    { double lo = 0.0, hi = 5.0;
      for (int i = 0; i < 3; ++i) {
          std::memcpy(file.data() + 179 + i*16,     &hi, 8);
          std::memcpy(file.data() + 179 + i*16 + 8, &lo, 8);
      } }

    // Tre punkter med klass 2, varannan med en flaggbit satt, plus tre med klass 5.
    const uint8_t class_bytes[kCount] = {
        2,                    // ren klass 2
        2 | (1 << 5),         // klass 2 + synthetic  -> rå byte 34
        2 | (1 << 7),         // klass 2 + withheld   -> rå byte 130
        5,
        5 | (1 << 6),         // klass 5 + key-point  -> rå byte 69
        5,
    };
    for (uint32_t i = 0; i < kCount; ++i) {
        uint8_t* p = file.data() + kOffset + i * kRecLen;
        const int32_t v = static_cast<int32_t>(i) * 100;
        std::memcpy(p,     &v, 4);
        std::memcpy(p + 4, &v, 4);
        std::memcpy(p + 8, &v, 4);
        p[14] = 0x09;                 // retur 1 av 1
        p[15] = class_bytes[i];
    }

    // Kan filen inte skrivas är det ett fel, inte en skip: testet bygger sin egen
    // indata och har därför inget yttre beroende som legitimt kan saknas.
    {
        std::FILE* f = testdata::fopen_utf8(path, "wb");
        CHECK(f != nullptr);
        if (!f) return fails;
        const size_t wrote = std::fwrite(file.data(), 1, file.size(), f);
        CHECK(wrote == file.size());
        CHECK(std::fclose(f) == 0);
    }

    HLas::Reader r;
    CHECK(r.open(path.c_str()) == HLas::Status::Ok);
    if (!r.is_open()) { testdata::remove_utf8(path); return fails; }

    // Vyn
    const auto v = r.points();
    CHECK(v.count() == kCount);
    CHECK(v.point_format() == 0);
    for (uint32_t i = 0; i < kCount; ++i)
        CHECK(v.classification(i) == (class_bytes[i] & 0x1F));
    CHECK(v.synthetic(1) && !v.synthetic(0));
    CHECK(v.withheld(2)  && !v.withheld(0));
    CHECK(v.key_point(4) && !v.key_point(3));

    // Reader::get_classification — samma svar som vyn, inte råbyten
    for (uint32_t i = 0; i < kCount; ++i)
        CHECK(r.get_classification(i) == (class_bytes[i] & 0x1F));

    // Histogrammet: tre punkter i klass 2 och tre i klass 5, inget i 34/69/130
    {
        uint64_t counts[256] = {};
        CHECK(r.get_classification_histogram(counts) == HLas::Status::Ok);
        CHECK(counts[2] == 3);
        CHECK(counts[5] == 3);
        CHECK(counts[34] == 0 && counts[69] == 0 && counts[130] == 0);
    }

    // get_xyz_filtered och accepted[256] måste hitta alla tre klass 2-punkterna
    {
        double x[8], y[8], z[8];
        CHECK(r.get_xyz_filtered(0, kCount, 2, x, y, z) == 3);

        double px[] = { -1e6, 1e6, 1e6, -1e6 };
        double py[] = { -1e6, -1e6, 1e6, 1e6 };
        bool accepted[256] = {}; accepted[2] = true;
        uint8_t oc[8];
        const uint64_t n = r.get_xyzc_polygon(0, kCount, px, py, 4, x, y, z, oc,
                                              -1, accepted);
        CHECK(n == 3);
        for (uint64_t i = 0; i < n; ++i) CHECK(oc[i] == 2);
    }

    // Polygonvägarna har egna kopior av filtret och måste testas för sig —
    // annars kunde maskningen tas bort där utan att något föll.
    {
        double px[] = { -1e6, 1e6, 1e6, -1e6 };
        double py[] = { -1e6, -1e6, 1e6, 1e6 };
        double x[8], y[8], z[8];

        // get_xyz_polygon med klassfilter (en annan gren än get_xyzc_polygon)
        CHECK(r.get_xyz_polygon(0, kCount, px, py, 4, x, y, z, 2) == 3);
        CHECK(r.get_xyz_polygon(0, kCount, px, py, 4, x, y, z, 5) == 3);
        CHECK(r.get_xyz_polygon(0, kCount, px, py, 4, x, y, z, 34) == 0);

        uint64_t counts[256] = {};
        CHECK(r.get_classification_histogram_polygon(counts, px, py, 4)
              == HLas::Status::Ok);
        CHECK(counts[2] == 3 && counts[5] == 3);
        CHECK(counts[34] == 0 && counts[69] == 0 && counts[130] == 0);

        // DEM:ens accepted[] är ytterligare en kopia, i dem/dem.cpp. Egen
        // polygon här: punkterna ligger på 0..5 m, och ett rutnät över hela
        // filterpolygonen ovan hade varit tusentals kilometer brett.
        double dx[] = { -1.0, 6.0, 6.0, -1.0 };
        double dy[] = { -1.0, -1.0, 6.0, 6.0 };
        bool ground[256] = {}; ground[2] = true;
        const auto d = r.generate_dem(dx, dy, 4, 1.0, HLas::DemFill::None, ground);
        CHECK(d.cols > 0 && d.rows > 0);
        uint64_t cells = 0;
        for (float e : d.elevation) if (!std::isnan(e)) ++cells;
        CHECK(cells == 3);          // tre klass 2-punkter, alla i egna celler

        // Och taket: en polygon på tusen mil med metercell ska ge ett TOMT
        // resultat, inte försöka allokera hundratals GB och terminera.
        double hx[] = { -1e7, 1e7, 1e7, -1e7 };
        double hy[] = { -1e7, -1e7, 1e7, 1e7 };
        const auto huge = r.generate_dem(hx, hy, 4, 1.0);
        CHECK(huge.cols == 0 && huge.rows == 0 && huge.elevation.empty());
    }

    printf("  klass 2 med synthetic/withheld och klass 5 med key-point: maskade i\n"
           "    vy, get_classification, histogram (helt + polygon), get_xyz_filtered,\n"
           "    get_xyz_polygon, get_xyzc_polygon och DEM\n");

    r.close();
    testdata::remove_utf8(path);
    return fails;
}

// Fälten hela vägen ut ur en riktig fil. Det här är det Fas 2 finns för.
int test_point_view_files() {
    int fails = 0;
    printf("test_point_view_files:\n");

#ifndef HLAS_HAVE_LAZ
    // Både COPC-filen och .laz:en kräver avkodaren. Utan den finns inget att
    // läsa fälten ur, och det är inte ett fel — det är ett bygge utan LAZ.
    printf("  SKIP — byggt utan LAZ\n");
    return 0;
#else
    SKIP_IF_MISSING(testdata::kCopc);

    HLas::CopcReader cr;
    CHECK(cr.open(testdata::kCopc) == HLas::Status::Ok);
    if (!cr.is_open()) return fails;

    const auto& h = cr.header();
    CHECK(h.point_count == testdata::kPointCount);
    CHECK(h.point_format == 7);
    CHECK(h.point_record_length == testdata::kRecLen14);

    // Ett litet urval mitt i rutan.
    const double qx0 = h.min_x + (h.max_x - h.min_x) * 0.40;
    const double qx1 = h.min_x + (h.max_x - h.min_x) * 0.50;
    const double qy0 = h.min_y + (h.max_y - h.min_y) * 0.40;
    const double qy1 = h.min_y + (h.max_y - h.min_y) * 0.50;

    const auto est = cr.estimate(qx0, qy0, qx1, qy1);
    CHECK(est.point_count > 0);
    if (est.point_count == 0) return fails;

    std::vector<uint8_t> recs(static_cast<size_t>(est.point_count) * h.point_record_length);
    const uint64_t n = cr.read_region(qx0, qy0, qx1, qy1, recs.data(), est.point_count);
    CHECK(n > 0);
    if (n == 0) return fails;

    const auto v = cr.view(recs.data(), n);
    CHECK(v.count() == n);
    CHECK(v.point_format() == 7);
    CHECK(v.has_rgb());
    CHECK(v.has_gps_time());
    CHECK(v.has_intensity());
    CHECK(!v.has_nir());
    CHECK(v.extra_bytes() == 0);

    // Fälten måste faktiskt bära data. Ett urval där RGB är noll rakt igenom
    // vore precis det symptom 1.1.2 finns för att få bort, så det räcker inte
    // att accessorn kompilerar.
    uint64_t rgb_nonzero = 0, gps_nonzero = 0, int_nonzero = 0, ang_nonzero = 0;
    uint64_t outside = 0;
    double gmin = 0, gmax = 0;
    bool first = true;
    for (uint64_t i = 0; i < n; ++i) {
        uint16_t r, g, b;
        if (v.rgb(i, r, g, b) && (r | g | b)) ++rgb_nonzero;
        const double t = v.gps_time(i);
        if (t != 0.0) {
            ++gps_nonzero;
            if (first) { gmin = gmax = t; first = false; }
            else { if (t < gmin) gmin = t; if (t > gmax) gmax = t; }
        }
        if (v.intensity(i) != 0) ++int_nonzero;
        if (v.scan_angle(i) != 0.0) ++ang_nonzero;
        if (v.x(i) < qx0 || v.x(i) > qx1 || v.y(i) < qy0 || v.y(i) > qy1) ++outside;
    }
    CHECK(outside == 0);
    CHECK(rgb_nonzero > n / 2);
    CHECK(gps_nonzero > n / 2);
    CHECK(int_nonzero > n / 2);
    CHECK(ang_nonzero > 0);

    // GPS-tiden ska vara sammanhängande och rimlig, inte brus: en skanning
    // tar timmar eller dagar, inte år.
    CHECK(gmax > gmin);
    CHECK(gmax - gmin < 7.0 * 86400.0);

    printf("  %llu punkter: RGB %llu, GPS %llu, intensitet %llu, scanvinkel %llu\n",
           (unsigned long long)n, (unsigned long long)rgb_nonzero,
           (unsigned long long)gps_nonzero, (unsigned long long)int_nonzero,
           (unsigned long long)ang_nonzero);
    printf("  GPS-tid spänner %.1f s\n", gmax - gmin);

    // Strömmande vägen måste ge exakt samma fält som den buffrade — annars är
    // det två läsvägar, inte en.
    {
        struct Acc {
            std::vector<uint16_t> r;
            std::vector<double>   t;
            bool bad_format = false;
        } acc;
        auto cb = [](void* u, const HLas::PointView& pv) -> bool {
            auto* a = static_cast<Acc*>(u);
            if (pv.point_format() != 7 || !pv.has_rgb()) a->bad_format = true;
            for (uint64_t i = 0; i < pv.count(); ++i) {
                uint16_t r, g, b;
                pv.rgb(i, r, g, b);
                a->r.push_back(r);
                a->t.push_back(pv.gps_time(i));
            }
            return true;
        };
        const uint64_t m = cr.read_region_streaming(qx0, qy0, qx1, qy1, cb, &acc);
        CHECK(m == n);
        CHECK(!acc.bad_format);
        CHECK(acc.r.size() == n);
        uint64_t diffs = 0;
        for (uint64_t i = 0; i < n && i < acc.r.size(); ++i) {
            uint16_t r, g, b;
            v.rgb(i, r, g, b);
            if (acc.r[i] != r || acc.t[i] != v.gps_time(i)) ++diffs;
        }
        CHECK(diffs == 0);
        printf("  strömmande vs buffrad: %llu avvikelser i RGB och GPS-tid\n",
               (unsigned long long)diffs);
    }

    // Reader::points() över hela filen ska bära samma fält.
    {
        HLas::Reader r;
        CHECK(r.open(testdata::kCopc) == HLas::Status::Ok);
        if (r.is_open()) {
            const auto pv = r.points();
            CHECK(pv.valid());
            CHECK(pv.count() == testdata::kPointCount);
            CHECK(pv.point_format() == 7);
            CHECK(pv.has_rgb() && pv.has_gps_time() && pv.has_intensity());

            uint64_t nz = 0;
            const uint64_t sample = 10000;
            for (uint64_t i = 0; i < sample; ++i) {
                uint16_t rr, gg, bb;
                if (pv.rgb(i, rr, gg, bb) && (rr | gg | bb)) ++nz;
            }
            CHECK(nz > sample / 2);
            printf("  Reader::points(): %llu punkter, format %u, RGB i %llu av %llu\n",
                   (unsigned long long)pv.count(), pv.point_format(),
                   (unsigned long long)nz, (unsigned long long)sample);
        }
    }

        // Sidecar-läget måste säga att det ÄR sidecar. En vy som påstod format 7
    // över en 13-bytespost hade läst RGB ur nästa punkts koordinater — tyst,
    // och med värden som ser rimliga ut.
    if (testdata::exists(testdata::kLaz)) {
        HLas::Reader cached;
        if (cached.open(testdata::kLaz) == HLas::Status::Ok) {
            const auto pv = cached.points();
            if (pv.record_length() == 13) {
                CHECK(pv.point_format() == HLas::PointView::kSidecarFormat);
                CHECK(!pv.has_rgb() && !pv.has_gps_time() && !pv.has_intensity());
                printf("  sidecar-läge: format %u, %u byte per post, inga fält utöver geometri\n",
                       pv.point_format(), pv.record_length());
            }

            // Samma fil utan cache bär allt igen.
            HLas::Reader raw;
            CHECK(raw.open(testdata::kLaz) == HLas::Status::Ok);
            const auto rv = raw.points();
            CHECK(rv.point_format() != HLas::PointView::kSidecarFormat);
            CHECK(rv.has_gps_time());
            CHECK(rv.count() == pv.count());
        }
    }

    return fails;
#endif // HLAS_HAVE_LAZ
}
