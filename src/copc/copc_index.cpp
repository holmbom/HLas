#include "copc_index.h"
#include "copc_query.h"
#include "header_parser.h"
#include "las_types.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <utility>

namespace HLas::detail {

namespace {

template <typename T>
T read_le(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// Punkterna i de noder vars ruta (med `info` som den står) skär headerns
// utbredning i XY. I en konform fil är det alla — varje punkt ligger i sin nod
// och inom filen.
uint64_t points_touching_extent(const CopcInfo& info, const std::vector<CopcEntry>& nodes,
                                const ParsedHeader& h) {
    uint64_t sum = 0;
    for (const auto& e : nodes) {
        const CopcBounds b = copc_node_bounds(info, e.key);
        if (b.max_x < h.min_x || b.min_x > h.max_x ||
            b.max_y < h.min_y || b.min_y > h.max_y)
            continue;
        sum += static_cast<uint64_t>(e.point_count);
    }
    return sum;
}

// Är octreet byggt över filens utbredning i stället för över kuben?
//
// Sätter `info.root_from_extent` bara när seed-punkterna i de djupaste noderna
// BEVISAR det — ⛔ aldrig på en gissning: en låda som slår fel ger tysta, glesa
// svar — en korridor som får en bråkdel av sina punkter. Ok också när inget byts; fel bara när
// en läsning misslyckas, för ett nätfel här ska inte tyst bli kuben.
ErrorCode prove_root_from_extent(ByteSource& src, const ParsedHeader& h,
                                 CopcInfo& info, const std::vector<CopcEntry>& nodes) {
    uint64_t total = 0;
    for (const auto& e : nodes) total += static_cast<uint64_t>(e.point_count);
    if (total == 0 || points_touching_extent(info, nodes, h) >= total)
        return ErrorCode::Ok;                       // konform — inget mer läses

    // Utbredningen måste vara en verklig låda i XY, och skalan måste gå att
    // räkna med — annars finns inget att pröva mot.
    if (!std::isfinite(h.min_x) || !std::isfinite(h.max_x) ||
        !std::isfinite(h.min_y) || !std::isfinite(h.max_y) ||
        !std::isfinite(h.min_z) || !std::isfinite(h.max_z) ||
        !(h.max_x > h.min_x) || !(h.max_y > h.min_y) ||
        !(h.x_scale != 0.0) || !(h.y_scale != 0.0) ||
        !std::isfinite(h.x_scale) || !std::isfinite(h.y_scale) ||
        h.point_record_length < 12)
        return ErrorCode::Ok;

    CopcInfo cand = info;
    cand.root_from_extent = true;
    cand.root_min_x = h.min_x;  cand.root_size_x = h.max_x - h.min_x;
    cand.root_min_y = h.min_y;  cand.root_size_y = h.max_y - h.min_y;
    // ⚠️ Z ur utbredningen för att vara konsekvent med X och Y. INTE uppmätt —
    // fallet med sammanfogade filer är bevisat i XY, och urvalen i HLas är 2D.
    cand.root_min_z = h.min_z;  cand.root_size_z = std::max(0.0, h.max_z - h.min_z);

    // De djupaste noderna med punkter, jämnt fördelade över listan. De djupa
    // är de små: en seed-punkt som träffar en liten ruta säger något, en som
    // träffar roten säger ingenting.
    const uint64_t seed_len = uint64_t(h.point_record_length) + 4;
    int32_t deepest = -1;
    for (const auto& e : nodes)
        if (e.point_count > 0 && e.key.level > deepest) deepest = e.key.level;
    std::vector<const CopcEntry*> deep;
    for (const auto& e : nodes)
        if (e.key.level == deepest && e.point_count > 0 &&
            static_cast<uint64_t>(e.byte_size) >= seed_len)
            deep.push_back(&e);
    if (deep.empty())
        return ErrorCode::Ok;

    const size_t probes = std::min(kCopcRootProbes, deep.size());
    std::vector<uint8_t> seed(static_cast<size_t>(seed_len));
    const double tol_x = std::fabs(h.x_scale) * 0.5;
    const double tol_y = std::fabs(h.y_scale) * 0.5;

    for (size_t i = 0; i < probes; ++i) {
        const CopcEntry& e = *deep[i * deep.size() / probes];
        if (!ByteSource::range_ok(e.offset, seed.size(), src.size()))
            return ErrorCode::CorruptEvlr;
        if (!src.read(e.offset, seed.size(), seed.data()))
            return ErrorCode::ReadPastEnd;

        // Chunken börjar med seed-posten — första punkten okomprimerad — och
        // sedan punktantalet. COPC kräver punktformat 6–8, alltså point14-
        // kodningen, och laz_reader.cpp läser chunken på samma sätt. Stämmer
        // antalet inte är det inte den layouten, och då rörs ingenting.
        if (read_le<uint32_t>(seed.data() + h.point_record_length) !=
            static_cast<uint32_t>(e.point_count))
            return ErrorCode::Ok;

        const double x = read_le<int32_t>(seed.data())     * h.x_scale + h.x_offset;
        const double y = read_le<int32_t>(seed.data() + 4) * h.y_scale + h.y_offset;
        const CopcBounds b = copc_node_bounds(cand, e.key);
        if (x < b.min_x - tol_x || x > b.max_x + tol_x ||
            y < b.min_y - tol_y || y > b.max_y + tol_y)
            return ErrorCode::Ok;   // utbredningen förklarar inte heller — behåll kuben
    }

    info = cand;
    return ErrorCode::Ok;
}

} // namespace

ErrorCode parse_copc_info(const std::vector<Vlr>& vlrs, CopcInfo& out) {
    // Specen kräver att info-VLR:n är den allra första, på offset 375. Det är
    // inte formalia: garantin gör att HTTP-stigen kan hämta header och info i
    // en enda range request i stället för att först lista alla VLR:er.
    // En fil där den ligger någon annanstans är inte en konform COPC-fil — vi
    // säger NotCopc och låter anroparen falla tillbaka på vanlig LAZ-läsning.
    if (vlrs.empty())
        return ErrorCode::NotCopc;
    const Vlr* v = &vlrs.front();
    if (std::strncmp(v->user_id, "copc", 16) != 0 || v->record_id != kCopcInfoRecordId)
        return ErrorCode::NotCopc;
    if (v->data_size != kCopcInfoSize || !v->data)
        return ErrorCode::CorruptVlr;

    const uint8_t* d = v->data;
    out.center_x = read_le<double>(d +  0);
    out.center_y = read_le<double>(d +  8);
    out.center_z = read_le<double>(d + 16);
    out.halfsize = read_le<double>(d + 24);
    out.spacing  = read_le<double>(d + 32);
    out.root_hier_offset = read_le<uint64_t>(d + 40);
    out.root_hier_size   = read_le<uint64_t>(d + 48);
    out.gpstime_min = read_le<double>(d + 56);
    out.gpstime_max = read_le<double>(d + 64);
    // Resterande 88 byte är reserverade och ska vara noll. Vi läser dem inte —
    // en skrivare som fyllt dem med skräp ska ändå gå att läsa.

    // NaN klarar sig igenom "<= 0.0" — därför isfinite först. Ett icke-ändligt
    // halfsize eller spacing skulle sedan tyst sabotera varje bounds- och
    // snittberäkning mot octreet.
    if (!std::isfinite(out.center_x) || !std::isfinite(out.center_y) ||
        !std::isfinite(out.center_z) || !std::isfinite(out.halfsize) ||
        !std::isfinite(out.spacing))
        return ErrorCode::CorruptVlr;
    if (out.halfsize <= 0.0 || out.spacing <= 0.0)
        return ErrorCode::CorruptVlr;
    if (out.root_hier_size == 0 || out.root_hier_size % kCopcEntrySize != 0)
        return ErrorCode::CorruptVlr;

    return ErrorCode::Ok;
}

ErrorCode parse_copc_hierarchy(const uint8_t* data, size_t size,
                               std::vector<CopcEntry>& out) {
    if (!data || size == 0 || size % kCopcEntrySize != 0)
        return ErrorCode::CorruptEvlr;

    const size_t n = size / kCopcEntrySize;
    // Flera sidor samlas i samma vektor, så vid fel måste vi kunna backa till
    // det som fanns innan — annars sitter anroparen med ett halvt index som
    // ser giltigt ut.
    const size_t restore_to = out.size();
    out.reserve(restore_to + n);

    auto fail = [&](ErrorCode ec) {
        out.resize(restore_to);
        return ec;
    };

    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = data + i * kCopcEntrySize;
        CopcEntry e;
        e.key.level = read_le<int32_t>(p +  0);
        e.key.x     = read_le<int32_t>(p +  4);
        e.key.y     = read_le<int32_t>(p +  8);
        e.key.z     = read_le<int32_t>(p + 12);
        e.offset      = read_le<uint64_t>(p + 16);
        e.byte_size   = read_le<int32_t>(p + 24);
        e.point_count = read_le<int32_t>(p + 28);

        // En nivå under noll finns inte, och en chunk kan inte ha negativ storlek.
        if (e.key.level < 0 || e.byte_size < 0)
            return fail(ErrorCode::CorruptEvlr);

        // Utan ett tak kan en nivå bortom det beräkningsbara ta sig igenom och
        // sedan få odefinierade gränser i copc_node_bounds — en nod som tyst
        // hamnar utanför varje urval.
        if (e.key.level > kCopcMaxLevel)
            return fail(ErrorCode::CorruptEvlr);

        // point_count är antingen ett antal (>= 0) eller exakt -1 för undersida.
        if (e.point_count < CopcEntry::kSubpageMarker)
            return fail(ErrorCode::CorruptEvlr);

        // En undersida måste vara en hel rad poster, annars går den inte att tolka
        // när den väl hämtats. Bättre att fånga det här än efter en HTTP-hämtning.
        if (e.is_subpage() &&
            (e.byte_size == 0 || e.byte_size % kCopcEntrySize != 0))
            return fail(ErrorCode::CorruptEvlr);

        // Negativa koordinater är alltid ogiltiga, oavsett nivå.
        if (e.key.x < 0 || e.key.y < 0 || e.key.z < 0)
            return fail(ErrorCode::CorruptEvlr);

        // Övre gränsen är 2^level. Nivåtaket ovan garanterar att skiftet ryms.
        const int32_t limit = int32_t(1) << e.key.level;
        if (e.key.x >= limit || e.key.y >= limit || e.key.z >= limit)
            return fail(ErrorCode::CorruptEvlr);

        out.push_back(e);
    }

    return ErrorCode::Ok;
}

ErrorCode read_copc_index(ByteSource& src, CopcInfo& out_info,
                          std::vector<CopcEntry>& out_nodes) {
    out_nodes.clear();
    out_info = CopcInfo{};

    // 1. LAS-headern. COPC kräver LAS 1.4, och därmed header_size 375 — det är
    //    den garantin som gör att info-VLR:n går att läsa utan att först lista
    //    alla VLR:er.
    constexpr uint64_t kHeaderSize = 375;
    uint8_t hdr[kHeaderSize];
    if (!src.read(0, sizeof(hdr), hdr))
        return ErrorCode::ReadPastEnd;
    if (std::memcmp(hdr, "LASF", 4) != 0)
        return ErrorCode::InvalidSignature;
    if (hdr[24] != 1 || hdr[25] != 4)
        return ErrorCode::NotCopc;      // COPC är alltid LAS 1.4
    uint16_t header_size = read_le<uint16_t>(hdr + 94);
    if (header_size != kHeaderSize)
        return ErrorCode::NotCopc;

    // 2. copc info-VLR:n ligger direkt efter headern.
    uint8_t vlr[sizeof(VlrHeader) + kCopcInfoSize];
    if (!src.read(kHeaderSize, sizeof(vlr), vlr))
        return ErrorCode::ReadPastEnd;

    // VlrHeader: reserved(2) user_id(16) record_id(2) record_length(2) desc(32)
    if (std::strncmp(reinterpret_cast<const char*>(vlr + 2), "copc", 16) != 0)
        return ErrorCode::NotCopc;
    if (read_le<uint16_t>(vlr + 18) != kCopcInfoRecordId)
        return ErrorCode::NotCopc;
    if (read_le<uint16_t>(vlr + 20) != kCopcInfoSize)
        return ErrorCode::CorruptVlr;

    Vlr info_vlr{};
    std::memcpy(info_vlr.user_id, vlr + 2, 16);
    info_vlr.record_id = kCopcInfoRecordId;
    info_vlr.data      = vlr + sizeof(VlrHeader);
    info_vlr.data_size = kCopcInfoSize;

    std::vector<Vlr> one{ info_vlr };
    ErrorCode ec = parse_copc_info(one, out_info);
    if (ec != ErrorCode::Ok)
        return ec;

    // 3. Rot-hierarkin.
    //
    //    Hela intervallet måste valideras INNAN vi allokerar. Att bara jämföra
    //    storleken mot src.size() räcker inte: en fil som ljuger om offset skulle
    //    då få oss att allokera först och upptäcka felet efteråt.
    if (!ByteSource::range_ok(out_info.root_hier_offset,
                              out_info.root_hier_size, src.size()))
        return ErrorCode::CorruptEvlr;

    if (out_info.root_hier_size > kCopcMaxHierarchyBytes)
        return ErrorCode::CorruptEvlr;

#if SIZE_MAX < UINT64_MAX
    // 32-bitars mål: en storlek som inte ryms i size_t skulle trunkeras och en
    // orimligt stor hierarki accepteras som en mindre, giltig.
    if (out_info.root_hier_size > static_cast<uint64_t>(SIZE_MAX))
        return ErrorCode::CorruptEvlr;
#endif

    std::vector<uint8_t> page(static_cast<size_t>(out_info.root_hier_size));
    if (!src.read(out_info.root_hier_offset, page.size(), page.data()))
        return ErrorCode::ReadPastEnd;

    ec = parse_copc_hierarchy(page.data(), page.size(), out_nodes);
    if (ec != ErrorCode::Ok) {
        out_nodes.clear();
        return ec;
    }

    // 4. Följ undersidor.
    //
    //    Sidorna hämtas i omgångar, inte en i taget: de ligger i praktiken
    //    direkt efter varandra i filen, så en läsning kan täcka alla. Över
    //    nätverk är skillnaden 33 sekunder mot en rundtur — och en förfrågan
    //    mot servern i stället för hundratals.
    std::set<uint64_t> visited{ out_info.root_hier_offset };
    uint32_t pages = 1;
    size_t scanned = 0;

    std::vector<uint8_t> batch;
    for (;;) {
        // Samla de undersidor som tillkommit sedan förra omgången.
        std::vector<std::pair<uint64_t, uint32_t>> pending;   // offset, storlek
        for (; scanned < out_nodes.size(); ++scanned) {
            const CopcEntry& e = out_nodes[scanned];
            if (!e.is_subpage())
                continue;
            const uint64_t sz = static_cast<uint64_t>(e.byte_size);
            if (sz > kCopcMaxHierarchyBytes ||
                !ByteSource::range_ok(e.offset, sz, src.size())) {
                out_nodes.clear();
                return ErrorCode::CorruptEvlr;
            }
            if (!visited.insert(e.offset).second) {
                out_nodes.clear();          // cykel
                return ErrorCode::CorruptEvlr;
            }
            if (++pages > kCopcMaxHierarchyPages) {
                out_nodes.clear();
                return ErrorCode::CorruptEvlr;
            }
            pending.emplace_back(e.offset, static_cast<uint32_t>(sz));
        }
        if (pending.empty())
            break;

        // Sortera på offset och slå ihop till körningar med små glapp.
        std::sort(pending.begin(), pending.end());

        uint64_t total_entries = 0;
        for (const auto& pr : pending)
            total_entries += pr.second / kCopcEntrySize;
        if (out_nodes.size() + total_entries > kCopcMaxNodes) {
            out_nodes.clear();
            return ErrorCode::CorruptEvlr;
        }

        size_t i = 0;
        while (i < pending.size()) {
            const uint64_t run_start = pending[i].first;
            uint64_t run_end = run_start + pending[i].second;
            size_t j = i + 1;
            for (; j < pending.size(); ++j) {
                const uint64_t gap = pending[j].first > run_end
                                   ? pending[j].first - run_end : 0;
                if (gap > kCopcMaxPageGap)
                    break;
                const uint64_t e = pending[j].first + pending[j].second;
                if (e > run_end) run_end = e;
            }

            const uint64_t run_len = run_end - run_start;
            if (run_len > kCopcMaxHierarchyBytes ||
                !ByteSource::range_ok(run_start, run_len, src.size())) {
                out_nodes.clear();
                return ErrorCode::CorruptEvlr;
            }
#if SIZE_MAX < UINT64_MAX
            if (run_len > static_cast<uint64_t>(SIZE_MAX)) {
                out_nodes.clear();
                return ErrorCode::CorruptEvlr;
            }
#endif
            batch.resize(static_cast<size_t>(run_len));
            if (!src.read(run_start, batch.size(), batch.data())) {
                out_nodes.clear();
                return ErrorCode::ReadPastEnd;
            }

            // Tolka varje sida ur den hämtade bufferten.
            for (size_t k = i; k < j; ++k) {
                const size_t rel = static_cast<size_t>(pending[k].first - run_start);
                ec = parse_copc_hierarchy(batch.data() + rel, pending[k].second,
                                          out_nodes);
                if (ec != ErrorCode::Ok) {
                    out_nodes.clear();
                    return ec;
                }
            }
            i = j;
        }
    }

    // Pekarposterna har gjort sitt. Det som blir kvar är datanoder, vilket är
    // vad varje konsument menar med "en nod".
    out_nodes.erase(std::remove_if(out_nodes.begin(), out_nodes.end(),
                                   [](const CopcEntry& e) { return e.is_subpage(); }),
                    out_nodes.end());

    // 5. Rotlådan. Headern finns redan i minnet; en konform fil kostar
    //    alltså ingen läsning till. Går headern inte att tolka avgörs det av
    //    läsaren som tolkar den efteråt — här rörs då ingenting.
    ParsedHeader ph{};
    if (parse_header(hdr, sizeof(hdr), ph) == ErrorCode::Ok) {
        ec = prove_root_from_extent(src, ph, out_info, out_nodes);
        if (ec != ErrorCode::Ok) {
            out_nodes.clear();
            return ec;
        }
    }

    return ErrorCode::Ok;
}

} // namespace HLas::detail
