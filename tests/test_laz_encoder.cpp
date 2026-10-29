// Fuzz på kodare/avkodare-paret.
//
// VAD DET HÄR BEVISAR: att kodaren och avkodaren är konsistenta med varandra.
// För den aritmetiska kodaren är det nästan hela kontraktet — den har inget annat
// åtagande än att avkodaren ska få tillbaka exakt det som stoppades in.
//
// VAD DET INTE BEVISAR: att strömmen är den en annan LAZ-kodare skulle ha producerat. Två
// giltiga aritmetiska kodare kan avsluta olika och ändå avkodas likadant av var
// och en av sina egna avkodare. Att vår utdata går att läsa för andra avgörs
// först av korsvalideringen mot en oberoende avkodare, längre fram i arbetet.
// Blanda inte ihop de två: "1,4 miljoner operationer gröna" säger ingenting om
// interoperabilitet.
//
// Det som jagas här är carry-propageringen. Den kräver att en 0xFF-svit står i
// utströmmen i samma ögonblick som intervallet spiller över 2^32 — sällsynt,
// datastyrt och tyst. Enstaka handskrivna fall hittar den inte; miljontals
// slumpade operationer gör det. Testet KRÄVER dessutom att vägen faktiskt körs
// (se carry_count nedan), så en framtida ändring som råkar sluta nå den blir ett
// misslyckat test i stället för en tystnad.

#include "laz/arithmetic_encoder.h"
#include "laz/integer_compressor.h"
#include "laz/point14_encoder.h"
#include "laz/point14.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

#ifdef HLAS_HAVE_LAZ

using namespace HLas::detail::laz;

namespace {

// Egen PRNG, inte <random>: ett fynd måste gå att reproducera exakt med sitt
// frö, oavsett bibliotek och plattform.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<uint32_t>(s >> 32);
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

enum class Op : uint8_t { Symbol, Bit, Raw };

struct Step {
    Op       op;
    uint8_t  model;      // index i models_ / bits_
    uint32_t value;      // symbol, bit, eller råvärde
    uint8_t  raw_bits;   // bara för Raw
};

// Modelluppsättningen är densamma i båda riktningarna. Storlekarna är valda för
// att träffa kodarens grenar: 2 är minimum, 3 och 13 är udda, 16 och 256 är de
// som Point14 faktiskt använder.
// 2 är minimum, 3 och 13 är udda, 16 och 256 använder Point14, och 515 är
// GPSTIME_MULTI_TOTAL — den största modellen som förekommer i formatet.
constexpr uint32_t kModelSizes[] = { 2, 3, 13, 16, 128, 256, 515 };
constexpr int      kNumModels    = 7;
constexpr int      kNumBitModels = 3;

struct ModelSet {
    SymbolModel sym[kNumModels];
    BitModel    bit[kNumBitModels];
    ModelSet() {
        for (int i = 0; i < kNumModels; ++i) sym[i].init(kModelSizes[i]);
        for (int i = 0; i < kNumBitModels; ++i) bit[i].init();
    }
};

// En körning: bygg ett slumpat program, koda det, avkoda det, kräv identiskt.
// `ff_bias` styr hur ofta råvärden sätts till lutter ettor — det är så
// 0xFF-sviter uppstår i utströmmen, och alltså så carry-vägen nås.
struct RoundStats { int fails = 0; uint64_t carries = 0; uint64_t carry_ff = 0; };

// `mode` 0 = uniform slump, 1 = 0xFF-övervikt i råvärden, 2 = långa serier av
// samma symbol/bit — en snedfördelning som ställer helt andra krav på
// modellerna än uniform slump.
//
// Obs: de här körningarna når INTE modellernas halveringar. Varje körning
// skapar nya modeller och gör som mest några tusen operationer, medan
// DM_MAX_COUNT är 32768. Halveringarna testas separat och deterministiskt
// nedan — summan 2 miljoner operationer är ett aggregat över 1200 skilda
// modelluppsättningar och säger ingenting om hur långt en enskild modell drevs.
RoundStats run_round(uint64_t seed, uint32_t steps, int mode) {
    RoundStats st;
    int fails = 0;
    const bool ff_bias = (mode == 1);
    const bool runs    = (mode == 2);
    Rng rng(seed);
    uint32_t hold_kind = 0, hold_model = 0, hold_value = 0, hold_left = 0;

    std::vector<Step> prog;
    prog.reserve(steps);
    for (uint32_t i = 0; i < steps; ++i) {
        Step s{};
        if (runs) {
            // Håll fast vid samma operation och värde en stund i taget.
            if (hold_left == 0) {
                hold_left  = 20 + rng.below(400);
                hold_kind  = rng.below(2);
                hold_model = rng.below(hold_kind == 0 ? kNumModels : kNumBitModels);
                hold_value = (hold_kind == 0) ? rng.below(kModelSizes[hold_model])
                                              : rng.below(2);
            }
            --hold_left;
            s.op    = (hold_kind == 0) ? Op::Symbol : Op::Bit;
            s.model = static_cast<uint8_t>(hold_model);
            s.value = hold_value;
            prog.push_back(s);
            continue;
        }
        const uint32_t pick = rng.below(10);
        if (pick < 5) {
            s.op    = Op::Symbol;
            s.model = static_cast<uint8_t>(rng.below(kNumModels));
            s.value = rng.below(kModelSizes[s.model]);
        } else if (pick < 8) {
            s.op    = Op::Bit;
            s.model = static_cast<uint8_t>(rng.below(kNumBitModels));
            s.value = rng.below(2);
        } else {
            s.op       = Op::Raw;
            s.raw_bits = static_cast<uint8_t>(1 + rng.below(32));
            if (ff_bias && rng.below(2)) {
                // Alla bitar satta — det är den här sortens värde som fyller
                // utströmmen med 0xFF och gör carry möjlig.
                s.value = (s.raw_bits == 32) ? 0xFFFFFFFFu
                                             : ((1u << s.raw_bits) - 1u);
            } else {
                s.value = (s.raw_bits == 32) ? rng.next()
                                             : (rng.next() & ((1u << s.raw_bits) - 1u));
            }
        }
        prog.push_back(s);
    }

    ModelSet enc_models;
    ArithmeticEncoder enc;
    enc.init();
    for (const Step& s : prog) {
        switch (s.op) {
            case Op::Symbol: enc.encodeSymbol(enc_models.sym[s.model], s.value); break;
            case Op::Bit:    enc.encodeBit(enc_models.bit[s.model], s.value);    break;
            case Op::Raw:    enc.writeBits(s.raw_bits, s.value);                 break;
        }
    }
    enc.done();

    CHECK(!enc.failed());
    st.carries  = enc.carry_count();
    st.carry_ff = enc.carry_ff_count();
    if (enc.failed()) {
        printf("  (seed %llu, steps %u)\n", (unsigned long long)seed, steps);
        st.fails = fails;
        return st;
    }

    ModelSet dec_models;
    ArithmeticDecoder dec;
    dec.init(enc.bytes().data(), enc.bytes().size());

    for (uint32_t i = 0; i < prog.size(); ++i) {
        const Step& s = prog[i];
        uint32_t got = 0;
        switch (s.op) {
            case Op::Symbol: got = dec.decodeSymbol(dec_models.sym[s.model]); break;
            case Op::Bit:    got = dec.decodeBit(dec_models.bit[s.model]);    break;
            case Op::Raw:    got = dec.readBits(s.raw_bits);                  break;
        }
        if (got != s.value) {
            printf("  FAIL: seed %llu, steg %u/%u: fick %u, väntade %u\n",
                   (unsigned long long)seed, i, (unsigned)prog.size(),
                   got, s.value);
            ++fails;
            st.fails = fails;
            return st;   // vidare steg är meningslösa, strömmen är ur fas
        }
    }
    st.fails = fails;
    return st;
}


// ---- IntegerCompressor -------------------------------------------------
//
// Bygger ett fast program som täcker alla grenar i writeCorrector/readCorrector:
// k == 0 (korrektor 0 eller 1), små k som kodas med en symbol, k > bits_high
// som kodas med symbol plus råbitar, och k == bits som bara nås av corr_min.
inline void build_ic_program(uint32_t bits,
                             std::vector<int32_t>& preds,
                             std::vector<int32_t>& reals) {
    const int64_t range = (bits < 32) ? (int64_t(1) << bits) : (int64_t(1) << 32);
    const int32_t cmin  = (bits < 32) ? -static_cast<int32_t>(1u << (bits - 1)) : INT32_MIN;
    const int32_t cmax  = (bits < 32) ?  static_cast<int32_t>((1u << (bits - 1)) - 1) : INT32_MAX;
    const int32_t wanted[] = { 0, 1, -1, 2, -2, 3, -3, 127, -128, 255, -256,
                               1000, -1000, cmin, cmax, cmin + 1, cmax - 1, 0, 1, -1 };
    for (int32_t c : wanted) {
        if (c < cmin || c > cmax) continue;   // ryms inte i den här bredden
        int64_t pr = (bits < 32) ? (range / 2) : 0;
        int64_t rl = pr + c;
        if (bits < 32) { if (rl < 0) rl += range; else if (rl >= range) rl -= range; }
        preds.push_back(static_cast<int32_t>(pr));
        reals.push_back(static_cast<int32_t>(rl));
    }
}

// Gyllene bytevektorer: exakt utdata för programmet ovan.
//
// De är INTE bara en regression mot oss själva. Varje vektor har avkodats av
// referensen med rätt värden innan den skrevs in här, alltså av en oberoende
// implementation. När referensen är borta ur bygget är det här som är kvar av
// det beviset — en CHANGELOG-rad hade varit en anteckning, det här går att köra.
//
// Proveniens, så de går att göra om: genererade av build_ic_program() nedan och
// verifierade mot referensen vid commit 14522addcb01125499119990cc8fbc6b1e43b148,
// samma commit som CMakeLists pinnade när referensen ännu användes.
// Verifieringen gjordes med referensens heltalskompressor mot dess
// aritmetiska avkodare. Ändras build_ic_program måste vektorerna
// genereras OCH verifieras om — inte bara uppdateras till det vi råkar skriva.
constexpr uint8_t kGolden8[] = {
    0x00, 0xE2, 0x9D, 0x27, 0xCB, 0x1B, 0x0A, 0x71, 0xF6, 0x3B, 0x0E, 0x29,
    0x12, 0x0E, 0x9E, 0x00, 0x00,
};
constexpr uint8_t kGolden16[] = {
    0x00, 0x3C, 0x21, 0x74, 0xF9, 0xCF, 0x4E, 0xD2, 0x62, 0x17, 0x26, 0xE4,
    0x86, 0xE4, 0x17, 0x4F, 0xDC, 0xD7, 0x7C, 0xEB, 0x7D, 0x90, 0xE2, 0x8C,
    0xD0, 0x83, 0xD0, 0x00, 0x00, 0x00,
};
constexpr uint8_t kGolden32[] = {
    0x00, 0x0F, 0x79, 0x13, 0xEC, 0x3E, 0x9D, 0x90, 0xEB, 0x2D, 0xF6, 0xAB,
    0x33, 0x2E, 0xF7, 0x8E, 0x86, 0x6F, 0x6E, 0x85, 0xAF, 0x17, 0x32, 0x78,
    0x08, 0x09, 0x7F, 0xFF, 0x66, 0xE7, 0x37, 0xD9, 0xC8, 0x00, 0x00, 0x00,
};


// ---- Point14 ------------------------------------------------------------

inline void p14_to_item(const Point14Fields& f, uint8_t* o) {
    std::memset(o, 0, 30);
    std::memcpy(o + 0, &f.x, 4);
    std::memcpy(o + 4, &f.y, 4);
    std::memcpy(o + 8, &f.z, 4);
    o[14] = 0x11;                 // retur 1 av 1
    o[16] = f.classification;
}

// Fast program: identiska punkter, jämna steg, stora hopp, wrappande
// int32-differenser, konstant och växlande klass inklusive 0, 31, 32 och 255.
inline void build_p14_program(std::vector<Point14Fields>& p) {
    p.push_back({100000, 200000, 3000, 2});
    for (int i = 0; i < 4; ++i) p.push_back({100000, 200000, 3000, 2});
    for (int i = 1; i <= 8; ++i) p.push_back({100000 + i*13, 200000 - i*7, 3000 + i, 2});
    p.push_back({100000 + 5000000, 200000 - 4000000, 12345, 9});
    p.push_back({INT32_MAX, INT32_MIN, 0, 0});
    p.push_back({INT32_MIN, INT32_MAX, 1, 31});
    p.push_back({0, 0, INT32_MAX, 32});
    p.push_back({-1, -1, INT32_MIN, 255});
    for (int i = 0; i < 6; ++i) p.push_back({i*3, i*5, i*2, static_cast<uint8_t>(i*40)});
    p.push_back({7, 7, 7, 2});
    p.push_back({7, 7, 7, 2});
}

// Gyllene vektor för Point14-lagren till programmet ovan.
//
// Den är BYTE-IDENTISK med vad referensens egen Point14Compressor producerar för
// samma punkter, och referensens Point14Decompressor läser tillbaka den till exakt
// rätt punkter. Båda kontrollerades innan vektorn skrevs in.
//
// Byte-identitet är ett starkare påstående än interoperabilitet: det säger att
// varje modell, varje kontext och strömavslutningen är kopplade precis som i
// referensen, inte bara att resultatet råkar gå att läsa.
//
// Proveniens: referensen commit 14522addcb01125499119990cc8fbc6b1e43b148. Ändras
// build_p14_program måste vektorn genereras OCH verifieras om mot referensen —
// inte uppdateras till det vi råkar skriva.
//
// De nio första uint32:orna är lagerlängderna. Att sex av dem är noll är hela
// lagerpolicyn, direkt avläsbar i vektorn.
constexpr uint8_t kGoldenP14[] = {
    0x56, 0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x1D, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x91,
    0xEE, 0xC3, 0x27, 0x2E, 0x22, 0x13, 0xBD, 0xE7, 0xF8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0xCC, 0x36, 0x1C, 0xCD, 0x06,
    0x0A, 0x99, 0x4A, 0x47, 0x9D, 0xC5, 0xB2, 0xE4, 0x35, 0x3B, 0x5D, 0x2A,
    0x83, 0xCE, 0xC4, 0x10, 0x4F, 0x7A, 0x46, 0x0E, 0x75, 0xB1, 0x59, 0xE4,
    0x11, 0x22, 0x16, 0xC4, 0x0D, 0x17, 0x03, 0xA6, 0xFE, 0x85, 0x12, 0xCA,
    0x39, 0xF5, 0xB7, 0x20, 0x00, 0x03, 0x62, 0x14, 0xE1, 0x61, 0x1C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0xB6, 0x57, 0x03, 0x6B, 0xB3, 0x41,
    0xFE, 0x26, 0xDD, 0x30, 0x2A, 0x8C, 0xBF, 0xB8, 0x6B, 0x98, 0xA8, 0x65,
    0x99, 0x91, 0x13, 0xA3, 0xA5, 0x00, 0x00, 0x00, 0x02, 0x02, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x07, 0xFF, 0x1A, 0xA0,
    0xEF, 0x6F, 0xA8, 0xD0, 0x64, 0x78, 0x8B, 0xB1, 0x9D, 0xFF, 0x00, 0x00,
    0x00,
};

// Sveper differensmagnituder runt kontextgränserna: dy_ctx byter vid kx == 20
// och z_ctx vid kxy == 18. Slumpdata träffar dem, men bevisar dem inte —
// det här programmet gör det avsiktligt, i båda riktningarna.
inline void build_p14_ctx_program(std::vector<Point14Fields>& p) {
    int64_t x = 0, y = 0, z = 0;
    p.push_back({0, 0, 0, 2});
    for (int k = 15; k <= 24; ++k) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            const int64_t d = int64_t(1) << k;
            x += sign * d;  y += sign * (d - 1);  z += sign * (d / 2);
            p.push_back({static_cast<int32_t>(x), static_cast<int32_t>(y),
                         static_cast<int32_t>(z), 2});
            // Liten differens direkt efter, så nästa punkt hamnar i en annan
            // kontext än den stora.
            x += sign;  y -= sign;  z += sign;
            p.push_back({static_cast<int32_t>(x), static_cast<int32_t>(y),
                         static_cast<int32_t>(z), 2});
        }
    }
}

constexpr uint8_t kGoldenP14Ctx[] = {
    0xBC, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xF7, 0xFD, 0xE0, 0xE7, 0xAD, 0x78, 0xA5, 0x24, 0xC0, 0x61, 0x8A,
    0x95, 0x87, 0xDE, 0x03, 0x45, 0x33, 0x0A, 0xBD, 0x50, 0x9C, 0xB2, 0x2B,
    0x29, 0x43, 0xEF, 0x80, 0x63, 0x23, 0xD0, 0x6D, 0x01, 0x5D, 0x05, 0xAB,
    0xFB, 0x32, 0xA5, 0x95, 0x66, 0x00, 0x7E, 0xCF, 0x87, 0xE3, 0xA0, 0x55,
    0xAC, 0x30, 0x08, 0x9E, 0x08, 0x28, 0x12, 0x45, 0x1F, 0x8A, 0x53, 0xB1,
    0x75, 0x65, 0x47, 0x81, 0xE9, 0xE7, 0x21, 0xA0, 0x04, 0xB4, 0x88, 0x38,
    0x6D, 0xD5, 0x13, 0x07, 0xD5, 0xDE, 0xB2, 0xC0, 0xA0, 0x2C, 0xE3, 0xB5,
    0xBA, 0x74, 0x01, 0x36, 0x0A, 0x17, 0x89, 0x81, 0x92, 0x7E, 0xC1, 0x83,
    0xA4, 0xC3, 0x5E, 0xB9, 0x93, 0x82, 0xD2, 0x0E, 0x00, 0x23, 0x8A, 0xD5,
    0xF9, 0x09, 0xEF, 0x33, 0xDE, 0x40, 0xDC, 0xB0, 0x85, 0xCB, 0x3D, 0x26,
    0xEB, 0x01, 0x1C, 0x00, 0x2A, 0xB8, 0x6A, 0x5E, 0x0B, 0xF6, 0xDF, 0x90,
    0xA0, 0xB1, 0x25, 0x6F, 0xF4, 0x55, 0x4F, 0xE2, 0x36, 0x6F, 0x18, 0x02,
    0x81, 0xD9, 0x22, 0x73, 0xCF, 0xC3, 0x9B, 0xFB, 0x60, 0x0C, 0x9E, 0x74,
    0x91, 0xF7, 0x3F, 0x4C, 0x37, 0x88, 0xEC, 0x02, 0x65, 0xD4, 0xFD, 0x10,
    0x33, 0x66, 0x69, 0x02, 0xFC, 0xDC, 0x18, 0xFD, 0x9E, 0xBA, 0xE2, 0x78,
    0xEA, 0x09, 0x1A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x74, 0x5B, 0xA5, 0x97,
    0x3A, 0x33, 0x09, 0x86, 0x32, 0xB0, 0xCB, 0x88, 0xA4, 0x6D, 0xA2, 0xC1,
    0x85, 0x54, 0xC8, 0x70, 0x26, 0x2B, 0xCF, 0x17, 0x1A, 0x1B, 0xFC, 0x49,
    0x9A, 0x97, 0x9D, 0x56, 0x45, 0x72, 0x8F, 0x5A, 0x21, 0x3E, 0x87, 0x64,
    0x28, 0x32, 0x3F, 0x74, 0xC1, 0x66, 0x2B, 0x33, 0x38, 0x09, 0x85, 0x23,
    0x63, 0xA0, 0xDE, 0xDD, 0x45, 0x26, 0x6D, 0x07, 0x2E, 0x17, 0xD8, 0xC2,
    0xCB, 0x82, 0xDC, 0xC3, 0x35, 0x65, 0xCB, 0xAC, 0x87, 0x66, 0x00, 0x00,
    0x00,
};

} // namespace

int test_laz_encoder() {
    int fails = 0;
    printf("test_laz_encoder:\n");

    // 1. Tom ström — done() direkt efter init(). Fyra svansbyte ska räcka för
    //    att avkodaren ska kunna initieras utan att läsa utanför bufferten.
    {
        ArithmeticEncoder enc;
        enc.init();
        enc.done();
        CHECK(!enc.failed());
        CHECK(enc.size() == 4);
        ArithmeticDecoder dec;
        dec.init(enc.bytes().data(), enc.bytes().size());
    }

    // 2. En enda symbol ur varje modellstorlek, varje symbol.
    for (int mi = 0; mi < kNumModels; ++mi) {
        for (uint32_t sym = 0; sym < kModelSizes[mi]; ++sym) {
            SymbolModel em; em.init(kModelSizes[mi]);
            ArithmeticEncoder enc;
            enc.init();
            enc.encodeSymbol(em, sym);
            enc.done();
            CHECK(!enc.failed());

            SymbolModel dm; dm.init(kModelSizes[mi]);
            ArithmeticDecoder dec;
            dec.init(enc.bytes().data(), enc.bytes().size());
            CHECK(dec.decodeSymbol(dm) == sym);
        }
    }

    // 3. Råbitar: varje bredd 1..32, med randvärdena. Bredd 20 och uppåt tar
    //    uppdelningsgrenen i writeBits/readBits, och 32 är hela intervallet.
    for (uint32_t bits = 1; bits <= 32; ++bits) {
        const uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        const uint32_t vals[] = { 0u, 1u, mask >> 1, mask - 1u, mask };
        for (uint32_t v : vals) {
            ArithmeticEncoder enc;
            enc.init();
            enc.writeBits(bits, v);
            enc.done();
            CHECK(!enc.failed());

            ArithmeticDecoder dec;
            dec.init(enc.bytes().data(), enc.bytes().size());
            const uint32_t got = dec.readBits(static_cast<int>(bits));
            if (got != v) {
                printf("  FAIL: råbitar bredd %u: fick %u, väntade %u\n", bits, got, v);
                ++fails;
            }
        }
    }

    // 4. writeInt/writeShort mot readInt/readShort — de går via två anrop
    //    vardera och ordningen mellan hög och låg halva är lätt att kasta om.
    {
        const uint32_t vals[] = { 0u, 1u, 0xFFFFu, 0x10000u, 0x89ABCDEFu, 0xFFFFFFFFu };
        for (uint32_t v : vals) {
            ArithmeticEncoder enc;
            enc.init();
            enc.writeInt(v);
            enc.writeShort(static_cast<uint16_t>(v & 0xFFFFu));
            enc.done();
            CHECK(!enc.failed());

            ArithmeticDecoder dec;
            dec.init(enc.bytes().data(), enc.bytes().size());
            CHECK(dec.readInt() == v);
            CHECK(dec.readShort() == static_cast<uint16_t>(v & 0xFFFFu));
        }
    }

    // 5. Fuzz i tre lägen: uniform slump, 0xFF-övervikt, och långa serier av
    //    samma värde. Den andra når carry-propageringen. Den tredje ger en sned
    //    fördelning som ställer andra krav på modellerna — men den når INTE
    //    halveringarna; de testas separat i punkt 9.
    uint64_t total_carries = 0, total_carry_ff = 0;
    {
        int fuzz_fails = 0;
        uint64_t ops = 0, rounds = 0;
        for (uint64_t seed = 1; seed <= 400; ++seed) {
            const uint32_t steps = 200 + (seed * 37) % 3000;
            for (int mode = 0; mode < 3; ++mode) {
                RoundStats st = run_round(seed * 6364136223846793005ull + mode + 1,
                                          steps, mode);
                fuzz_fails     += st.fails;
                total_carries  += st.carries;
                total_carry_ff += st.carry_ff;
                ops += steps;
                ++rounds;
            }
        }
        fails += fuzz_fails;
        if (fuzz_fails == 0)
            printf("  fuzz: %llu operationer, %llu körningar, inga avvikelser\n",
                   (unsigned long long)ops, (unsigned long long)rounds);
    }

    // 6. Långkörning: en enda ström med många operationer, så modellerna hinner
    //    genom flera update_distribution-cykler och halveringar.
    {
        RoundStats st = run_round(0xC0FFEEull, 300000, /*mode=*/1);
        fails += st.fails;
        total_carries  += st.carries;
        total_carry_ff += st.carry_ff;
        if (st.fails == 0)
            printf("  långkörning: 300000 operationer i en ström, inga avvikelser\n");
    }

    // 7. Carry-vägen MÅSTE ha körts. Utan det här kravet är ett grönt fuzz-test
    //    förenligt med att den svåraste kodvägen aldrig nåddes — och då bevisar
    //    grönt ingenting om just den. Flerbytesvansen (carry genom minst en
    //    0xFF-byte) räknas separat, för det är den som är svår.
    {
        CHECK(total_carries > 1000);
        CHECK(total_carry_ff > 0);
        printf("  carry: %llu propageringar, varav %llu byte genom 0xFF-svit\n",
               (unsigned long long)total_carries,
               (unsigned long long)total_carry_ff);
    }

    // 8. Båda grenarna i done(). De ger BÅDA exakt fyra svansbyte — det är
    //    konstruktionen, eftersom avkodarens init() alltid läser fyra — men de
    //    kommer dit olika: en renorm-byte plus tre nollor, respektive två plus två.
    //
    //    Grenen avgörs av om `length` är över eller under 2*AC_MIN_LENGTH när
    //    strömmen avslutas, och det går att styra exakt: writeBits skiftar ned
    //    length med sin bredd. Från 0xFFFFFFFF ger bredd 6 length 0x03FFFFFF
    //    (över gränsen) och bredd 7 ger 0x01FFFFFF (under). Ingen renorm hinner
    //    emellan, så grenvalet är bestämt.
    {
        struct Case { uint32_t bits; const char* gren; uint8_t tail[4]; };
        const Case cases[] = {
            {6, "length > 2*AC_MIN_LENGTH",  {0x01, 0x00, 0x00, 0x00}},
            {7, "length <= 2*AC_MIN_LENGTH", {0x00, 0x80, 0x00, 0x00}},
        };
        for (const Case& c : cases) {
            ArithmeticEncoder enc;
            enc.init();
            enc.writeBits(c.bits, 0);
            const size_t before = enc.size();
            enc.done();
            CHECK(!enc.failed());
            const size_t tail = enc.size() - before;
            if (tail != 4) {
                printf("  FAIL: %s gav %zu svansbyte, väntade 4\n", c.gren, tail);
                ++fails;
            }
            // Antalet svansbyte SÄRSKILJER inte grenarna — båda ger fyra. Utan
            // ett krav på innehållet kan testet förbli grönt även om samma gren
            // alltid tas. Med sym == 0 är svansen exakt bestämd:
            //   bredd 6: renorm skriver 0x01, sedan tre nollor
            //   bredd 7: renorm skriver 0x00 och 0x80, sedan två nollor
            // Det är interna avslutningsvektorer för VÅR kodare, inte ett
            // påstående om vad en annan kodare skulle ha skrivit.
            if (tail == 4) {
                const uint8_t* t = enc.bytes().data() + before;
                bool match = (t[0] == c.tail[0] && t[1] == c.tail[1] &&
                              t[2] == c.tail[2] && t[3] == c.tail[3]);
                if (!match) {
                    printf("  FAIL: %s gav svans %02X %02X %02X %02X, "
                           "väntade %02X %02X %02X %02X\n", c.gren,
                           t[0], t[1], t[2], t[3],
                           c.tail[0], c.tail[1], c.tail[2], c.tail[3]);
                    ++fails;
                }
            }
            // Och strömmen måste fortfarande avkodas rätt.
            ArithmeticDecoder dec;
            dec.init(enc.bytes().data(), enc.bytes().size());
            CHECK(dec.readBits(static_cast<int>(c.bits)) == 0u);
        }
        printf("  done(): båda grenarna nådda, svansen exakt som väntat\n");
    }

    // 9. Modellernas halveringar. Fuzzen ovan når dem INTE: varje körning skapar
    //    nya modeller och gör som mest några tusen operationer, medan
    //    DM_MAX_COUNT är 32768 och BM_MAX_COUNT 8192. Halveringen ändrar hela
    //    sannolikhetsfördelningen och måste ske identiskt i kodare och avkodare,
    //    så den ska inte vara otestad.
    //
    //    Testet KRÄVER att grenen körts, inte bara att många operationer gjordes:
    //    summan av räknarna växer med ett per operation och kan bara minska genom
    //    en halvering.
    {
        // Symbolmodell: driv en och samma modell förbi DM_MAX_COUNT.
        constexpr uint32_t kOps = 100000;
        SymbolModel em; em.init(4);
        ArithmeticEncoder enc;
        enc.init();

        int halvings = 0;
        uint32_t prev_total = 0;
        for (uint32_t i = 0; i < 4; ++i) prev_total += em.symbol_count[i];

        Rng rng(0xABCDEFull);
        std::vector<uint32_t> syms;
        syms.reserve(kOps);
        for (uint32_t i = 0; i < kOps; ++i) {
            const uint32_t sym = rng.below(4);
            syms.push_back(sym);
            enc.encodeSymbol(em, sym);
            uint32_t total = 0;
            for (uint32_t k = 0; k < 4; ++k) total += em.symbol_count[k];
            if (total < prev_total) ++halvings;
            prev_total = total;
        }
        enc.done();
        CHECK(!enc.failed());
        CHECK(halvings > 0);

        SymbolModel dm; dm.init(4);
        ArithmeticDecoder dec;
        dec.init(enc.bytes().data(), enc.bytes().size());
        int mismatches = 0;
        for (uint32_t i = 0; i < kOps; ++i)
            if (dec.decodeSymbol(dm) != syms[i]) { ++mismatches; break; }
        CHECK(mismatches == 0);
        printf("  symbolmodell: %u operationer, %d halveringar, rundgång ren\n",
               kOps, halvings);
    }
    {
        // Bitmodell: samma sak mot BM_MAX_COUNT. bit_count kan bara minska
        // genom halveringsgrenen i update_bit_distribution.
        constexpr uint32_t kOps = 50000;
        BitModel em; em.init();
        ArithmeticEncoder enc;
        enc.init();

        int halvings = 0;
        uint16_t prev = em.bit_count;

        Rng rng(0x13579Bull);
        std::vector<uint32_t> bits;
        bits.reserve(kOps);
        for (uint32_t i = 0; i < kOps; ++i) {
            // Sned fördelning: halveringen är intressantast när bit_0_count och
            // bit_count ligger nära varandra, vilket är fallet som kräver
            // korrigeringen bit_count++ i update_bit_distribution.
            const uint32_t b = (rng.below(16) == 0) ? 1u : 0u;
            bits.push_back(b);
            enc.encodeBit(em, b);
            if (em.bit_count < prev) ++halvings;
            prev = em.bit_count;
        }
        enc.done();
        CHECK(!enc.failed());
        CHECK(halvings > 0);

        BitModel dm; dm.init();
        ArithmeticDecoder dec;
        dec.init(enc.bytes().data(), enc.bytes().size());
        int mismatches = 0;
        for (uint32_t i = 0; i < kOps; ++i)
            if (dec.decodeBit(dm) != bits[i]) { ++mismatches; break; }
        CHECK(mismatches == 0);
        printf("  bitmodell: %u operationer, %d halveringar, rundgång ren\n",
               kOps, halvings);
    }

    // 10. Kontraktsbrotten. I debug avbryter assert, så de går bara att prova i
    //     en build utan assert — och där MÅSTE failed_ sättas, annars är
    //     release-skyddet bara en kommentar. Det är just i release en trasig
    //     ström blir en trasig fil hos någon annan.
#ifdef NDEBUG
    {
        struct { const char* vad; bool (*run)(); } cases[] = {
            { "symbol utanför modellen", []{
                SymbolModel m; m.init(4);
                ArithmeticEncoder e; e.init(); e.encodeSymbol(m, 4); return e.failed(); } },
            { "bit större än 1", []{
                BitModel m; m.init();
                ArithmeticEncoder e; e.init(); e.encodeBit(m, 2); return e.failed(); } },
            { "värde ryms inte i bredden", []{
                ArithmeticEncoder e; e.init(); e.writeBits(3, 8); return e.failed(); } },
            { "bit_count 0", []{
                ArithmeticEncoder e; e.init(); e.writeBits(0, 0); return e.failed(); } },
            { "bit_count 33", []{
                ArithmeticEncoder e; e.init(); e.writeBits(33, 0); return e.failed(); } },
            { "dubbel done()", []{
                ArithmeticEncoder e; e.init(); e.done(); e.done(); return e.failed(); } },
            { "kodning efter done()", []{
                BitModel m; m.init();
                ArithmeticEncoder e; e.init(); e.done(); e.encodeBit(m, 0);
                return e.failed(); } },
            { "IC: värde utanför bitbredden", []{
                // Kontraktet är att pred och real båda ligger i [0, 2^bits).
                // 200 gör det (invikningen ger -56 och är helt giltig) — 1000
                // gör det inte: efter ett invikningssteg är korrektorn 744,
                // fortfarande utanför [-128, 127].
                ArithmeticEncoder e; e.init();
                IntegerCompressor ic; ic.init(e, 8, 1);
                ic.compress(0, 1000, 0);
                return e.failed(); } },
            { "IC: kontext utanför num_instances", []{
                ArithmeticEncoder e; e.init();
                IntegerCompressor ic; ic.init(e, 16, 2);
                ic.compress(100, 101, 5);
                return e.failed(); } },
            { "Point14: dubbel start_chunk", []{
                Point14Encoder e;
                e.start_chunk({1,2,3,2});
                e.start_chunk({4,5,6,2});     // chunken är redan igång
                return e.failed(); } },
            { "Point14: encode efter finish_chunk", []{
                Point14Encoder e;
                e.start_chunk({1,2,3,2});
                std::vector<uint8_t> out;
                e.finish_chunk(out);
                e.encode({4,5,6,2});
                return e.failed(); } },
            { "Point14: dubbel finish_chunk", []{
                Point14Encoder e;
                e.start_chunk({1,2,3,2});
                std::vector<uint8_t> out;
                e.finish_chunk(out);
                const size_t n = out.size();
                e.finish_chunk(out);
                return e.failed() && out.size() == n; } },
            { "done() efter fel lämnar strömmen orörd", []{
                ArithmeticEncoder e; e.init();
                e.writeBits(3, 8);                 // avvisas
                const size_t n = e.size();
                e.done();                          // får inte skriva någon svans
                return e.failed() && e.size() == n; } },
        };
        int bad_contracts = 0;
        for (const auto& c : cases) {
            if (!c.run()) {
                printf("  FAIL: \"%s\" satte inte failed()\n", c.vad);
                ++fails;
                ++bad_contracts;
            }
        }
        if (bad_contracts == 0)
            printf("  kontraktsbrott: %zu fall, alla satte failed()\n",
                   sizeof cases / sizeof cases[0]);
    }
#else
    printf("  kontraktsbrott: SKIP — assert aktiv, provas i release-bygget\n");
#endif

    // 11. IntegerCompressor: rundgång genom vår egen kodare/avkodare, över alla
    //     bitbredder som formatet använder och flera kontexter. Randkorrektorerna
    //     ligger först — corr_min är den som tog oss hit (se punkt 12).
    {
        int ic_fails = 0;
        uint64_t vals = 0;
        for (uint32_t bits : { 8u, 16u, 32u }) {
            for (uint32_t ctxs : { 1u, 2u, 8u }) {
                std::vector<int32_t> preds, reals;
                std::vector<uint32_t> ctx;
                build_ic_program(bits, preds, reals);
                ctx.assign(preds.size(), 0);

                const int64_t range = (bits < 32) ? (int64_t(1) << bits) : 0;
                Rng rng(bits * 1000003ull + ctxs);
                for (int i = 0; i < 5000; ++i) {
                    const uint32_t r1 = rng.next(), r2 = rng.next();
                    if (bits < 32) {
                        preds.push_back(static_cast<int32_t>(r1 % static_cast<uint64_t>(range)));
                        reals.push_back(static_cast<int32_t>(r2 % static_cast<uint64_t>(range)));
                    } else {
                        preds.push_back(static_cast<int32_t>(r1));
                        reals.push_back(static_cast<int32_t>(r2));
                    }
                    ctx.push_back(r1 % ctxs);
                }

                ArithmeticEncoder enc;
                enc.init();
                IntegerCompressor ec;
                ec.init(enc, bits, ctxs);
                for (size_t i = 0; i < preds.size(); ++i)
                    ec.compress(preds[i], reals[i], ctx[i]);
                enc.done();
                if (enc.failed()) {
                    printf("  FAIL: IC %u bitar %u kontexter: kodaren felade\n", bits, ctxs);
                    ++ic_fails;
                    continue;
                }

                ArithmeticDecoder dec;
                dec.init(enc.bytes().data(), enc.bytes().size());
                IntegerCompressor dc;
                dc.init(dec, bits, ctxs);
                for (size_t i = 0; i < preds.size(); ++i) {
                    const int32_t got = dc.decompress(preds[i], ctx[i]);
                    if (got != reals[i]) {
                        printf("  FAIL: IC %u bitar %u kontexter, index %zu: "
                               "fick %d, väntade %d (pred %d)\n",
                               bits, ctxs, i, got, reals[i], preds[i]);
                        ++ic_fails;
                        break;
                    }
                }
                vals += preds.size();
            }
        }
        fails += ic_fails;
        if (ic_fails == 0)
            printf("  IntegerCompressor: %llu värden över 8/16/32 bitar, rundgång ren\n",
                   (unsigned long long)vals);
    }

    // 12. Gyllene vektorer. Rundgången ovan skulle vara grön även om kodare och
    //     avkodare delade samma missuppfattning om formatet — och en sådan fanns
    //     faktiskt: readCorrector kortslöt vid k >= bits_ i stället för k == 32,
    //     så en 16-bitars korrektor lika med corr_min tappade fasen i strömmen.
    //     Rundgången mot oss själva hade aldrig hittat det. Byten nedan är
    //     avkodade av referensen, alltså av någon annan.
    {
        struct G { uint32_t bits; const uint8_t* bytes; size_t len; };
        const G golden[] = {
            {  8, kGolden8,  sizeof kGolden8  },
            { 16, kGolden16, sizeof kGolden16 },
            { 32, kGolden32, sizeof kGolden32 },
        };
        int bad_vectors = 0;
        for (const G& g : golden) {
            std::vector<int32_t> preds, reals;
            build_ic_program(g.bits, preds, reals);

            ArithmeticEncoder enc;
            enc.init();
            IntegerCompressor ec;
            ec.init(enc, g.bits, 1);
            for (size_t i = 0; i < preds.size(); ++i)
                ec.compress(preds[i], reals[i], 0);
            enc.done();
            CHECK(!enc.failed());

            if (enc.size() != g.len) {
                printf("  FAIL: gyllene %u bitar: %zu byte, väntade %zu\n",
                       g.bits, enc.size(), g.len);
                ++fails;
                ++bad_vectors;
                continue;
            }
            size_t diff = 0;
            for (size_t i = 0; i < g.len; ++i)
                if (enc.bytes()[i] != g.bytes[i]) { ++diff; }
            if (diff) {
                printf("  FAIL: gyllene %u bitar: %zu av %zu byte avviker\n",
                       g.bits, diff, g.len);
                ++fails;
                ++bad_vectors;
                continue;
            }

            // Och åt andra hållet: avkoda ur den STATISKA vektorn, inte ur det
            // vi nyss kodade. Det isolerar en avkodarregression — det var just
            // avkodaren som hade fel, och hade den och kodaren gått sönder
            // tillsammans hade jämförelsen ovan fortfarande varit grön.
            ArithmeticDecoder dec;
            dec.init(g.bytes, g.len);
            IntegerCompressor dc;
            dc.init(dec, g.bits, 1);
            for (size_t i = 0; i < preds.size(); ++i) {
                const int32_t got = dc.decompress(preds[i], 0);
                if (got != reals[i]) {
                    printf("  FAIL: gyllene %u bitar, avkodning index %zu: "
                           "fick %d, väntade %d\n", g.bits, i, got, reals[i]);
                    ++fails;
                    ++bad_vectors;
                    break;
                }
            }
        }
        if (bad_vectors == 0)
            printf("  gyllene vektorer: 8/16/32 bitar identiska åt båda håll "
                   "(referensverifierade)\n");
    }

    // 13. Återinitiering. En IntegerCompressor får återanvändas efter att
    //     modellerna hunnit lära sig något — Point14 gör det per chunk. Blir
    //     modellerna inte helt nollställda beror andra chunken på den första,
    //     och den som läser filen fristående får fel.
    {
        std::vector<int32_t> preds, reals;
        build_ic_program(16, preds, reals);

        // Kör en gång med 8 kontexter så modellerna hinner drivas.
        ArithmeticEncoder warm;
        warm.init();
        IntegerCompressor ic;
        ic.init(warm, 16, 8);
        Rng rng(0x5EED11);
        for (int i = 0; i < 20000; ++i)
            ic.compress(static_cast<int32_t>(rng.next() % 65536u),
                        static_cast<int32_t>(rng.next() % 65536u),
                        rng.below(8));
        warm.done();

        // Återinitiera SAMMA instans, nu med en annan kontextmängd, och kräv
        // exakt samma byte som en helt ny instans ger.
        ArithmeticEncoder e1;
        e1.init();
        ic.init(e1, 16, 1);
        for (size_t i = 0; i < preds.size(); ++i) ic.compress(preds[i], reals[i], 0);
        e1.done();

        ArithmeticEncoder e2;
        e2.init();
        IntegerCompressor fresh;
        fresh.init(e2, 16, 1);
        for (size_t i = 0; i < preds.size(); ++i) fresh.compress(preds[i], reals[i], 0);
        e2.done();

        CHECK(!e1.failed() && !e2.failed());
        CHECK(e1.size() == e2.size());
        bool same = e1.size() == e2.size();
        for (size_t i = 0; same && i < e1.size(); ++i)
            if (e1.bytes()[i] != e2.bytes()[i]) same = false;
        CHECK(same);
        if (same)
            printf("  återinitiering: använd instans ger samma byte som en ny\n");
    }

    // 14. 32-bitars fall där pred + korrektor spiller över int32 åt båda håll.
    //     Summan ska wrappa modulo 2^32 — det är algoritmen — men den fick inte
    //     göras som signed overflow.
    {
        const int32_t cases[][2] = {
            { INT32_MAX,      INT32_MIN      },   // korrektor +1 över kanten
            { INT32_MIN,      INT32_MAX      },   // och åt andra hållet
            { INT32_MAX,      -1             },
            { INT32_MIN,      0              },
            { 2000000000,     -2000000000    },
            { -2000000000,    2000000000     },
            { 0,              INT32_MIN      },
        };
        ArithmeticEncoder enc;
        enc.init();
        IntegerCompressor ec;
        ec.init(enc, 32, 1);
        for (const auto& c : cases) ec.compress(c[0], c[1], 0);
        enc.done();
        CHECK(!enc.failed());

        ArithmeticDecoder dec;
        dec.init(enc.bytes().data(), enc.bytes().size());
        IntegerCompressor dc;
        dc.init(dec, 32, 1);
        int bad = 0;
        for (const auto& c : cases)
            if (dc.decompress(c[0], 0) != c[1]) ++bad;
        CHECK(bad == 0);
        if (bad == 0)
            printf("  32-bitars spill: %zu randfall över int32-kanten, rundgång ren\n",
                   sizeof cases / sizeof cases[0]);
    }

    // 15. Point14: rundgång genom vår egen avkodare.
    {
        struct Case { const char* namn; int n; int slag; };
        const Case cases[] = {
            { "1 punkt (bara seed)",        1, 0 },
            { "2 punkter",                  2, 0 },
            { "500 identiska",            500, 1 },
            { "5000 linjära",            5000, 2 },
            { "5000 slumpad klass",      5000, 3 },
            { "3000 helt slumpade",      3000, 4 },
        };
        int p14_fails = 0;
        for (const Case& c : cases) {
            std::vector<Point14Fields> pts;
            Rng rng(static_cast<uint64_t>(c.slag) * 7919ull + 11ull);
            for (int i = 0; i < c.n; ++i) {
                switch (c.slag) {
                    case 0: pts.push_back({1000 + i, 2000 + 2*i, 300 + i, 2}); break;
                    case 1: pts.push_back({5, 5, 5, 2}); break;
                    case 2: pts.push_back({i*10, i*7, 100 + i % 50, 2}); break;
                    case 3: pts.push_back({static_cast<int32_t>(i*10 + rng.below(7)),
                                           static_cast<int32_t>(i*7),
                                           static_cast<int32_t>(100 + rng.below(200)),
                                           static_cast<uint8_t>(rng.below(256))}); break;
                    default: pts.push_back({static_cast<int32_t>(rng.next()),
                                            static_cast<int32_t>(rng.next()),
                                            static_cast<int32_t>(rng.next()),
                                            static_cast<uint8_t>(rng.below(256))}); break;
                }
            }

            Point14Encoder enc;
            enc.start_chunk(pts[0]);
            for (size_t i = 1; i < pts.size(); ++i) enc.encode(pts[i]);
            std::vector<uint8_t> chunk;
            enc.finish_chunk(chunk);
            if (enc.failed()) {
                printf("  FAIL: Point14 %s: kodaren felade\n", c.namn);
                ++p14_fails; continue;
            }

            uint8_t seed[30];
            p14_to_item(pts[0], seed);
            const uint8_t* ptr = chunk.data();
            Point14Decoder dec;
            dec.read_chunk_sizes(ptr);
            dec.init_chunk(ptr, seed);

            uint32_t dctx = 0;
            for (size_t i = 1; i < pts.size(); ++i) {
                uint8_t got[30], want[30];
                dec.decompress(dctx, got);
                p14_to_item(pts[i], want);
                if (std::memcmp(got, want, 30) != 0) {
                    printf("  FAIL: Point14 %s: punkt %zu avviker\n", c.namn, i);
                    ++p14_fails;
                    break;
                }
            }
        }
        fails += p14_fails;
        if (p14_fails == 0)
            printf("  Point14: 6 mönster, 13502 punkter, rundgång ren\n");
    }

    // 16. Point14: gyllene vektor, byte-identisk med referensens egen kodare.
    //     Rundgången ovan är blind för en gemensam missuppfattning mellan vår
    //     kodare och vår avkodare — precis den sortens fel som fanns i
    //     IntegerCompressor. Den här jämförelsen är inte det.
    {
        std::vector<Point14Fields> pts;
        build_p14_program(pts);

        Point14Encoder enc;
        enc.start_chunk(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) enc.encode(pts[i]);
        std::vector<uint8_t> chunk;
        enc.finish_chunk(chunk);
        CHECK(!enc.failed());

        bool ok = chunk.size() == sizeof kGoldenP14;
        if (!ok) {
            printf("  FAIL: Point14 gyllene: %zu byte, väntade %zu\n",
                   chunk.size(), sizeof kGoldenP14);
            ++fails;
        } else {
            size_t diff = 0;
            for (size_t i = 0; i < chunk.size(); ++i)
                if (chunk[i] != kGoldenP14[i]) ++diff;
            if (diff) {
                printf("  FAIL: Point14 gyllene: %zu av %zu byte avviker\n",
                       diff, chunk.size());
                ++fails;
                ok = false;
            }
        }

        // Lagerpolicyn ska gå att läsa direkt ur vektorn: sex nollängder.
        if (ok) {
            uint32_t sizes[9];
            std::memcpy(sizes, kGoldenP14, sizeof sizes);
            CHECK(sizes[0] > 0);   // channel_returns_XY, alltid
            CHECK(sizes[1] > 0);   // Z, alltid
            CHECK(sizes[2] > 0);   // classification, varierar i programmet
            for (int i = 3; i < 9; ++i)
                if (sizes[i] != 0) {
                    printf("  FAIL: lager %d skulle vara tomt men är %u byte\n",
                           i, sizes[i]);
                    ++fails;
                }

            // Och avkoda ur de STATISKA byten, inte ur det vi nyss kodade.
            uint8_t seed[30];
            p14_to_item(pts[0], seed);
            const uint8_t* ptr = kGoldenP14;
            Point14Decoder dec;
            dec.read_chunk_sizes(ptr);
            dec.init_chunk(ptr, seed);
            uint32_t dctx = 0;
            for (size_t i = 1; i < pts.size(); ++i) {
                uint8_t got[30], want[30];
                dec.decompress(dctx, got);
                p14_to_item(pts[i], want);
                if (std::memcmp(got, want, 30) != 0) {
                    printf("  FAIL: Point14 gyllene, avkodning punkt %zu\n", i);
                    ++fails;
                    ok = false;
                    break;
                }
            }
            if (ok)
                printf("  Point14 gyllene: %zu byte identiska åt båda håll, "
                       "sex tomma lager (referensverifierade)\n", chunk.size());
        }
    }

    // 17. Point14: samma kodare återanvänd för flera chunkar måste ge exakt
    //     samma byte som en färsk instans per chunk. Läcker tillstånd mellan
    //     chunkar — last_Z, medianerna, modellerna — blir chunk två beroende av
    //     chunk ett, och den som läser filen fristående får fel. Seed-Z skiljer
    //     medvetet mellan chunkarna, eftersom last_Z initieras ur just den.
    {
        std::vector<std::vector<Point14Fields>> chunks;
        Rng rng(0xC0DEC0DE);
        for (int c = 0; c < 4; ++c) {
            std::vector<Point14Fields> pts;
            const int32_t base_z = c * 100000 - 50000;
            for (int i = 0; i < 300; ++i)
                pts.push_back({static_cast<int32_t>(rng.next() % 100000),
                               static_cast<int32_t>(rng.next() % 100000),
                               base_z + static_cast<int32_t>(rng.next() % 500),
                               static_cast<uint8_t>(rng.below(20))});
            chunks.push_back(std::move(pts));
        }

        Point14Encoder reused;
        int mismatch = 0;
        for (const auto& pts : chunks) {
            std::vector<uint8_t> a, b;
            reused.start_chunk(pts[0]);
            for (size_t i = 1; i < pts.size(); ++i) reused.encode(pts[i]);
            reused.finish_chunk(a);
            CHECK(!reused.failed());

            Point14Encoder fresh;
            fresh.start_chunk(pts[0]);
            for (size_t i = 1; i < pts.size(); ++i) fresh.encode(pts[i]);
            fresh.finish_chunk(b);
            CHECK(!fresh.failed());

            if (a.size() != b.size() ||
                std::memcmp(a.data(), b.data(), a.size()) != 0)
                ++mismatch;
        }
        CHECK(mismatch == 0);
        if (mismatch == 0)
            printf("  Point14 chunkar: 4 chunkar återanvänd kodare, "
                   "byte-identiska med färska instanser\n");
    }

    // 18. Lagerlängderna direkt. De är lagerpolicyn, och den ska inte bara gälla
    //     i genomsnitt utan i varje enskild chunk.
    {
        auto layer_sizes = [](const std::vector<Point14Fields>& pts, uint32_t out[9]) {
            Point14Encoder enc;
            enc.start_chunk(pts[0]);
            for (size_t i = 1; i < pts.size(); ++i) enc.encode(pts[i]);
            std::vector<uint8_t> chunk;
            enc.finish_chunk(chunk);
            if (enc.failed() || chunk.size() < 36) { for (int i=0;i<9;++i) out[i]=0xFFFFFFFFu; return; }
            std::memcpy(out, chunk.data(), 36);
        };
        uint32_t sz[9];

        // Bara seed: ingenting kodas, men XY och Z avslutas ändå — en tom
        // aritmetisk ström är fyra byte, och avkodaren läser fyra vid init.
        {
            std::vector<Point14Fields> pts{{10, 20, 30, 2}};
            layer_sizes(pts, sz);
            CHECK(sz[0] == 4);
            CHECK(sz[1] == 4);
            CHECK(sz[2] == 0);
            for (int i = 3; i < 9; ++i) CHECK(sz[i] == 0);
        }
        // Konstant klass över många punkter: klasslagret ska utelämnas helt.
        {
            std::vector<Point14Fields> pts;
            for (int i = 0; i < 1000; ++i) pts.push_back({i*3, i*5, i*2, 2});
            layer_sizes(pts, sz);
            CHECK(sz[0] > 4);
            CHECK(sz[1] > 0);
            CHECK(sz[2] == 0);       // aldrig ändrad -> längd 0
            for (int i = 3; i < 9; ++i) CHECK(sz[i] == 0);
        }
        // En enda klassändring räcker för att lagret ska skrivas.
        {
            std::vector<Point14Fields> pts;
            for (int i = 0; i < 1000; ++i)
                pts.push_back({i*3, i*5, i*2, static_cast<uint8_t>(i == 700 ? 9 : 2)});
            layer_sizes(pts, sz);
            CHECK(sz[2] > 0);
            for (int i = 3; i < 9; ++i) CHECK(sz[i] == 0);
        }
        printf("  Point14 lagerlängder: seed-only 4/4/0, konstant klass ger 0, "
               "en ändring räcker\n");
    }

    // 19. Kontextgränserna. dy_ctx byter vid kx == 20 och z_ctx vid kxy == 18.
    //     Slumpdata träffar dem men bevisar dem inte; den här vektorn sveper
    //     differensmagnituder 2^15..2^24 i båda riktningarna och är, precis som
    //     den förra, byte-identisk med referensens egen kodare.
    {
        std::vector<Point14Fields> pts;
        build_p14_ctx_program(pts);

        Point14Encoder enc;
        enc.start_chunk(pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) enc.encode(pts[i]);
        std::vector<uint8_t> chunk;
        enc.finish_chunk(chunk);
        CHECK(!enc.failed());

        bool ok = chunk.size() == sizeof kGoldenP14Ctx;
        if (!ok) {
            printf("  FAIL: Point14 kontextgränser: %zu byte, väntade %zu\n",
                   chunk.size(), sizeof kGoldenP14Ctx);
            ++fails;
        } else {
            for (size_t i = 0; i < chunk.size(); ++i)
                if (chunk[i] != kGoldenP14Ctx[i]) { ok = false; break; }
            if (!ok) {
                printf("  FAIL: Point14 kontextgränser: byte avviker\n");
                ++fails;
            }
        }
        if (ok) {
            uint8_t seed[30];
            p14_to_item(pts[0], seed);
            const uint8_t* ptr = kGoldenP14Ctx;
            Point14Decoder dec;
            dec.read_chunk_sizes(ptr);
            dec.init_chunk(ptr, seed);
            uint32_t dctx = 0;
            for (size_t i = 1; i < pts.size(); ++i) {
                uint8_t got[30], want[30];
                dec.decompress(dctx, got);
                p14_to_item(pts[i], want);
                if (std::memcmp(got, want, 30) != 0) {
                    printf("  FAIL: Point14 kontextgränser, punkt %zu\n", i);
                    ++fails; ok = false; break;
                }
            }
            if (ok)
                printf("  Point14 kontextgränser: %zu punkter över 2^15..2^24, "
                       "%zu byte identiska (referensverifierade)\n",
                       pts.size(), chunk.size());
        }
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}

#else  // !HLAS_HAVE_LAZ

int test_laz_encoder() {
    printf("test_laz_encoder:\n  SKIP — byggd utan HLAS_ENABLE_LAZ\n");
    return 0;
}

#endif
