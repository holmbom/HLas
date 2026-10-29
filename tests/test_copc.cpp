// COPC-indexet: copc info-VLR och hierarkisidor.
//
// Testet läser hierarkin direkt ur den mmap:ade filen på den offset info-VLR:n
// pekar ut — alltså exakt samma åtkomstmönster som HTTP-stigen i net/ kommer
// att använda, bara med en annan källa till byten.

#include "copc_index.h"
#include "byte_source.h"
#include "file_byte_source.h"
#include "file_io.h"
#ifdef HLAS_HAVE_LAZ
#include "copc_node.h"
#include "copc_query.h"
#include "laz_reader.h"
#include "HLas.h"
#include <chrono>
#endif
// Utanför ifdef:en — undersidetestet använder std::sort oavsett LAZ, och utan
// avkodaren kom <algorithm> inte längre in transitivt via laz-headerna.
#include <algorithm>
// Utanför ifdef:en — rotlådetestet prövar urvalet utan att packa upp något.
#include "copc_query.h"
#include "header_parser.h"
#include "vlr_parser.h"
#include "test_data.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <cstdio>

using namespace HLas::detail;

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

int test_copc() {
    int fails = 0;
    printf("test_copc:\n");

    // 1. Trasiga indata — kräver ingen testdata
    {
        std::vector<CopcEntry> e;
        CHECK(parse_copc_hierarchy(nullptr, 32, e) == ErrorCode::CorruptEvlr);
        uint8_t buf[64] = {};
        CHECK(parse_copc_hierarchy(buf, 0, e) == ErrorCode::CorruptEvlr);
        // Inte en jämn multipel av 32 byte
        CHECK(parse_copc_hierarchy(buf, 40, e) == ErrorCode::CorruptEvlr);
        CHECK(e.empty());

        // Bygg en post och peta på ett fält i taget
        auto entry = [](int32_t lvl, int32_t x, int32_t y, int32_t z,
                        uint64_t off, int32_t bytes, int32_t pts) {
            std::vector<uint8_t> b(32, 0);
            std::memcpy(b.data() +  0, &lvl,   4);
            std::memcpy(b.data() +  4, &x,     4);
            std::memcpy(b.data() +  8, &y,     4);
            std::memcpy(b.data() + 12, &z,     4);
            std::memcpy(b.data() + 16, &off,   8);
            std::memcpy(b.data() + 24, &bytes, 4);
            std::memcpy(b.data() + 28, &pts,   4);
            return b;
        };

        // level 3 tillåter koordinater 0..7 — 8 ligger utanför rutnätet
        auto b1 = entry(3, 8, 0, 0, 0, 64, 10);
        CHECK(parse_copc_hierarchy(b1.data(), 32, e) == ErrorCode::CorruptEvlr);

        // Negativ koordinat på hög nivå — skiftkontrollen hoppas över där, men
        // negativt ska nekas ändå
        auto b2 = entry(31, -1, 0, 0, 0, 64, 10);
        CHECK(parse_copc_hierarchy(b2.data(), 32, e) == ErrorCode::CorruptEvlr);
        auto b3 = entry(40, 0, -5, 0, 0, 64, 10);
        CHECK(parse_copc_hierarchy(b3.data(), 32, e) == ErrorCode::CorruptEvlr);

        // Nivå bortom taket ska nekas i parsningen — annars får noden
        // odefinierade gränser och faller tyst ur varje urval
        auto b_lvl = entry(kCopcMaxLevel + 1, 0, 0, 0, 0, 64, 10);
        CHECK(parse_copc_hierarchy(b_lvl.data(), 32, e) == ErrorCode::CorruptEvlr);
        auto b_lvl2 = entry(63, 0, 0, 0, 0, 64, 10);
        CHECK(parse_copc_hierarchy(b_lvl2.data(), 32, e) == ErrorCode::CorruptEvlr);
        // Precis på taket ska däremot gå igenom
        auto b_ok = entry(kCopcMaxLevel, 0, 0, 0, 0, 64, 10);
        CHECK(parse_copc_hierarchy(b_ok.data(), 32, e) == ErrorCode::Ok);
        e.clear();

        // point_count under -1 är ogiltigt; bara exakt -1 betyder undersida
        auto b4 = entry(0, 0, 0, 0, 0, 64, -2);
        CHECK(parse_copc_hierarchy(b4.data(), 32, e) == ErrorCode::CorruptEvlr);

        // Undersida med storlek som inte är en hel rad poster
        auto b5 = entry(0, 0, 0, 0, 0, 40, -1);
        CHECK(parse_copc_hierarchy(b5.data(), 32, e) == ErrorCode::CorruptEvlr);
        auto b6 = entry(0, 0, 0, 0, 0, 0, -1);
        CHECK(parse_copc_hierarchy(b6.data(), 32, e) == ErrorCode::CorruptEvlr);

        CHECK(e.empty());   // inget av felfallen fick lämna kvar något

        // En giltig undersida — testfilen har inga, så det här är enda täckningen
        auto ok_sub = entry(2, 1, 1, 1, 4096, 64, -1);
        CHECK(parse_copc_hierarchy(ok_sub.data(), 32, e) == ErrorCode::Ok);
        CHECK(e.size() == 1);
        CHECK(e[0].is_subpage());
        CHECK(e[0].point_count == CopcEntry::kSubpageMarker);
        CHECK(e[0].offset == 4096);
        CHECK(e[0].byte_size == 64);
        CHECK(e[0].key.level == 2 && e[0].key.x == 1 && e[0].key.y == 1 && e[0].key.z == 1);

        // Halvtrasig sida: första posten giltig, andra trasig. Vektorn ska
        // backas till det den innehöll innan anropet — inte lämnas halvfylld.
        std::vector<uint8_t> mixed;
        auto good = entry(1, 0, 0, 0, 100, 32, 5);
        auto bad2 = entry(1, 9, 0, 0, 200, 32, 5);   // 9 > 2^1
        mixed.insert(mixed.end(), good.begin(), good.end());
        mixed.insert(mixed.end(), bad2.begin(), bad2.end());
        const size_t before = e.size();
        CHECK(parse_copc_hierarchy(mixed.data(), mixed.size(), e) == ErrorCode::CorruptEvlr);
        CHECK(e.size() == before);

        std::vector<Vlr> none;
        CopcInfo ci;
        CHECK(parse_copc_info(none, ci) == ErrorCode::NotCopc);
    }

    SKIP_IF_MISSING(testdata::kCopc);

    MappedFile f;
    if (f.open(testdata::kCopc) != ErrorCode::Ok) {
        printf("  SKIP — kunde inte mmap:a %s\n", testdata::kCopc);
        return fails;
    }
    ParsedHeader h;
    CHECK(parse_header(f.data(), f.size(), h) == ErrorCode::Ok);
    std::vector<Vlr> vlrs;
    CHECK(parse_vlrs(f.data(), f.size(), h.header_size, h.num_vlrs, vlrs) == ErrorCode::Ok);

    // 2. copc info-VLR
    CopcInfo info;
    CHECK(parse_copc_info(vlrs, info) == ErrorCode::Ok);
    CHECK(std::fabs(info.center_x  - testdata::kCopcCenterX)  < 1e-6);
    CHECK(std::fabs(info.center_y  - testdata::kCopcCenterY)  < 1e-6);
    CHECK(std::fabs(info.center_z  - testdata::kCopcCenterZ)  < 1e-6);
    CHECK(std::fabs(info.halfsize  - testdata::kCopcHalfSize) < 1e-6);
    CHECK(std::fabs(info.spacing   - testdata::kCopcSpacing)  < 1e-9);
    CHECK(info.root_hier_offset == testdata::kCopcRootHierOffset);
    CHECK(info.root_hier_size   == testdata::kCopcRootHierSize);

    // Rotkuben måste omsluta filens bounding box
    CHECK(info.center_x - info.halfsize <= h.min_x);
    CHECK(info.center_x + info.halfsize >= h.max_x);
    CHECK(info.center_y - info.halfsize <= h.min_y);
    CHECK(info.center_y + info.halfsize >= h.max_y);

    // 3. En vanlig LAZ utan copc-VLR ska ge NotCopc, inte ett falskt index
    if (testdata::exists(testdata::kLaz)) {
        MappedFile g;
        if (g.open(testdata::kLaz) == ErrorCode::Ok) {
            ParsedHeader gh;
            parse_header(g.data(), g.size(), gh);
            std::vector<Vlr> gv;
            parse_vlrs(g.data(), g.size(), gh.header_size, gh.num_vlrs, gv);
            CopcInfo gi;
            CHECK(parse_copc_info(gv, gi) == ErrorCode::NotCopc);
        }
    }

    // 4. Hierarkin. Subtraktionsform — offset + size kan wrappa i uint64_t,
    //    och CHECK avbryter inte, så pekaren måste vara säker att bilda.
    const bool hier_in_file =
        info.root_hier_offset <= f.size() &&
        info.root_hier_size <= f.size() - info.root_hier_offset;
    CHECK(hier_in_file);
    if (!hier_in_file) {
        printf("  ABORT — hierarkin ligger utanför filen\n");
        return fails;
    }

    std::vector<CopcEntry> nodes;
    CHECK(parse_copc_hierarchy(f.data() + info.root_hier_offset,
                               info.root_hier_size, nodes) == ErrorCode::Ok);
    CHECK(nodes.size() == testdata::kCopcRootHierNodes);

    // Alla poster i den här filen är löv — inga undersidor
    uint64_t sum_points = 0, sum_bytes = 0, subpages = 0;
    int32_t min_level = 1000, max_level = -1;
    bool offsets_in_file = true;
    uint32_t roots = 0;
    for (const auto& e : nodes) {
        if (e.is_subpage()) { ++subpages; continue; }
        sum_points += static_cast<uint64_t>(e.point_count);
        sum_bytes  += static_cast<uint64_t>(e.byte_size);
        if (e.key.level < min_level) min_level = e.key.level;
        if (e.key.level > max_level) max_level = e.key.level;
        const uint64_t bs = static_cast<uint64_t>(e.byte_size);
        if (e.offset > f.size() || bs > f.size() - e.offset)
            offsets_in_file = false;
        if (e.key.level == 0) ++roots;
    }
    CHECK(subpages == 0);
    CHECK(offsets_in_file);
    CHECK(min_level == 0);
    CHECK(max_level == 5);
    CHECK(roots == 1);   // exakt en rotnod (0,0,0,0)

    // Den starka invarianten: octreet innehåller varenda punkt i filen, en gång.
    CHECK(sum_points == testdata::kPointCount);
    CHECK(sum_points == h.point_count);

    printf("  copc_index: %zu noder, nivå %d..%d, %llu punkter (= header), %llu chunk-byte\n",
           nodes.size(), min_level, max_level,
           (unsigned long long)sum_points, (unsigned long long)sum_bytes);

    // 4b. Undersidor.
    //
    //     Varken Autzen eller de nationella laserrutorna delar upp hierarkin — båda
    //     får plats på en sida. Så för att testa stigen bygger vi en källa som
    //     serverar den RIKTIGA filen men med hierarkin omstöpt: en rotsida med
    //     några löv och pekare till två undersidor, som i sin tur innehåller
    //     resten. Resultatet måste bli exakt samma 278 noder.
    {
        struct SplitSource final : ByteSource {
            const uint8_t* file = nullptr;
            uint64_t       total = 0;
            // Omskrivna regioner: offset -> byte
            std::vector<std::pair<uint64_t, std::vector<uint8_t>>> patches;

            bool read(uint64_t offset, size_t len, uint8_t* dest) override {
                if (!range_ok(offset, len, total)) return false;
                std::memcpy(dest, file + offset, len);
                // Lägg patcharna ovanpå, för den del som överlappar
                for (const auto& pr : patches) {
                    const uint64_t ps = pr.first, pe = ps + pr.second.size();
                    const uint64_t rs = offset,   re = offset + len;
                    const uint64_t s = ps > rs ? ps : rs;
                    const uint64_t e = pe < re ? pe : re;
                    if (s < e)
                        std::memcpy(dest + (s - rs), pr.second.data() + (s - ps),
                                    static_cast<size_t>(e - s));
                }
                return true;
            }
            uint64_t size() const override { return total; }
        };

        auto entry_bytes = [](const CopcEntry& e) {
            std::vector<uint8_t> b(32, 0);
            std::memcpy(b.data() +  0, &e.key.level, 4);
            std::memcpy(b.data() +  4, &e.key.x, 4);
            std::memcpy(b.data() +  8, &e.key.y, 4);
            std::memcpy(b.data() + 12, &e.key.z, 4);
            std::memcpy(b.data() + 16, &e.offset, 8);
            std::memcpy(b.data() + 24, &e.byte_size, 4);
            std::memcpy(b.data() + 28, &e.point_count, 4);
            return b;
        };

        // Dela de 278 noderna: 78 i rotsidan, 100 + 100 i två undersidor.
        // Undersidorna läggs i outnyttjat utrymme — vi lånar början av
        // punktdatan, som ändå inte läses i det här testet.
        const size_t n_root = 78, n_a = 100;
        const uint64_t sub_a_off = 2000, sub_b_off = 2000 + n_a * 32;

        std::vector<uint8_t> root, suba, subb;
        for (size_t i = 0; i < n_root; ++i) {
            auto b = entry_bytes(nodes[i]);
            root.insert(root.end(), b.begin(), b.end());
        }
        // Två pekarposter i rotsidan
        CopcEntry pa{}; pa.key = CopcKey{0,0,0,0};
        pa.offset = sub_a_off; pa.byte_size = int32_t(n_a * 32);
        pa.point_count = CopcEntry::kSubpageMarker;
        auto pab = entry_bytes(pa);
        root.insert(root.end(), pab.begin(), pab.end());

        const size_t n_b = nodes.size() - n_root - n_a;
        CopcEntry pb{}; pb.key = CopcKey{0,0,0,0};
        pb.offset = sub_b_off; pb.byte_size = int32_t(n_b * 32);
        pb.point_count = CopcEntry::kSubpageMarker;
        auto pbb = entry_bytes(pb);
        root.insert(root.end(), pbb.begin(), pbb.end());

        for (size_t i = n_root; i < n_root + n_a; ++i) {
            auto b = entry_bytes(nodes[i]);
            suba.insert(suba.end(), b.begin(), b.end());
        }
        for (size_t i = n_root + n_a; i < nodes.size(); ++i) {
            auto b = entry_bytes(nodes[i]);
            subb.insert(subb.end(), b.begin(), b.end());
        }

        // Peka om info-VLR:n till den nya rotsidan, som vi lägger efter subb
        const uint64_t root_off = sub_b_off + n_b * 32;
        std::vector<uint8_t> info_patch(16);
        const uint64_t rsz = root.size();
        std::memcpy(info_patch.data(),     &root_off, 8);
        std::memcpy(info_patch.data() + 8, &rsz, 8);

        SplitSource ss;
        ss.file  = f.data();
        ss.total = f.size();
        ss.patches.push_back({ 375 + 54 + 40, info_patch });
        ss.patches.push_back({ sub_a_off, suba });
        ss.patches.push_back({ sub_b_off, subb });
        ss.patches.push_back({ root_off,  root });

        CopcInfo si; std::vector<CopcEntry> sn;
        CHECK(read_copc_index(ss, si, sn) == ErrorCode::Ok);
        CHECK(si.root_hier_offset == root_off);

        // Samma noder som den odelade filen — pekarposterna ska vara borta
        CHECK(sn.size() == nodes.size());
        uint64_t sub_left = 0, node_diffs = 0, sum = 0;
        for (const auto& e : sn) {
            if (e.is_subpage()) ++sub_left;
            sum += static_cast<uint64_t>(e.point_count);
        }
        CHECK(sub_left == 0);
        CHECK(sum == testdata::kPointCount);

        // Ordningen blir rot först, sedan undersidorna — jämför som mängd
        auto key_of = [](const CopcEntry& e) {
            return std::to_string(e.key.level) + "/" + std::to_string(e.key.x) + "/" +
                   std::to_string(e.key.y) + "/" + std::to_string(e.key.z) + ":" +
                   std::to_string(e.offset);
        };
        std::vector<std::string> a, b;
        for (const auto& e : nodes) a.push_back(key_of(e));
        for (const auto& e : sn)    b.push_back(key_of(e));
        std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
        CHECK(a == b);
        if (a != b) ++node_diffs;

        printf("  copc_subpages: 3 sidor (%zu + %zu + %zu) -> %zu noder, %llu avvikelser\n",
               n_root, n_a, n_b, sn.size(), (unsigned long long)node_diffs);

        // En sida som pekar på sig själv måste avvisas, inte snurra
        CopcEntry loop{}; loop.key = CopcKey{0,0,0,0};
        loop.offset = root_off; loop.byte_size = int32_t(root.size());
        loop.point_count = CopcEntry::kSubpageMarker;
        std::vector<uint8_t> loop_root = entry_bytes(loop);
        SplitSource ls;
        ls.file = f.data(); ls.total = f.size();
        const uint64_t lsz = loop_root.size();
        std::vector<uint8_t> lp(16);
        std::memcpy(lp.data(), &root_off, 8);
        std::memcpy(lp.data() + 8, &lsz, 8);
        ls.patches.push_back({ 375 + 54 + 40, lp });
        ls.patches.push_back({ root_off, loop_root });
        CopcInfo li; std::vector<CopcEntry> ln;
        CHECK(read_copc_index(ls, li, ln) == ErrorCode::CorruptEvlr);
        CHECK(ln.empty());
    }

    // 5. Samma index läst genom ByteSource — utan att hämta filen.
    //
    //    Den andra källan är medvetet INTE minnesmappad. Den läser med fseek och
    //    fread och räknar sina anrop, alltså samma form som en HTTP-klient: hämta
    //    ett intervall, få byte tillbaka. Att båda ger identiskt resultat är
    //    beviset för att abstraktionen håller — innan någon nätverkskod finns.
    {
        // Räknande stdio-källa, byggd bara för det här testet.
        struct StdioByteSource final : ByteSource {
            FILE*    f = nullptr;
            uint64_t total = 0;
            int      reads = 0;
            uint64_t bytes = 0;

            ~StdioByteSource() override { if (f) std::fclose(f); }

            bool read(uint64_t offset, size_t len, uint8_t* dest) override {
                if (!f || !dest) return false;
                if (!range_ok(offset, len, total)) return false;
                ++reads; bytes += len;
                if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return false;
                return std::fread(dest, 1, len, f) == len;
            }
            uint64_t size() const override { return total; }
        };

        FileByteSource fbs;
        CHECK(fbs.open(testdata::kCopc) == ErrorCode::Ok);
        CopcInfo i1; std::vector<CopcEntry> n1;
        CHECK(read_copc_index(fbs, i1, n1) == ErrorCode::Ok);

        StdioByteSource sbs;
        sbs.f = std::fopen(testdata::kCopc, "rb");
        CHECK(sbs.f != nullptr);
        if (sbs.f) {
            std::fseek(sbs.f, 0, SEEK_END);
            sbs.total = static_cast<uint64_t>(std::ftell(sbs.f));
        }
        CopcInfo i2; std::vector<CopcEntry> n2;
        CHECK(read_copc_index(sbs, i2, n2) == ErrorCode::Ok);

        // Identiskt resultat från båda källorna, och identiskt med den
        // direkta pekarläsningen tidigare i testet.
        CHECK(sbs.total == testdata::kCopcFileSize);
        CHECK(i1.root_hier_offset == i2.root_hier_offset);
        CHECK(i1.root_hier_size   == i2.root_hier_size);
        CHECK(i1.spacing == i2.spacing);
        CHECK(i1.halfsize == i2.halfsize);
        CHECK(i1.root_hier_offset == info.root_hier_offset);
        CHECK(n1.size() == nodes.size());
        CHECK(n2.size() == nodes.size());

        uint64_t diffs = 0;
        for (size_t i = 0; i < nodes.size() && i < n2.size(); ++i) {
            if (!(n2[i].key == nodes[i].key) ||
                n2[i].offset      != nodes[i].offset ||
                n2[i].byte_size   != nodes[i].byte_size ||
                n2[i].point_count != nodes[i].point_count)
                ++diffs;
        }
        CHECK(diffs == 0);

        // Tre läsningar: header, info-VLR, hierarki. Inte hela filen.
        CHECK(sbs.reads == 3);
        CHECK(sbs.bytes < 16384);
        printf("  byte_source: %d läsningar, %llu byte av %llu (%.4f%% av filen), 0 avvikelser\n",
               sbs.reads, (unsigned long long)sbs.bytes,
               (unsigned long long)sbs.total, 100.0 * sbs.bytes / sbs.total);
    }

    // 6. Trasig källa ska nekas, inte läsa utanför
    {
        struct TinySource final : ByteSource {
            uint8_t buf[16] = {};
            bool read(uint64_t offset, size_t len, uint8_t* dest) override {
                if (!range_ok(offset, len, sizeof(buf))) return false;
                std::memcpy(dest, buf + offset, len);
                return true;
            }
            uint64_t size() const override { return sizeof(buf); }
        } tiny;
        CopcInfo ci; std::vector<CopcEntry> cn;
        // För liten för ens en LAS-header
        CHECK(read_copc_index(tiny, ci, cn) == ErrorCode::ReadPastEnd);
        CHECK(cn.empty());

        // En källa som ljuger: giltig header och info-VLR, men hierarkin
        // påstås ligga utanför filen med en enorm storlek. Får inte leda till
        // att vi allokerar först och upptäcker felet efteråt.
        struct LyingSource final : ByteSource {
            std::vector<uint8_t> head;   // header + info-VLR, i övrigt tom
            uint64_t claimed = 0;
            bool read(uint64_t offset, size_t len, uint8_t* dest) override {
                if (!range_ok(offset, len, claimed)) return false;
                if (offset + len > head.size()) return false;   // bara huvudet finns
                std::memcpy(dest, head.data() + offset, len);
                return true;
            }
            uint64_t size() const override { return claimed; }
        } liar;
        {
            // Kopiera riktig header + info-VLR, peta sedan om hierarkin
            liar.head.resize(375 + 54 + 160);
            FileByteSource real;
            if (real.open(testdata::kCopc) == ErrorCode::Ok)
                CHECK(real.read(0, liar.head.size(), liar.head.data()));
            uint64_t bad_off = 1ull << 62;
            uint64_t bad_size = 1ull << 40;      // 1 TB
            std::memcpy(liar.head.data() + 375 + 54 + 40, &bad_off, 8);
            std::memcpy(liar.head.data() + 375 + 54 + 48, &bad_size, 8);
            liar.claimed = UINT64_MAX;           // påstår sig vara enorm

            CopcInfo li; std::vector<CopcEntry> ln;
            CHECK(read_copc_index(liar, li, ln) == ErrorCode::CorruptEvlr);
            CHECK(ln.empty());
        }

        // range_ok får inte wrappa: offset nära uint64-max plus längd
        CHECK(!ByteSource::range_ok(UINT64_MAX, 8, 1024));
        CHECK(!ByteSource::range_ok(1000, 100, 1024));
        CHECK(ByteSource::range_ok(1000, 24, 1024));
        CHECK(ByteSource::range_ok(1024, 0, 1024));
    }

#ifdef HLAS_HAVE_LAZ
    // 7. Partiell uppackning — en enda nod.
    //
    //    Kärnan i COPC. Noden är en självständig LAZ-chunk, så den ska gå att
    //    packa upp utan att röra resten av filen. Beviset är att punkterna blir
    //    bit-identiska med motsvarande skiva ur den fullständiga uppackningen.
    {
        LazReader laz;
        CHECK(laz.init_layout(h, vlrs) == ErrorCode::Ok);
        CHECK(laz.record_length() == testdata::kRecLen14);

        // Noderna ligger i filen i offsetordning, och det är samma ordning som
        // den fullständiga uppackningen skriver dem i. Kumulativa punktantal ger
        // därmed varje nods startindex i den fulla strömmen.
        std::vector<CopcEntry> ordered = nodes;
        std::sort(ordered.begin(), ordered.end(),
                  [](const CopcEntry& a, const CopcEntry& b) { return a.offset < b.offset; });

        // Facit: hela filen uppackad en gång.
        HLas::Reader full;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(full.open(testdata::kCopc) == HLas::Status::Ok);
        double full_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0).count();

        FileByteSource fbs2;
        CHECK(fbs2.open(testdata::kCopc) == ErrorCode::Ok);

        // Stickprov spritt över octreet: första, sista och var 37:e nod.
        std::vector<size_t> pick;
        for (size_t i = 0; i < ordered.size(); i += 37) pick.push_back(i);
        pick.push_back(ordered.size() - 1);

        uint64_t start_index = 0;
        std::vector<uint64_t> starts(ordered.size(), 0);
        for (size_t i = 0; i < ordered.size(); ++i) {
            starts[i] = start_index;
            start_index += static_cast<uint64_t>(ordered[i].point_count);
        }
        CHECK(start_index == testdata::kPointCount);

        std::vector<uint8_t> node_pts, ref;
        uint64_t mismatches = 0, checked_nodes = 0, checked_points = 0, node_bytes = 0;
        double node_ms = 0.0;

        for (size_t idx : pick) {
            const CopcEntry& e = ordered[idx];
            auto n0 = std::chrono::steady_clock::now();
            ErrorCode ec = read_copc_node(fbs2, laz, e, node_pts);
            node_ms += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - n0).count();
            CHECK(ec == ErrorCode::Ok);
            if (ec != ErrorCode::Ok) continue;

            CHECK(node_pts.size() ==
                  static_cast<size_t>(e.point_count) * testdata::kRecLen14);
            node_bytes += static_cast<uint64_t>(e.byte_size);

            ref.resize(node_pts.size());
            uint64_t got = full.read_points(starts[idx],
                                            static_cast<uint64_t>(e.point_count),
                                            ref.data());
            CHECK(got == static_cast<uint64_t>(e.point_count));
            if (std::memcmp(node_pts.data(), ref.data(), node_pts.size()) != 0)
                ++mismatches;

            ++checked_nodes;
            checked_points += static_cast<uint64_t>(e.point_count);
        }

        CHECK(mismatches == 0);
        printf("  copc_node: %llu noder, %llu punkter, %llu avvikelser\n",
               (unsigned long long)checked_nodes,
               (unsigned long long)checked_points,
               (unsigned long long)mismatches);
        printf("    partiellt: %.1f ms för %llu byte  |  fullt: %.0f ms för hela filen\n",
               node_ms, (unsigned long long)node_bytes, full_ms);

        // 8. Trasiga noder ska nekas, inte packas upp
        std::vector<uint8_t> tmp;
        CopcEntry bad = ordered[0];
        bad.byte_size = -1;
        CHECK(read_copc_node(fbs2, laz, bad, tmp) == ErrorCode::CorruptEvlr);
        bad = ordered[0]; bad.point_count = CopcEntry::kSubpageMarker;
        CHECK(read_copc_node(fbs2, laz, bad, tmp) == ErrorCode::CorruptEvlr);
        bad = ordered[0]; bad.offset = fbs2.size();   // hela chunken utanför filen
        CHECK(read_copc_node(fbs2, laz, bad, tmp) == ErrorCode::CorruptEvlr);
        bad = ordered[0]; bad.point_count += 1;       // ljuger om punktantalet
        CHECK(read_copc_node(fbs2, laz, bad, tmp) == ErrorCode::CorruptChunk);
        CHECK(tmp.empty());

        // 9. Avhuggen chunk. Avkodarnas read_chunk_sizes och init_chunk tar ingen
        //    slutpekare — de litar på att längden är kontrollerad. Här matas de
        //    med successivt kortare indata och får inte läsa utanför bufferten.
        {
            const CopcEntry& e0 = ordered[0];
            std::vector<uint8_t> whole(static_cast<size_t>(e0.byte_size));
            CHECK(fbs2.read(e0.offset, whole.size(), whole.data()));

            std::vector<uint8_t> sink(static_cast<size_t>(e0.point_count) *
                                      testdata::kRecLen14);
            uint64_t rejected = 0, cut_points = 0;
            // Klipp på en rad ställen: mitt i seed, mitt i lagerhuvudet,
            // mitt i nyttolasten, och strax före slutet.
            for (size_t cut : { size_t(0), size_t(1), size_t(20),
                                size_t(testdata::kRecLen14),
                                size_t(testdata::kRecLen14 + 4),
                                size_t(testdata::kRecLen14 + 20),
                                size_t(testdata::kRecLen14 + 44),
                                whole.size() / 2,
                                whole.size() - 1 }) {
                if (cut >= whole.size()) continue;
                ++cut_points;
                ErrorCode ec2 = laz.decompress_chunk(whole.data(), cut,
                                                     uint32_t(e0.point_count),
                                                     sink.data(), sink.size());
                if (ec2 != ErrorCode::Ok) ++rejected;
            }
            CHECK(cut_points > 0);
            CHECK(rejected == cut_points);   // varenda avhuggning måste nekas

            // Hela chunken ska däremot gå igenom
            CHECK(laz.decompress_chunk(whole.data(), whole.size(),
                                       uint32_t(e0.point_count),
                                       sink.data(), sink.size()) == ErrorCode::Ok);

            // För liten utbuffert ska nekas, inte skriva utanför
            CHECK(laz.decompress_chunk(whole.data(), whole.size(),
                                       uint32_t(e0.point_count),
                                       sink.data(), sink.size() - 1) ==
                  ErrorCode::CorruptChunk);

            printf("  copc_node_trunc: %llu avhuggningar, alla nekade\n",
                   (unsigned long long)cut_points);
        }

        // 10. Nodernas kuber måste faktiskt innehålla sina punkter.
        {
            uint64_t outside = 0, tested = 0;
            std::vector<uint8_t> pts;
            for (size_t idx : pick) {
                const CopcEntry& e = ordered[idx];
                if (read_copc_node(fbs2, laz, e, pts) != ErrorCode::Ok) continue;
                const CopcBounds b = copc_node_bounds(info, e.key);
                for (int32_t i = 0; i < e.point_count; i += 97) {
                    const uint8_t* p = pts.data() +
                        static_cast<size_t>(i) * testdata::kRecLen14;
                    int32_t ix, iy, iz;
                    std::memcpy(&ix, p, 4); std::memcpy(&iy, p+4, 4); std::memcpy(&iz, p+8, 4);
                    double x = ix * h.x_scale + h.x_offset;
                    double y = iy * h.y_scale + h.y_offset;
                    double z = iz * h.z_scale + h.z_offset;
                    const double eps = 1e-6;
                    if (x < b.min_x - eps || x > b.max_x + eps ||
                        y < b.min_y - eps || y > b.max_y + eps ||
                        z < b.min_z - eps || z > b.max_z + eps)
                        ++outside;
                    ++tested;
                }
            }
            CHECK(tested > 0);
            CHECK(outside == 0);
            printf("  copc_bounds: %llu punkter testade mot sin nods kub, %llu utanför\n",
                   (unsigned long long)tested, (unsigned long long)outside);
        }

        // 11. LOD och full utbredning
        {
            CopcSelection all = select_copc_nodes(info, nodes,
                                                  h.min_x, h.min_y, h.max_x, h.max_y, -1);
            CHECK(all.nodes.size() == nodes.size());
            CHECK(all.point_count == testdata::kPointCount);

            CopcSelection root = select_copc_nodes(info, nodes,
                                                   h.min_x, h.min_y, h.max_x, h.max_y, 0);
            CHECK(root.nodes.size() == 1);
            CHECK(root.nodes[0].key.level == 0);

            // Varje nivåtak ska ge minst lika många noder som det föregående
            uint64_t prev = 0;
            for (int lvl = 0; lvl <= 5; ++lvl) {
                CopcSelection s2 = select_copc_nodes(info, nodes,
                                                     h.min_x, h.min_y, h.max_x, h.max_y, lvl);
                CHECK(s2.nodes.size() >= prev);
                prev = s2.nodes.size();
            }
            CHECK(prev == nodes.size());

            // En rektangel långt utanför filen ska ge tomt urval
            CopcSelection none = select_copc_nodes(info, nodes,
                                                   h.max_x + 1e6, h.max_y + 1e6,
                                                   h.max_x + 2e6, h.max_y + 2e6, -1);
            CHECK(none.nodes.empty());
            CHECK(none.point_count == 0);

            // Omvänd rektangel ger samma sak som rättvänd
            CopcSelection rev = select_copc_nodes(info, nodes,
                                                  h.max_x, h.max_y, h.min_x, h.min_y, -1);
            CHECK(rev.nodes.size() == all.nodes.size());

            // NaN i rektangeln har ingen definierad insida
            const double nan_v = std::nan("");
            CHECK(select_copc_nodes(info, nodes, nan_v, h.min_y,
                                    h.max_x, h.max_y, -1).nodes.empty());
            CHECK(select_copc_nodes(info, nodes, h.min_x, h.min_y,
                                    h.max_x, nan_v, -1).nodes.empty());

            // Handgjorda poster med negativa fält får inte spränga summorna
            std::vector<CopcEntry> hostile;
            CopcEntry hb{}; hb.key = CopcKey{0,0,0,0};
            hb.offset = 0; hb.byte_size = -1; hb.point_count = 10;
            hostile.push_back(hb);
            hb.byte_size = 10; hb.point_count = -2;
            hostile.push_back(hb);
            CopcSelection hs = select_copc_nodes(info, hostile,
                                                 h.min_x, h.min_y, h.max_x, h.max_y, -1);
            CHECK(hs.nodes.empty());
            CHECK(hs.point_count == 0);
            CHECK(hs.byte_size == 0);

            // COPC-noder är inte redundanta: nivåerna delar upp punkterna, de
            // duplicerar dem inte. Att summan över alla noder är exakt filens
            // punktantal bevisar det — vore de redundanta vore summan större.
            CHECK(all.point_count == h.point_count);

            printf("  copc_lod: nivå 0 -> %zu nod, alla nivåer -> %zu noder, %llu punkter\n",
                   root.nodes.size(), all.nodes.size(),
                   (unsigned long long)all.point_count);
        }

        // 12. Urvalet får inte missa en enda punkt.
        //
        //     Räkna punkter inom en rektangel ur HELA filen, räkna sedan samma
        //     sak ur bara de utvalda noderna. Lika siffror betyder att den
        //     grova nodgallringen inte tappat något.
        {
            const double qx0 = h.min_x + (h.max_x - h.min_x) * 0.30;
            const double qx1 = h.min_x + (h.max_x - h.min_x) * 0.45;
            const double qy0 = h.min_y + (h.max_y - h.min_y) * 0.55;
            const double qy1 = h.min_y + (h.max_y - h.min_y) * 0.70;

            // Facit: full skanning
            uint64_t ref_inside = 0;
            const uint64_t total = full.header().point_count;
            const uint64_t chunk = 100000;
            std::vector<double> bx(chunk), by(chunk), bz(chunk);
            for (uint64_t off = 0; off < total; off += chunk) {
                uint64_t n = full.get_xyz_batch(off, chunk, bx.data(), by.data(), bz.data());
                for (uint64_t i = 0; i < n; ++i)
                    if (bx[i] >= qx0 && bx[i] <= qx1 && by[i] >= qy0 && by[i] <= qy1)
                        ++ref_inside;
            }

            CopcSelection sel = select_copc_nodes(info, nodes, qx0, qy0, qx1, qy1, -1);
            CHECK(!sel.nodes.empty());
            CHECK(sel.nodes.size() < nodes.size());

            uint64_t sel_inside = 0;
            std::vector<uint8_t> pts;
            for (const auto& e : sel.nodes) {
                CHECK(read_copc_node(fbs2, laz, e, pts) == ErrorCode::Ok);
                for (int32_t i = 0; i < e.point_count; ++i) {
                    const uint8_t* p = pts.data() +
                        static_cast<size_t>(i) * testdata::kRecLen14;
                    int32_t ix, iy;
                    std::memcpy(&ix, p, 4); std::memcpy(&iy, p+4, 4);
                    double x = ix * h.x_scale + h.x_offset;
                    double y = iy * h.y_scale + h.y_offset;
                    if (x >= qx0 && x <= qx1 && y >= qy0 && y <= qy1)
                        ++sel_inside;
                }
            }

            CHECK(sel_inside == ref_inside);
            printf("  copc_query: %llu punkter i rutan — %llu ur full fil, %llu ur %zu/%zu noder\n",
                   (unsigned long long)ref_inside, (unsigned long long)ref_inside,
                   (unsigned long long)sel_inside, sel.nodes.size(), nodes.size());
            printf("    hämtat: %llu av %llu byte (%.1f%%)\n",
                   (unsigned long long)sel.byte_size,
                   (unsigned long long)testdata::kCopcFileSize,
                   100.0 * sel.byte_size / testdata::kCopcFileSize);
        }
    }
#endif // HLAS_HAVE_LAZ

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}

// Octreet byggt över filens utbredning. Behöver ingen testdata: filen
// byggs i minnet, så provet körs alltid.
int test_copc_root_extent() {
    int fails = 0;

    // 6b. Octreet byggt över filens utbredning, inte över kuben.
    //
    //    Sammanfogade filer med rektangulär utbredning har en kub
    //    i COPC-infon men noderna fördelade över rektangeln. Filen här byggs i
    //    minnet: header, info-VLR, en hierarkisida och för varje nod en chunk
    //    som bara bär seed-posten och punktantalet — allt read_copc_index läser.
    //    Utbredningen är E 0–200 × N 0–ext_y, kuben 200 m med centrum i mitten.
    //    Med ext_y = 100 sträcker sig kuben N −50–150, precis som den sammanfogade filen.
    {
        struct MemSource final : ByteSource {
            std::vector<uint8_t> buf;
            int      reads = 0;
            uint64_t fail_from = UINT64_MAX;   // läsningar från hit misslyckas
            bool read(uint64_t off, size_t len, uint8_t* dest) override {
                if (!range_ok(off, len, buf.size())) return false;
                if (off >= fail_from) return false;
                ++reads;
                std::memcpy(dest, buf.data() + off, len);
                return true;
            }
            uint64_t size() const override { return buf.size(); }
        };

        enum class Seeds { Extent, Nowhere };
        constexpr uint16_t kRecLen = 30;            // punktformat 6
        constexpr double   kScale  = 0.01;
        constexpr uint64_t kChunkLen = kRecLen + 4; // seed-post + punktantal
        constexpr uint64_t kInfoAt = 375, kHierAt = 375 + 54 + 160;

        auto put = [](std::vector<uint8_t>& b, uint64_t at, auto v) {
            std::memcpy(b.data() + at, &v, sizeof(v));
        };

        // Roten och hela nivå 3 i XY (8 × 8, z = 0). Seed-punkten läggs i
        // nodens ruta räknad från UTBREDNINGEN, eller i samma hörn för alla.
        auto build = [&](double ext_y, Seeds seeds, MemSource& m, uint64_t& total) {
            struct Node { int32_t l, x, y; int32_t n; };
            std::vector<Node> ns{ { 0, 0, 0, 10 } };
            for (int32_t y = 0; y < 8; ++y)
                for (int32_t x = 0; x < 8; ++x)
                    ns.push_back({ 3, x, y, 100 });

            const uint64_t hier_size = ns.size() * 32;
            const uint64_t chunks_at = kHierAt + hier_size;
            m.buf.assign(chunks_at + ns.size() * kChunkLen, 0);
            auto& b = m.buf;

            total = 0;
            for (const auto& n : ns) total += static_cast<uint64_t>(n.n);

            std::memcpy(b.data(), "LASF", 4);
            b[24] = 1; b[25] = 4;
            put(b, 94, uint16_t(375));
            put(b, 96, uint32_t(kHierAt));
            put(b, 100, uint32_t(1));
            b[104] = 6 | 0x80;
            put(b, 105, kRecLen);
            for (int a = 0; a < 3; ++a) put(b, 131 + 8 * a, kScale);
            put(b, 179, 200.0);  put(b, 187, 0.0);     // max_x, min_x
            put(b, 195, ext_y);  put(b, 203, 0.0);     // max_y, min_y
            put(b, 211, 10.0);   put(b, 219, 0.0);     // max_z, min_z
            put(b, 247, total);

            std::memcpy(b.data() + kInfoAt + 2, "copc", 4);
            put(b, kInfoAt + 18, uint16_t(kCopcInfoRecordId));
            put(b, kInfoAt + 20, uint16_t(kCopcInfoSize));
            const uint64_t info = kInfoAt + 54;
            put(b, info + 0,  100.0);         // center_x
            put(b, info + 8,  ext_y / 2.0);   // center_y
            put(b, info + 16, 5.0);           // center_z
            put(b, info + 24, 100.0);         // halfsize
            put(b, info + 32, 1.0);           // spacing
            put(b, info + 40, kHierAt);
            put(b, info + 48, hier_size);

            for (size_t i = 0; i < ns.size(); ++i) {
                const Node& n = ns[i];
                const uint64_t e = kHierAt + i * 32, c = chunks_at + i * kChunkLen;
                put(b, e + 0, n.l); put(b, e + 4, n.x); put(b, e + 8, n.y); put(b, e + 12, int32_t(0));
                put(b, e + 16, c);
                put(b, e + 24, int32_t(kChunkLen));
                put(b, e + 28, n.n);

                const double cells = double(1 << n.l);
                double sx = 1.0, sy = 1.0;
                if (seeds == Seeds::Extent) {
                    sx = (n.x + 0.5) * 200.0 / cells;
                    sy = (n.y + 0.5) * ext_y / cells;
                }
                put(b, c + 0, int32_t(std::lround(sx / kScale)));
                put(b, c + 4, int32_t(std::lround(sy / kScale)));
                put(b, c + kRecLen, uint32_t(n.n));
            }
        };

        auto has_key = [](const CopcSelection& s, int32_t l, int32_t x, int32_t y) {
            for (const auto& e : s.nodes)
                if (e.key.level == l && e.key.x == x && e.key.y == y) return true;
            return false;
        };

        // A. Rektangulär, octreet över utbredningen: lådan byts, och bara då.
        {
            MemSource m; uint64_t total = 0;
            build(100.0, Seeds::Extent, m, total);
            CopcInfo ci; std::vector<CopcEntry> cn;
            CHECK(read_copc_index(m, ci, cn) == ErrorCode::Ok);
            CHECK(ci.root_from_extent);
            CHECK(ci.root_min_x == 0.0 && ci.root_size_x == 200.0);
            CHECK(ci.root_min_y == 0.0 && ci.root_size_y == 100.0);
            // Header, info, hierarki — och en läsning per seed-punkt.
            CHECK(m.reads == 3 + static_cast<int>(kCopcRootProbes));

            // Hela utbredningen täcker nu varenda punkt.
            const CopcSelection all = select_copc_nodes(ci, cn, 0, 0, 200, 100);
            CHECK(all.point_count == total);

            // En smal remsa uppe i norr, E 10–20 × N 90–95. Noden som rymmer
            // den är (3, 0, 7): N 87,5–100 med utbredningen. Med kuben ligger
            // samma nyckel på N 125–150 och väljs inte — det var felet.
            const double px[] = { 10, 20, 20, 10 }, py[] = { 90, 90, 95, 95 };
            CHECK(has_key(select_copc_nodes_polygon(ci, cn, px, py, 4), 3, 0, 7));
            CopcInfo cube = ci; cube.root_from_extent = false;
            CHECK(!has_key(select_copc_nodes_polygon(cube, cn, px, py, 4), 3, 0, 7));
            printf("  copc_root_extent: rektangulär rot bevisad med %zu seed-punkter, %d läsningar\n",
                   kCopcRootProbes, m.reads);
        }

        // B. Kvadratisk — kuben och utbredningen är samma. Inget byts och
        //    ingenting extra läses: en konform fil kostar som förut.
        {
            MemSource m; uint64_t total = 0;
            build(200.0, Seeds::Extent, m, total);
            CopcInfo ci; std::vector<CopcEntry> cn;
            CHECK(read_copc_index(m, ci, cn) == ErrorCode::Ok);
            CHECK(!ci.root_from_extent);
            CHECK(m.reads == 3);
            CHECK(select_copc_nodes(ci, cn, 0, 0, 200, 200).point_count == total);
        }

        // C. Rektangulär, men seed-punkterna stämmer med ingen av tolkningarna.
        //    ⛔ Ingen gissning: kuben behålls, och anroparen ser att indexet
        //    inte går ihop med headern.
        {
            MemSource m; uint64_t total = 0;
            build(100.0, Seeds::Nowhere, m, total);
            CopcInfo ci; std::vector<CopcEntry> cn;
            CHECK(read_copc_index(m, ci, cn) == ErrorCode::Ok);
            CHECK(!ci.root_from_extent);
            CHECK(cn.size() == 65);
            CHECK(select_copc_nodes(ci, cn, 0, 0, 200, 100).point_count < total);
        }

        // D. Seed-läsningen misslyckas: ett fel, inte en tyst kub.
        {
            MemSource m; uint64_t total = 0;
            build(100.0, Seeds::Extent, m, total);
            m.fail_from = kHierAt + 65 * 32;   // chunkarna
            CopcInfo ci; std::vector<CopcEntry> cn;
            CHECK(read_copc_index(m, ci, cn) == ErrorCode::ReadPastEnd);
            CHECK(cn.empty());
        }
    }

    return fails;
}
