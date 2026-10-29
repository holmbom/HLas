// Gyllene vektorer för punktformat 0-5: VÅR kod mot REFERENSENS.
//
// Samma skäl som test_golden.cpp — rundgång mot vår egen kod kan inte upptäcka
// ett delat missförstånd — men ett annat filupplägg. Format 0-5 använder
// compressor 2: ingen lagerindelning, en enda aritmetisk ström där varje punkts
// items kodas efter varandra. Chunken är alltså seed-posten rått följd av den
// strömmen, och inget annat.
//
// Testerna driver Point10-, GPSTime11- och RGB12-kodarna och -avkodarna DIREKT. VLR-tolkningen och
// item-utpekningen på LÄSSIDAN ligger i LazReader och täcks av de filbaserade
// testerna. På SKRIVSIDAN finns ingen motsvarighet ännu — LazWriter kan bara
// compressor 3, och chunk-skrivningen för 0-5 kommer i en senare fas.

#include "golden_ref10.h"

#ifdef HLAS_HAVE_LAZ
#include "laz/arithmetic.h"
#include "laz/arithmetic_encoder.h"
#include "laz/point10.h"
#include "laz/point10_encoder.h"
#include "laz/point10_item.h"
#include "laz/gpstime11.h"
#include "laz/gpstime11_encoder.h"
#include "laz/rgb12.h"
#include "laz/rgb12_encoder.h"
#endif

#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

#ifdef HLAS_HAVE_LAZ
namespace {

using namespace HLas::detail::laz;

struct Case10 {
    const char*    name;
    const uint8_t* records;
    size_t         records_len;   // arrayens FAKTISKA storlek, inte count * längd
    uint32_t       count;
    uint16_t       record_length;
    uint8_t        format;
    const uint8_t* chunk;
    size_t         chunk_len;
};

#define CASE10(n) Case10{ #n, testdata::golden10::kP10_##n##_Records, \
                          sizeof(testdata::golden10::kP10_##n##_Records), \
                          testdata::golden10::kP10_##n##_Count, \
                          testdata::golden10::kP10_##n##_RecordLength, \
                          testdata::golden10::kP10_##n##_Format, \
                          testdata::golden10::kP10_##n##_Chunk, \
                          sizeof(testdata::golden10::kP10_##n##_Chunk) }

// Vilka items ett punktformat har, och därmed postens längd. Ordningen är
// LAZ-VLR:ns: Point10, GPSTime11, RGB12. Den ordningen är inte fri — items
// delar EN ström, så ett utelämnat item flyttar alla följande.
struct Layout10 {
    bool     has_gps;
    bool     has_rgb;
    uint16_t record_length;
};

bool layout_for(uint8_t format, Layout10& l) {
    switch (format) {
        case 0: l = Layout10{ false, false, 20 }; return true;
        case 1: l = Layout10{ true,  false, 28 }; return true;
        case 2: l = Layout10{ false, true,  26 }; return true;
        case 3: l = Layout10{ true,  true,  34 }; return true;
        default: return false;
    }
}

// Facit är en GENERERAD fil, men den ligger i repot och kan redigeras för hand.
// Gör någon det inkonsekvent — höjer Count utan att utöka Records — ska det bli
// ett testfel och inte en läsning utanför arrayen.
bool case_consistent(const Case10& c) {
    Layout10 l{};
    if (!layout_for(c.format, l)) return false;
    if (c.count == 0) return false;
    if (c.record_length != l.record_length) return false;
    if (c.count > c.records_len / c.record_length) return false;
    return c.records_len == static_cast<size_t>(c.count) * c.record_length;
}

const Case10 kCases[] = {
    CASE10(p10_basic),
    CASE10(p10_returns),
    CASE10(p10_flags),
    CASE10(p10_constant),
    CASE10(p10_intensity),
    CASE10(p10_extremes),
    CASE10(p10_attrs),
    CASE10(p10_gps),
    CASE10(p10_gps_repeat),
    CASE10(p10_gps_multi),
    CASE10(p10_gps_frames),
    CASE10(p10_rgb),
    CASE10(p10_rgb_gray),
    CASE10(p10_full),
};

// Packar upp en compressor 2-chunk med våra avkodare.
//
// Seed-posten ligger rå först; resten är EN aritmetisk ström där varje punkts
// items avkodas efter varandra. Avkodarna delar alltså dekodern och måste
// anropas i VLR-ordning — hoppar man över ett item läser nästa fel byte.
bool decode_chunk10(const Case10& c, std::vector<uint8_t>& out) {
    Layout10 l{};
    if (!case_consistent(c) || !layout_for(c.format, l)) return false;
    if (c.chunk_len <= static_cast<size_t>(c.record_length)) return false;

    out.assign(static_cast<size_t>(c.count) * c.record_length, 0);

    const uint8_t* seed = c.chunk;
    ArithmeticDecoder dec;
    dec.init(c.chunk + c.record_length, c.chunk_len - c.record_length);

    Point10Decoder p10;
    p10.init(dec);
    GPSTime11Decoder gps;
    if (l.has_gps) gps.init(dec);
    RGB12Decoder rgb;
    if (l.has_rgb) rgb.init(dec);

    for (uint32_t i = 0; i < c.count; ++i) {
        uint8_t* dst = out.data() + static_cast<size_t>(i) * c.record_length;
        const bool s = (i == 0);
        uint16_t off = 0;
        p10.decompress(s ? seed : nullptr, dst);
        off += kPoint10Size;
        if (l.has_gps) {
            gps.decompress(s ? seed + off : nullptr, dst + off);
            off += 8;
        }
        if (l.has_rgb)
            rgb.decompress(s ? seed + off : nullptr, dst + off);
    }
    return true;
}

// Kodar posterna med våra kodare och bygger chunken som skrivaren måste göra:
// seed-posten rå först, sedan strömmen. Samma item-ordning som avkodningen, av
// samma skäl.
bool encode_chunk10(const Case10& c, std::vector<uint8_t>& out) {
    Layout10 l{};
    if (!case_consistent(c) || !layout_for(c.format, l)) return false;

    out.assign(c.records, c.records + c.record_length);

    ArithmeticEncoder enc;
    enc.init();

    Point10Encoder p10;
    p10.init(enc);
    p10.start_chunk(c.records);

    GPSTime11Encoder gps;
    RGB12Encoder rgb;
    uint16_t seed_off = kPoint10Size;
    if (l.has_gps) {
        gps.init(enc);
        gps.start_chunk(c.records + seed_off);
        seed_off += 8;
    }
    if (l.has_rgb) {
        rgb.init(enc);
        rgb.start_chunk(c.records + seed_off);
    }

    for (uint32_t i = 1; i < c.count; ++i) {
        const uint8_t* rec = c.records + static_cast<size_t>(i) * c.record_length;
        uint16_t off = kPoint10Size;
        p10.encode(rec);
        if (l.has_gps) { gps.encode(rec + off); off += 8; }
        if (l.has_rgb) rgb.encode(rec + off);
    }

    enc.done();
    if (p10.failed() || (l.has_gps && gps.failed()) || (l.has_rgb && rgb.failed()) ||
        enc.failed())
        return false;

    out.insert(out.end(), enc.bytes().begin(), enc.bytes().end());
    return true;
}

// Var skiljer sig två byteföljder? Utan positionen är ett fel bara "olika".
void report_diff(const char* name, const std::vector<uint8_t>& got,
                 const uint8_t* want, size_t want_len) {
    printf("  %s: %zu byte mot %zu förväntade", name, got.size(), want_len);
    const size_t n = got.size() < want_len ? got.size() : want_len;
    for (size_t i = 0; i < n; ++i) {
        if (got[i] != want[i]) {
            printf(", första avvikelse vid %zu (0x%02X mot 0x%02X)", i, got[i], want[i]);
            break;
        }
    }
    printf("\n");
}


// ---- Att kommentarerna är sanna ---------------------------------------------
//
// Bytejämförelsen ovan säger bara "lika" eller "olika". Den kan inte upptäcka
// att ett fall INTE täcker det som står om det — och exakt det hände: p10_returns
// påstods täcka alla 64 rutor i tabellerna medan seed-posten gick förbi dem, så
// kombinationen 0 av 0 aldrig kodades. Kontrollerna nedan gör påståendena
// falsifierbara i stället för att lita på att den som skrev fallet räknade rätt.

const Case10* find_case(const char* name) {
    for (const Case10& c : kCases)
        if (std::strcmp(c.name, name) == 0) return &c;
    return nullptr;
}

// Alla 64 rutor i kReturnMap/kReturnLevel måste nås av en KODAD punkt.
// Seed-posten räknas inte: den skrivs rå och rör aldrig tabellerna.
int check_returns_coverage() {
    int fails = 0;
    const Case10* c = find_case("p10_returns");
    if (!c || !case_consistent(*c)) { printf("  FAIL: p10_returns saknas\n"); return 1; }

    bool seen[8][8] = {};
    for (uint32_t i = 1; i < c->count; ++i) {
        const uint8_t* p = c->records + static_cast<size_t>(i) * c->record_length;
        seen[get10_number_of_returns(p)][get10_return_number(p)] = true;
    }
    int missing = 0;
    for (int n = 0; n < 8; ++n)
        for (int r = 0; r < 8; ++r)
            if (!seen[n][r]) ++missing;
    if (missing) {
        printf("  FAIL: p10_returns når %d av 64 rutor\n", 64 - missing);
        ++fails;
    }
    return fails;
}

// p10_intensity ska skilja den rätta jämförelsen från den uppenbara.
//
// Kontrollen simulerar båda: intensitetsbiten mot senaste intensiteten för
// samma retur-klass, och mot föregående punkt. Skiljer de sig aldrig åt är
// fallet tandlöst, oavsett att bytena stämmer.
int check_intensity_discriminates() {
    int fails = 0;
    const Case10* c = find_case("p10_intensity");
    if (!c || !case_consistent(*c)) { printf("  FAIL: p10_intensity saknas\n"); return 1; }

    uint16_t last_per_m[16] = {};      // seed fyller den INTE — samma regel som kodaren
    uint32_t diverging = 0;
    uint16_t prev_intensity = 0;
    std::memcpy(&prev_intensity, c->records + OFF10_INTENSITY, 2);

    for (uint32_t i = 1; i < c->count; ++i) {
        const uint8_t* p = c->records + static_cast<size_t>(i) * c->record_length;
        uint16_t v;
        std::memcpy(&v, p + OFF10_INTENSITY, 2);
        const uint8_t m = kReturnMap[get10_number_of_returns(p)][get10_return_number(p)];

        const bool correct = (v != last_per_m[m]);
        const bool naive   = (v != prev_intensity);
        if (correct != naive) ++diverging;

        if (correct) last_per_m[m] = v;
        prev_intensity = v;
    }
    if (diverging == 0) {
        printf("  FAIL: p10_intensity skiljer inte de två jämförelserna åt\n");
        ++fails;
    }
    return fails;
}

// Båda scanvinkelmodellerna ska användas, alltså båda scanriktningarna
// tillsammans med en ÄNDRAD vinkel — en oförändrad vinkel kodas inte alls och
// når därför ingen modell.
int check_scan_angle_models() {
    int fails = 0;
    const Case10* c = find_case("p10_flags");
    if (!c || !case_consistent(*c)) { printf("  FAIL: p10_flags saknas\n"); return 1; }

    bool used[2] = {};
    for (uint32_t i = 1; i < c->count; ++i) {
        const uint8_t* p    = c->records + static_cast<size_t>(i) * c->record_length;
        const uint8_t* prev = p - c->record_length;
        if (p[OFF10_SCAN_ANGLE] != prev[OFF10_SCAN_ANGLE])
            used[get10_scan_direction(p)] = true;
    }
    if (!used[0] || !used[1]) {
        printf("  FAIL: p10_flags når scanvinkelmodell %s\n",
               used[0] ? "0 men inte 1" : (used[1] ? "1 men inte 0" : "ingen"));
        ++fails;
    }
    return fails;
}

// Koordinaterna ska nå int32:ans båda ändar, som är det fallet finns för.
int check_extremes_present() {
    int fails = 0;
    const Case10* c = find_case("p10_extremes");
    if (!c || !case_consistent(*c)) { printf("  FAIL: p10_extremes saknas\n"); return 1; }

    bool has_min = false, has_max = false;
    for (uint32_t i = 0; i < c->count; ++i) {
        const uint8_t* p = c->records + static_cast<size_t>(i) * c->record_length;
        for (int off : { OFF10_X, OFF10_Y, OFF10_Z }) {
            int32_t v;
            std::memcpy(&v, p + off, 4);
            if (v == INT32_MIN) has_min = true;
            if (v == INT32_MAX) has_max = true;
        }
    }
    if (!has_min || !has_max) {
        printf("  FAIL: p10_extremes saknar INT32_%s\n", has_min ? "MAX" : "MIN");
        ++fails;
    }
    return fails;
}

} // namespace
#endif // HLAS_HAVE_LAZ

int test_golden10() {
    int fails = 0;
#ifndef HLAS_HAVE_LAZ
    printf("[test_golden10] SKIP — byggt utan LAZ\n");
#else
    for (const Case10& c : kCases) {
        std::vector<uint8_t> got;
        if (!decode_chunk10(c, got)) {
            printf("  FAIL: %s kunde inte packas upp\n", c.name);
            ++fails;
            continue;
        }
        const size_t want = static_cast<size_t>(c.count) * c.record_length;
        if (got.size() != want || std::memcmp(got.data(), c.records, want) != 0) {
            report_diff(c.name, got, c.records, want);
            ++fails;
        }
    }
    fails += check_returns_coverage();
    fails += check_intensity_discriminates();
    fails += check_scan_angle_models();
    fails += check_extremes_present();
    if (fails == 0)
        printf("[test_golden10] OK — %zu vektorer ur referensen uppackade med Point10,"
               " GPSTime11 och RGB12; alla 64 returrutor kodade, intensitetsfallet"
               " skiljer jämförelserna åt\n",
               sizeof(kCases) / sizeof(kCases[0]));
#endif
    return fails;
}

int test_golden10_encode() {
    int fails = 0;
#ifndef HLAS_HAVE_LAZ
    printf("[test_golden10_encode] SKIP — byggt utan LAZ\n");
#else
    for (const Case10& c : kCases) {
        std::vector<uint8_t> got;
        if (!encode_chunk10(c, got)) {
            printf("  FAIL: %s kunde inte kodas\n", c.name);
            ++fails;
            continue;
        }
        if (got.size() != c.chunk_len ||
            std::memcmp(got.data(), c.chunk, c.chunk_len) != 0) {
            report_diff(c.name, got, c.chunk, c.chunk_len);
            ++fails;
        }
    }
    if (fails == 0)
        printf("[test_golden10_encode] OK — %zu chunkar BYTE-IDENTISKA med referensens\n",
               sizeof(kCases) / sizeof(kCases[0]));
#endif
    return fails;
}
