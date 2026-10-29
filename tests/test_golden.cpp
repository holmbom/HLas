// Gyllene vektorer: VÅR avkodare mot REFERENSENS kodare.
//
// Alla andra LAZ-test i sviten är rundgång — vi kodar, vi avkodar, vi jämför.
// Den formen kan inte upptäcka ett DELAT missförstånd, alltså när kodare och
// avkodare är eniga om något som ändå är fel. Precis den buggen fanns:
// readCorrector kortslöt på k >= bits_ i stället för k == 32, och vår egen
// rundgång var lycklig hela tiden.
//
// Bytena i golden_ref.h kommer ur referensen, som vi inte har skrivit. Klarar
// vår avkodare dem är enigheten inte längre intern.
//
// Fallen är kantfall som riktiga filer sällan har — extrema GPS-hopp,
// alla fyra scannerkanaler, klass över 31, retur 15 av 15, INT32_MIN i
// koordinaterna. Filen bevisar att vi klarar riktig data; de här bevisar att vi
// klarar det riktig data råkade sakna.

#include "golden_ref.h"

#ifdef HLAS_HAVE_LAZ
#include "laz/point14.h"
#include "laz/point14_encoder.h"
#include "laz/rgb14.h"
#include "laz/rgb14_encoder.h"
#include "laz/chunk_layers.h"
#endif

#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

#ifdef HLAS_HAVE_LAZ
namespace {

using namespace HLas::detail::laz;

struct Case {
    const char*    name;
    const uint8_t* records;
    uint32_t       count;
    uint16_t       record_length;
    uint8_t        format;
    const uint8_t* chunk;
    size_t         chunk_len;
};

// Packar upp en referenschunk med våra avkodare, i samma ordning som
// LazReader::decompress_chunk: alla lagerhuvuden först, sedan alla init_chunk,
// sedan punkterna. Ordningen är inte fri — lagren ligger efter varandra i
// strömmen och läses med en löpande pekare.
bool decode_chunk(const Case& c, std::vector<uint8_t>& out) {
    const bool has_rgb = (c.format == 7);
    const uint16_t p14_size = 30;

    // Gränskontroller trots att indata är konstant och redan verifierad.
    // Avkodarnas read_chunk_sizes och init_chunk tar ingen slutpekare — de litar
    // på anroparen, precis som i LazReader::decompress_chunk. Skulle någon
    // redigera facit för hand ska det bli ett testfel, inte odefinierat beteende.
    const uint64_t hdr = uint64_t(c.record_length) + 4
                       + Point14Decoder::kChunkSizeHeaderBytes
                       + (has_rgb ? RGB14Decoder::kChunkSizeHeaderBytes : 0);
    if (c.chunk_len < hdr) return false;

    // Seed-posten är punkt 0, rå och okodad.
    const uint8_t* seed = c.chunk;
    const uint8_t* ptr  = c.chunk + c.record_length;

    uint32_t n = 0;
    std::memcpy(&n, ptr, 4);
    ptr += 4;
    if (n != c.count) return false;

    Point14Decoder p14;
    p14.read_chunk_sizes(ptr);
    RGB14Decoder rgb;
    if (has_rgb) rgb.read_chunk_sizes(ptr);

    // Lagerlängderna kommer också ur facit och måste rymmas i det som är kvar.
    const uint64_t payload = p14.declared_layer_bytes()
                           + (has_rgb ? rgb.declared_layer_bytes() : 0);
    if (payload != c.chunk_len - hdr) return false;   // exakt, inte bara "ryms"

    const uint32_t ctx = (seed[15] >> 4) & 0x03;

    p14.init_chunk(ptr, seed);
    if (has_rgb) rgb.init_chunk(ptr, seed + p14_size, ctx);

    out.assign(static_cast<size_t>(n) * c.record_length, 0);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* dst = out.data() + static_cast<size_t>(i) * c.record_length;
        if (i == 0) { std::memcpy(dst, seed, c.record_length); continue; }
        uint32_t k = ctx;
        p14.decompress(k, dst);
        if (has_rgb) rgb.decompress(k, dst + p14_size);
    }
    return true;
}

} // namespace
#endif // HLAS_HAVE_LAZ

int test_golden() {
    int fails = 0;
    printf("test_golden:\n");

#ifndef HLAS_HAVE_LAZ
    printf("  SKIP — byggt utan LAZ\n");
    return 0;
#else
    using namespace testdata::golden;

#define CASE(N) Case{ #N, k##N##_Records, k##N##_Count, k##N##_RecordLength, \
                      k##N##_Format, k##N##_Chunk, sizeof(k##N##_Chunk) }
    const Case cases[] = {
        CASE(Synth_gps_jump),
        CASE(Synth_gps_repeat),
        CASE(Synth_channels),
        CASE(Synth_channels_seed),
        CASE(Synth_returns),
        CASE(Synth_highclass),
        CASE(Synth_flags),
        CASE(Synth_attrs),
        CASE(Synth_constant),
        CASE(Synth_extremes),
        CASE(Synth_rgb),
        CASE(Synth_rgb_channels),
        CASE(Synth_rgb_gray),
        CASE(Real),
    };
#undef CASE

    for (const Case& c : cases) {
        std::vector<uint8_t> got;
        if (!decode_chunk(c, got)) {
            printf("  FAIL: %s: chunken kunde inte packas upp\n", c.name);
            ++fails;
            continue;
        }
        const size_t want_bytes = static_cast<size_t>(c.count) * c.record_length;
        if (got.size() != want_bytes) {
            printf("  FAIL: %s: %zu byte ut, väntade %zu\n", c.name, got.size(), want_bytes);
            ++fails;
            continue;
        }
        if (std::memcmp(got.data(), c.records, want_bytes) != 0) {
            // Peka ut FÖRSTA avvikande punkten och byten i den. Ett "de skiljer
            // sig" utan position är oanvändbart när felet ligger i ett av nio
            // lager.
            uint32_t bad_pt = 0; uint16_t bad_off = 0;
            for (uint32_t i = 0; i < c.count && !bad_off; ++i) {
                const uint8_t* g = got.data() + static_cast<size_t>(i) * c.record_length;
                const uint8_t* w = c.records  + static_cast<size_t>(i) * c.record_length;
                for (uint16_t b = 0; b < c.record_length; ++b)
                    if (g[b] != w[b]) { bad_pt = i; bad_off = static_cast<uint16_t>(b + 1); break; }
            }
            const uint16_t off = static_cast<uint16_t>(bad_off - 1);
            printf("  FAIL: %s: punkt %u byte %u — vi 0x%02X, referensen 0x%02X\n",
                   c.name, bad_pt, off,
                   got[static_cast<size_t>(bad_pt) * c.record_length + off],
                   c.records[static_cast<size_t>(bad_pt) * c.record_length + off]);
            ++fails;
            continue;
        }
    }

    printf("  %zu fall ur referensen avkodade korrekt (%d fel)\n",
           sizeof(cases) / sizeof(cases[0]), fails);
    return fails;
#endif
}

// Andra riktningen: VÅR kodare mot referensens byte.
//
// Det här är det hårda provet. Avkodningen ovan visar att vi TOLKAR referensens
// ström rätt; det här visar att vi PRODUCERAR samma ström. Skillnaden är
// avgörande, för byte-identitet kräver att varje lagerbeslut, varje kontextval
// och varje modelluppdatering sker i samma ordning — inte bara att resultatet
// går att packa upp igen.
//
// Fall som ännu inte går att koda utelämnas UTTRYCKLIGEN och räknas, så listan
// inte tyst krymper: GPS-lagret hör till fas 5 och RGB14 till fas 6.
int test_golden_encode() {
    int fails = 0;
    printf("test_golden_encode:\n");

#ifndef HLAS_HAVE_LAZ
    printf("  SKIP — byggt utan LAZ\n");
    return 0;
#else
    using namespace testdata::golden;

#define CASE(N) Case{ #N, k##N##_Records, k##N##_Count, k##N##_RecordLength, \
                      k##N##_Format, k##N##_Chunk, sizeof(k##N##_Chunk) }
    const Case cases[] = {
        CASE(Synth_gps_jump),
        CASE(Synth_gps_repeat),
        CASE(Synth_channels),
        CASE(Synth_channels_seed),
        CASE(Synth_returns),
        CASE(Synth_highclass),
        CASE(Synth_flags),
        CASE(Synth_attrs),
        CASE(Synth_constant),
        CASE(Synth_extremes),
        CASE(Synth_rgb),
        CASE(Synth_rgb_channels),
        CASE(Synth_rgb_gray),
        CASE(Real),
    };
#undef CASE

    int identical = 0;
    for (const Case& c : cases) {
        // Lagerhuvudet och nyttolasten i facit ligger efter seed-posten och
        // punktantalet. Vår finish_chunk skriver just den delen.
        const size_t prefix = static_cast<size_t>(c.record_length) + 4;

        const bool has_rgb = (c.format == 7);

        Point14Encoder enc;
        RGB14Encoder   rgb;
        enc.start_chunk_item(c.records);
        if (has_rgb)
            rgb.start_chunk(c.records + 30, get_scanner_channel(c.records));

        for (uint32_t i = 1; i < c.count; ++i) {
            const uint8_t* p = c.records + static_cast<size_t>(i) * c.record_length;
            enc.encode_item(p);
            if (has_rgb)
                rgb.encode(get_scanner_channel(p), p + 30);
        }

        // append_chunk_layers sköter ordningen mellan items lagerlängder och
        // lagerdata, och är allt-eller-inget: felar något item skrivs
        // ingenting.
        std::vector<uint8_t> out;
        if (!append_chunk_layers(out, enc, has_rgb ? &rgb : nullptr)) {
            printf("  FAIL: %s: kodaren felade\n", c.name);
            CHECK(out.empty());
            ++fails;
            continue;
        }
        static const char* names[9] = {"chan/ret/XY","Z","classification","flags",
                                       "intensity","scan_angle","user_data",
                                       "point_source","gps_time"};
        if (out.size() != c.chunk_len - prefix) {
            printf("  FAIL: %s: %zu byte ut, facit har %zu\n",
                   c.name, out.size(), c.chunk_len - prefix);
            // Vilket LAGER som skiljer säger vad som är fel; totalen säger inget.
            if (out.size() >= 36) {
                for (int i = 0; i < 9; ++i) {
                    uint32_t a = 0, b = 0;
                    std::memcpy(&a, out.data() + i * 4, 4);
                    std::memcpy(&b, c.chunk + prefix + i * 4, 4);
                    if (a != b)
                        printf("      %-16s vi %u, referensen %u\n", names[i], a, b);
                }
            }
            ++fails;
            continue;
        }
        if (std::memcmp(out.data(), c.chunk + prefix, out.size()) != 0) {
            // Peka ut vilket LAGER som skiljer, inte bara vilken byte. De nio
            // längderna ligger först, så en avvikelse där betyder fel
            // lagerpolicy medan en avvikelse senare betyder fel kodning.
            size_t d = 0;
            while (d < out.size() && out[d] == c.chunk[prefix + d]) ++d;
            if (d < 36) {
                printf("  FAIL: %s: LAGERLÄNGD för %s skiljer (byte %zu)\n",
                       c.name, names[d / 4], d);
            } else {
                size_t off = d - 36, layer = 0, acc = 0;
                for (; layer < 9; ++layer) {
                    uint32_t sz = 0;
                    std::memcpy(&sz, c.chunk + prefix + layer * 4, 4);
                    if (off < acc + sz) break;
                    acc += sz;
                }
                printf("  FAIL: %s: lagret %s skiljer vid byte %zu i lagret "
                       "(vi 0x%02X, referensen 0x%02X)\n",
                       c.name, layer < 9 ? names[layer] : "?", off - acc,
                       out[d], c.chunk[prefix + d]);
            }
            ++fails;
            continue;
        }
        ++identical;
    }

    printf("  %d av %zu fall BYTE-IDENTISKA med referensen\n",
           identical, sizeof(cases) / sizeof(cases[0]));
    printf("  varav %s ur den riktiga tredjepartsfilen\n",
           "Real (32 punkter, punktformat 7)");
    return fails;
#endif
}

// Kodarens tillstånd och lagerpolicy i gränsfall.
//
// Fas 4-versionen av det här testet krävde att kodaren VÄGRADE när GPS-tiden
// ändrades, eftersom lagret inte fanns än. Fas 5 gav den lagret, så kravet är
// borta — och det som står kvar är det som fortfarande gäller: att en chunk med
// bara seed-punkten ger rätt lagerlängder, att en felad kodare inte producerar
// något, och att kodaren går att återanvända.
int test_encoder_refusal() {
    int fails = 0;
    printf("test_encoder_state:\n");

#ifndef HLAS_HAVE_LAZ
    printf("  SKIP — byggt utan LAZ\n");
    return 0;
#else
    using namespace HLas::detail::laz;

    auto make = [](int32_t x, double gps, uint8_t rn = 1, uint8_t nr = 1) {
        std::vector<uint8_t> r(kPoint14Size, 0);
        set_X(r.data(), x);
        set_Y(r.data(), x * 2);
        set_Z(r.data(), x * 3);
        r[OFF_RETURN_INFO] = static_cast<uint8_t>((rn & 0x0F) | (nr << 4));
        r[OFF_CLASSIFICATION] = 2;
        set_gps_time(r.data(), gps);
        return r;
    };

    // 1. GPS-tiden får nu ändras — det var fas 5:s hela innehåll.
    {
        Point14Encoder enc;
        enc.start_chunk_item(make(100, 1000.0).data());
        enc.encode_item(make(200, 1000.0).data());
        enc.encode_item(make(300, 2000.0).data());       // ändrad tid
        enc.encode_item(make(400, 2000.0 + 1e-6).data());
        CHECK(!enc.failed());
        std::vector<uint8_t> out;
        enc.finish_chunk(out);
        CHECK(!enc.failed());
        CHECK(out.size() >= Point14Encoder::kChunkSizeHeaderBytes);
        if (out.size() >= Point14Encoder::kChunkSizeHeaderBytes) {
            uint32_t gps_len = 0;
            std::memcpy(&gps_len, out.data() + 8 * 4, 4);
            CHECK(gps_len > 0);                          // lagret skrevs
        }
    }

    // 2. En chunk med bara seed-punkten. Alla lager utom de två som ALLTID
    //    skrivs ska ha längd 0, och de två ska ha kodarens fyra svansbyte.
    {
        Point14Encoder enc;
        enc.start_chunk_item(make(7, 1.0).data());
        std::vector<uint8_t> out;
        enc.finish_chunk(out);
        CHECK(!enc.failed());
        CHECK(out.size() >= Point14Encoder::kChunkSizeHeaderBytes);
        if (out.size() >= Point14Encoder::kChunkSizeHeaderBytes) {
            uint32_t sizes[9];
            for (int i = 0; i < 9; ++i) std::memcpy(&sizes[i], out.data() + i * 4, 4);
            CHECK(sizes[0] == 4 && sizes[1] == 4);   // XY och Z: bara svansen
            for (int i = 2; i < 9; ++i) CHECK(sizes[i] == 0);
            CHECK(out.size() == 36u + sizes[0] + sizes[1]);
        }
    }

    // 3. En framkallad felväg: GPS-tid som byter tecken på NOLL.
    //
    // gps_time_change jämförs numeriskt, och +0.0 == -0.0, så kodaren ser ingen
    // ändring och hoppar över GPS-lagret. Posten hade då kommit ut med fel
    // tecken — vilket memcmp-kontrollen fångar. Det är den enda felvägen som går
    // att nå med giltig indata, och därför den enda som kan pröva att
    // finish_chunk lämnar anroparens buffert ORÖRD.
    {
        Point14Encoder enc;
        enc.start_chunk_item(make(1, +0.0).data());
        enc.encode_item(make(2, -0.0).data());
        CHECK(enc.failed());

        std::vector<uint8_t> out(7, 0xAB);
        enc.finish_chunk(out);
        if (out.size() == 7) {
            for (uint8_t v : out) CHECK(v == 0xAB);
        } else {
            printf("  FAIL: finish_chunk skrev %zu byte trots fel\n", out.size());
            ++fails;
        }

        // En NY chunk är en ny start, och då ska den fungera igen.
        enc.start_chunk_item(make(1, 5.0).data());
        CHECK(!enc.failed());
        enc.encode_item(make(2, 6.0).data());
        CHECK(!enc.failed());
        out.clear();
        enc.finish_chunk(out);
        CHECK(!enc.failed());
        CHECK(out.size() > Point14Encoder::kChunkSizeHeaderBytes);
    }

    // 4. Rundgång över en GPS-tung ström som är längre än de gyllene
    //    vektorerna: fyra sekvenser som växlar, plus upprepade tider.
    {
        std::vector<std::vector<uint8_t>> pts;
        double t[4] = { 1000.0, 5.0e8, -3.0e7, 2.5 };
        for (int i = 0; i < 400; ++i) {
            const int seq = (i / 3) % 4;
            t[seq] += (i % 5 == 0) ? 0.0 : 1e-6 * ((i % 7) + 1);
            pts.push_back(make(i * 13, t[seq], 1, 1));
        }

        Point14Encoder enc;
        enc.start_chunk_item(pts[0].data());
        for (size_t i = 1; i < pts.size(); ++i) enc.encode_item(pts[i].data());
        std::vector<uint8_t> chunk;
        enc.finish_chunk(chunk);
        CHECK(!enc.failed());

        const uint8_t* ptr = chunk.data();
        Point14Decoder dec;
        dec.read_chunk_sizes(ptr);
        dec.init_chunk(ptr, pts[0].data());
        uint32_t ctx = 0, diffs = 0;
        uint8_t got[kPoint14Size];
        for (size_t i = 1; i < pts.size(); ++i) {
            dec.decompress(ctx, got);
            if (std::memcmp(got, pts[i].data(), kPoint14Size) != 0) ++diffs;
        }
        CHECK(diffs == 0);
        printf("  400 punkter över fyra GPS-sekvenser: %u avvikelser i rundgång\n", diffs);
    }

    printf("  GPS skrivs vid ändring, seed-only ger 4/4, minus noll avvisas utan\n"
           "    att bufferten rörs\n");
    return fails;
#endif
}
