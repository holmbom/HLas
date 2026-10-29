// HELA autzen.laz genom VÅR kodare, jämförd mot filens EGNA byte.
//
// Omkodningsprovet för de gamla formaten. Filen är
// punktformat 3 med 10,6 miljoner punkter, skriven av ett tredjepartsverktyg —
// alltså varken av oss eller av referensen.
//
// Provet är hårdare än det låter. Byte-identitet kräver inte bara att varje
// värde kodas rätt, utan att VARJE kontextval och VARJE modelluppdatering sker i
// samma ordning som i den kodare som skrev filen. Och till skillnad från
// COPC-filen ligger chunkgränserna redan rätt: autzen.laz använder samma
// chunkstorlek som vi, 50000 punkter, så hela filen går att jämföra chunk för
// chunk utan att gränserna först måste förhandlas.
//
// Testet parsar chunk-tabellen själv. Den är komprimerad med samma aritmetiska
// kodare som punkterna, och positionerna är inte något det publika API:t lämnar
// ut — med rätta, eftersom ingen anropare har användning för dem.

#include "HLas.h"
#include "test_data.h"

#ifdef HLAS_HAVE_LAZ
#include "laz/arithmetic.h"
#include "laz/arithmetic_encoder.h"
#include "laz/integer_compressor.h"
#include "laz/point10.h"
#include "laz/point10_encoder.h"
#include "laz/point10_item.h"
#include "laz/gpstime11.h"
#include "laz/gpstime11_encoder.h"
#include "laz/rgb12.h"
#include "laz/rgb12_encoder.h"
#include "../src/write/point_encode.h"
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

#ifdef HLAS_HAVE_LAZ
namespace {

using namespace HLas::detail::laz;

// Läser ett fält ur bufferten med gränskontroll. Filen kommer utifrån, och en
// trunkerad eller elak fil ska ge ett testfel och inte odefinierat beteende.
template <typename T>
bool get_at(const std::vector<uint8_t>& b, uint64_t off, T& out) {
    if (off > b.size() || b.size() - off < sizeof(T)) return false;
    std::memcpy(&out, b.data() + off, sizeof(T));
    return true;
}

struct Chunk { uint64_t offset; uint64_t byte_size; uint32_t points; };

struct LegacyFile {
    std::vector<uint8_t> bytes;
    uint8_t   format = 0;
    uint16_t  record_length = 0;
    uint64_t  point_count = 0;
    bool      has_gps = false, has_rgb = false;
    std::vector<Chunk> chunks;
};

// Varför load() har TRE utfall och inte två.
//
// "Filen finns inte" och "filen är inte den sortens fil testet handlar om" ska
// hoppa över testet. Allt ANNAT — trunkerad header, trasig VLR, en chunk-tabell
// som inte går ihop — är ett FEL och ska fälla sviten. Med bara SKIP och Ok blir
// varje parserfel tyst grönt, och en trasig autzen.laz hade gjort hela testet
// meningslöst utan att någon märkte det.
enum class LoadResult { Ok, Skip, Error };

// Parsar header, LAZ-VLR och chunk-tabell.
//
// Varje intervall kontrolleras med SUBTRAKTION och aldrig med addition — en
// summa som wrappar hade annars sett ut att rymmas. Filen kommer utifrån, och
// måtten i den styr både allokeringar och hur många byte avkodarna skriver.
LoadResult parse(LegacyFile& f, const char*& why) {
    const uint64_t size = f.bytes.size();
    if (size < 227 || std::memcmp(f.bytes.data(), "LASF", 4) != 0) {
        why = "inte en LAS-fil"; return LoadResult::Skip;
    }
    uint16_t hdr_size = 0, rec_len = 0;
    uint32_t offset_to_points = 0, nvlr = 0, legacy_count = 0;
    if (!get_at(f.bytes, 94, hdr_size) || !get_at(f.bytes, 96, offset_to_points) ||
        !get_at(f.bytes, 100, nvlr) || !get_at(f.bytes, 105, rec_len) ||
        !get_at(f.bytes, 107, legacy_count)) {
        why = "trunkerad header"; return LoadResult::Error;
    }
    const uint8_t raw_format = f.bytes[104];
    if ((raw_format & 0x80) == 0) { why = "inte komprimerad"; return LoadResult::Skip; }
    f.format = static_cast<uint8_t>(raw_format & 0x3F);
    if (f.format > 3) { why = "inte punktformat 0-3"; return LoadResult::Skip; }

    // Postlängden MÅSTE vara formatets. Utan den kopplingen kan en fil påstå
    // postlängd 1 och ändå deklarera ett Point10-item om 20 byte — då allokeras
    // en byte per punkt och avkodaren skriver tjugo. Det är en heapöverskrivning
    // styrd av filens innehåll, alltså precis vad de här kontrollerna finns för.
    const uint16_t want_len = HLas::detail::las_record_len(f.format);
    if (rec_len != want_len) { why = "postlängden matchar inte punktformatet"; return LoadResult::Error; }
    f.record_length = rec_len;
    f.point_count = legacy_count;
    if (f.point_count == 0) { why = "noll punkter"; return LoadResult::Error; }
    if (hdr_size > size || offset_to_points > size || offset_to_points < hdr_size) {
        why = "headern pekar utanför filen"; return LoadResult::Error;
    }

    // LAZ-VLR:n
    uint64_t p = hdr_size;
    bool found = false;
    for (uint32_t i = 0; i < nvlr && !found; ++i) {
        // Hela 54-bytes VLR-huvudet måste finnas INNAN något fält i det läses,
        // och det måste ligga före punktdatan.
        if (p > offset_to_points || offset_to_points - p < 54) {
            why = "VLR-huvudet får inte plats"; return LoadResult::Error;
        }
        uint16_t rid = 0, rlen = 0;
        if (!get_at(f.bytes, p + 18, rid) || !get_at(f.bytes, p + 20, rlen)) {
            why = "trunkerad VLR"; return LoadResult::Error;
        }
        const uint64_t payload = p + 54;
        if (payload > offset_to_points || offset_to_points - payload < rlen) {
            why = "VLR-innehållet får inte plats"; return LoadResult::Error;
        }
        if (rid != 22204) { p = payload + rlen; continue; }

        if (std::memcmp(f.bytes.data() + p + 2, "laszip encoded", 14) != 0) {
            why = "fel user_id på LAZ-VLR:n"; return LoadResult::Error;
        }
        uint16_t comp = 0, coder = 1, nitems = 0;
        uint32_t chunk_size = 0;
        if (rlen < 34 || !get_at(f.bytes, payload, comp) ||
            !get_at(f.bytes, payload + 2, coder) ||
            !get_at(f.bytes, payload + 12, chunk_size) ||
            !get_at(f.bytes, payload + 32, nitems)) {
            why = "trasig LAZ-VLR"; return LoadResult::Error;
        }
        if (comp != 2) { why = "inte compressor 2"; return LoadResult::Skip; }
        if (coder != 0) { why = "okänd coder"; return LoadResult::Error; }
        if (rlen - 34u < 6u * nitems) { why = "itemlistan får inte plats"; return LoadResult::Error; }

        // Itemlistan måste vara EXAKT den formatet föreskriver, i rätt ordning
        // och med rätt storlek och version. Items delar en ström, så ett item på
        // fel plats flyttar allt som kommer efter.
        struct Want { uint16_t type, size; };
        Want want[3]; int nwant = 0;
        want[nwant++] = Want{ 6, 20 };
        if (f.format == 1 || f.format == 3) want[nwant++] = Want{ 7, 8 };
        if (f.format == 2 || f.format == 3) want[nwant++] = Want{ 8, 6 };
        if (nitems != nwant) { why = "fel antal items för punktformatet"; return LoadResult::Error; }
        for (int k = 0; k < nwant; ++k) {
            uint16_t type = 0, isize = 0, iver = 0;
            if (!get_at(f.bytes, payload + 34 + 6 * k, type) ||
                !get_at(f.bytes, payload + 36 + 6 * k, isize) ||
                !get_at(f.bytes, payload + 38 + 6 * k, iver)) {
                why = "trasigt item"; return LoadResult::Error;
            }
            if (type != want[k].type || isize != want[k].size || iver != 2) {
                why = "itemlistan matchar inte punktformatet"; return LoadResult::Error;
            }
        }
        f.has_gps = (f.format == 1 || f.format == 3);
        f.has_rgb = (f.format == 2 || f.format == 3);

        if (chunk_size == 0 || chunk_size == 0xFFFFFFFFu) {
            why = "adaptiv chunkning stöds inte här"; return LoadResult::Skip;
        }

        // Chunk-tabellens position ligger i de första åtta byten av punktdatan.
        int64_t table_pos = 0;
        if (!get_at(f.bytes, offset_to_points, table_pos)) {
            why = "ingen chunk-tabellposition"; return LoadResult::Error;
        }
        if (table_pos < 0 || static_cast<uint64_t>(table_pos) > size ||
            size - static_cast<uint64_t>(table_pos) < 8) {
            why = "chunk-tabellen utanför filen"; return LoadResult::Error;
        }
        const uint64_t tpos = static_cast<uint64_t>(table_pos);
        uint32_t version = 0, num_chunks = 0;
        if (!get_at(f.bytes, tpos, version) || !get_at(f.bytes, tpos + 4, num_chunks)) {
            why = "trunkerad chunk-tabell"; return LoadResult::Error;
        }
        if (version != 0) { why = "okänd chunk-tabellversion"; return LoadResult::Error; }

        // Antalet chunkar är GIVET av punktantalet och chunkstorleken. Att bara
        // läsa så många som behövs skulle låta en tabell med extra poster passera
        // utan att de jämförts; att lita på num_chunks utan tak skulle låta en
        // fil begära miljarder iterationer.
        const uint64_t expect = (f.point_count + chunk_size - 1) / chunk_size;
        if (num_chunks != expect) {
            why = "antalet chunkar stämmer inte med punktantalet"; return LoadResult::Error;
        }

        ArithmeticDecoder dec;
        dec.init(f.bytes.data() + tpos + 8, static_cast<size_t>(size - tpos - 8));
        IntegerCompressor ic;
        ic.init(dec, 32, 2);

        const uint64_t data_start = static_cast<uint64_t>(offset_to_points) + 8;
        if (data_start > tpos) { why = "punktdatan börjar efter chunk-tabellen"; return LoadResult::Error; }
        uint64_t running = data_start;
        uint64_t remaining = f.point_count;
        int32_t prev_size = 0;
        f.chunks.reserve(static_cast<size_t>(num_chunks));
        for (uint32_t k = 0; k < num_chunks; ++k) {
            const int32_t sz = ic.decompress(prev_size, 1);
            prev_size = sz;
            const uint64_t bs = static_cast<uint64_t>(static_cast<uint32_t>(sz));
            if (running > tpos || tpos - running < bs) {
                why = "chunk utanför punktdatan"; return LoadResult::Error;
            }
            const uint32_t pts = (remaining < chunk_size)
                               ? static_cast<uint32_t>(remaining) : chunk_size;
            if (pts == 0) { why = "chunk utan punkter"; return LoadResult::Error; }
            f.chunks.push_back(Chunk{ running, bs, pts });
            running += bs;
            remaining -= pts;
        }
        // Chunkarna ska täcka ALLA punkter och ALL punktdata, exakt.
        if (remaining != 0) { why = "chunkarna täcker inte alla punkter"; return LoadResult::Error; }
        if (running != tpos) { why = "chunkarna täcker inte all punktdata"; return LoadResult::Error; }
        found = true;
    }
    if (!found) { why = "ingen LAZ-VLR"; return LoadResult::Skip; }
    return LoadResult::Ok;
}

LoadResult load(const char* path, LegacyFile& f, const char*& why) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) { why = "filen finns inte"; return LoadResult::Skip; }
    std::fseek(fp, 0, SEEK_END);
    const long long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (len <= 0) { std::fclose(fp); why = "tom fil"; return LoadResult::Error; }
    f.bytes.resize(static_cast<size_t>(len));
    const size_t got = std::fread(f.bytes.data(), 1, f.bytes.size(), fp);
    std::fclose(fp);
    if (got != f.bytes.size()) { why = "kort läsning"; return LoadResult::Error; }
    return parse(f, why);
}

// Att parsningen AVVISAR trasig indata är en egenskap testet självt vilar på:
// blir ett parserfel i stället en SKIP är hela provet tyst grönt på fel
// underlag. Därför prövas den, i minnet och utan att röra disken.
//
// Muteringarna är valda så att var och en träffar en egen kontroll: ett fält som
// styr en ALLOKERING, ett som styr hur många byte en avkodare skriver, ett som
// styr hur långt en läsning går, och ett som avgör om chunkarna går ihop.
int check_parser_rejects(const std::vector<uint8_t>& good) {
    int fails = 0;
    // `count` är antalet byte som sätts, inte ett värde: punktantalet är ett
    // uint32, och att nollställa bara dess lägsta byte ändrar det till 10653184
    // i stället för till noll. Den fällan gick jag i när kontrollen skrevs, och
    // det var kontrollen själv som visade det.
    struct Mut { const char* what; uint64_t off; uint8_t value; int count; };
    const Mut muts[] = {
        { "postlängd som inte hör till formatet", 105, 0x11, 1 },
        { "punktformat utan LAZ-bit",             104, 0x03, 1 },
        { "punktantal noll",                      107, 0x00, 4 },
        { "trasig offset till punktdata",          96, 0xFF, 4 },
    };
    for (const Mut& m : muts) {
        LegacyFile bad;
        bad.bytes = good;
        if (m.off > bad.bytes.size() ||
            bad.bytes.size() - m.off < static_cast<uint64_t>(m.count)) continue;
        for (int k = 0; k < m.count; ++k)
            bad.bytes[static_cast<size_t>(m.off) + k] = m.value;
        const char* why = "";
        const LoadResult lr = parse(bad, why);
        // Ett par av muteringarna är legitima "inte vår sorts fil" och ger Skip;
        // ingen av dem får ge Ok, för då har en trasig fil tagit sig igenom.
        if (lr == LoadResult::Ok) {
            printf("  FAIL: parsningen accepterade %s\n", m.what);
            ++fails;
        }
    }
    // En trunkerad fil ska falla, inte tolkas.
    {
        LegacyFile bad;
        bad.bytes.assign(good.begin(), good.begin() + static_cast<long>(good.size() / 2));
        const char* why = "";
        if (parse(bad, why) == LoadResult::Ok) {
            printf("  FAIL: parsningen accepterade en halv fil\n");
            ++fails;
        }
    }
    return fails;
}

} // namespace
#endif // HLAS_HAVE_LAZ

int test_reencode_legacy() {
    int fails = 0;
    printf("test_reencode_legacy:\n");
#ifndef HLAS_HAVE_LAZ
    printf("  SKIP — byggt utan LAZ\n");
    return 0;
#else
    SKIP_IF_MISSING(testdata::kLaz);

    LegacyFile f;
    const char* why = "";
    const LoadResult lr = load(testdata::kLaz, f, why);
    if (lr == LoadResult::Skip) {
        printf("  SKIP — %s\n", why);
        return fails;
    }
    if (lr != LoadResult::Ok) {
        printf("  FAIL: kunde inte läsa %s — %s\n", testdata::kLaz, why);
        return fails + 1;
    }

    // Provet är bara värt något på en STOR fil skriven av någon annan. Skulle
    // testdatan bytas mot en liten fil ska det synas, inte tyst bli grönt på ett
    // par chunkar.
    if (f.chunks.size() < 10) {
        printf("  SKIP — bara %zu chunkar, för litet underlag\n", f.chunks.size());
        return fails;
    }

    fails += check_parser_rejects(f.bytes);

    std::vector<uint8_t> decoded, encoded;
    uint64_t identical = 0, total_points = 0;
    uint64_t first_bad = 0;
    bool have_bad = false;

    for (size_t ci = 0; ci < f.chunks.size(); ++ci) {
        const Chunk& ch = f.chunks[ci];
        const uint8_t* src = f.bytes.data() + ch.offset;

        if (ch.byte_size < f.record_length) { CHECK(false); break; }

        // --- Packa upp med VÅRA avkodare ---
        decoded.assign(static_cast<size_t>(ch.points) * f.record_length, 0);
        {
            ArithmeticDecoder dec;
            dec.init(src + f.record_length,
                     static_cast<size_t>(ch.byte_size) - f.record_length);
            Point10Decoder p10;   p10.init(dec);
            GPSTime11Decoder gps; if (f.has_gps) gps.init(dec);
            RGB12Decoder rgb;     if (f.has_rgb) rgb.init(dec);

            for (uint32_t i = 0; i < ch.points; ++i) {
                uint8_t* dst = decoded.data() + static_cast<size_t>(i) * f.record_length;
                const bool s = (i == 0);
                uint16_t off = kPoint10Size;
                p10.decompress(s ? src : nullptr, dst);
                if (f.has_gps) { gps.decompress(s ? src + off : nullptr, dst + off); off += 8; }
                if (f.has_rgb) rgb.decompress(s ? src + off : nullptr, dst + off);
            }
        }

        // --- Koda om med VÅRA kodare ---
        encoded.assign(decoded.begin(), decoded.begin() + f.record_length);
        {
            ArithmeticEncoder enc;
            enc.init();
            Point10Encoder p10;   p10.init(enc); p10.start_chunk(decoded.data());
            GPSTime11Encoder gps;
            RGB12Encoder rgb;
            uint16_t off = kPoint10Size;
            if (f.has_gps) { gps.init(enc); gps.start_chunk(decoded.data() + off); off += 8; }
            if (f.has_rgb) { rgb.init(enc); rgb.start_chunk(decoded.data() + off); }

            for (uint32_t i = 1; i < ch.points; ++i) {
                const uint8_t* rec = decoded.data() + static_cast<size_t>(i) * f.record_length;
                uint16_t o = kPoint10Size;
                p10.encode(rec);
                if (f.has_gps) { gps.encode(rec + o); o += 8; }
                if (f.has_rgb) rgb.encode(rec + o);
            }
            enc.done();
            CHECK(!p10.failed() && !enc.failed());
            encoded.insert(encoded.end(), enc.bytes().begin(), enc.bytes().end());
        }

        // Hela chunken jämförs — seed-posten och strömmen.
        if (encoded.size() == ch.byte_size &&
            std::memcmp(encoded.data(), src, encoded.size()) == 0) {
            ++identical;
        } else if (!have_bad) {
            have_bad = true;
            first_bad = ci;
        }
        total_points += ch.points;
    }

    CHECK(identical == f.chunks.size());
    if (have_bad) {
        const Chunk& ch = f.chunks[first_bad];
        printf("  första avvikande chunk: %zu (%u punkter, %llu byte i filen)\n",
               static_cast<size_t>(first_bad), ch.points,
               (unsigned long long)ch.byte_size);
    }
    printf("  %llu av %zu HELA chunkar byte-identiska med filen (%llu punkter, format %u)\n",
           (unsigned long long)identical, f.chunks.size(),
           (unsigned long long)total_points, f.format);
    return fails;
#endif
}
