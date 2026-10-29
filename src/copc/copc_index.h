#pragma once
#include "byte_source.h"
#include "las_error.h"
#include "vlr_parser.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace HLas::detail {

// Nyckel för en nod i COPC-octreet. level 0 är roten; varje nivå delar varje
// axel i två, så en nod på nivå L har koordinater i [0, 2^L).
struct CopcKey {
    int32_t level = -1;
    int32_t x = 0, y = 0, z = 0;

    bool valid() const { return level >= 0; }
    bool operator==(const CopcKey& o) const {
        return level == o.level && x == o.x && y == o.y && z == o.z;
    }
};

// En post i hierarkin — 32 byte på disk.
struct CopcEntry {
    CopcKey  key;
    uint64_t offset = 0;        // byte-offset i filen till nodens LAZ-chunk,
                                // eller till ytterligare en hierarkisida
    int32_t  byte_size = 0;     // chunkens (eller sidans) storlek i byte
    int32_t  point_count = 0;   // -1 betyder att posten pekar på en undersida

    // Specen definierar exakt -1. Andra negativa värden är ogiltiga och
    // avvisas redan i parsningen, så likhet är rätt test här.
    bool is_subpage() const { return point_count == kSubpageMarker; }

    static constexpr int32_t kSubpageMarker = -1;
};

inline constexpr size_t kCopcEntrySize = 32;

// Högsta tillåtna octree-nivå. Vid nivå 30 är rutnätet 2^30 celler per axel,
// vilket är långt bortom vad någon verklig fil använder (Autzen når nivå 5).
// Taket gör också koordinatkontrollen meningsfull hela vägen: från nivå 31 är
// 2^level större än INT32_MAX, så varje icke-negativ int32 skulle passera.
inline constexpr int32_t kCopcMaxLevel = 30;

// Innehållet i copc info-VLR:n: user_id "copc", record_id 1, exakt 160 byte,
// och enligt spec den allra första VLR:n i filen.
struct CopcInfo {
    double   center_x = 0, center_y = 0, center_z = 0;
    double   halfsize = 0;      // halva sidan på rotnodens kub
    double   spacing  = 0;      // punktavstånd på nivå 0
    uint64_t root_hier_offset = 0;
    uint64_t root_hier_size   = 0;
    double   gpstime_min = 0, gpstime_max = 0;

    // Rotlådan när octreet INTE är byggt över kuben ovan utan över filens
    // utbredning per axel (headerns min/max). Specen kräver kuben, men
    // sammanfogade filer med rektangulär utbredning gör så här: en fil på
    // 10 × 5 km kan ha en 10 × 10 km-kub
    // i infon, och med kuben placeras de djupa noderna på fel plats — ett
    // korridorurval fick 2 % av punkterna, tyst.
    //
    // Sätts BARA av read_copc_index, och bara när seed-punkterna i de djupaste
    // noderna visat att lådan stämmer. `root_from_extent == false` betyder
    // kuben, och då används inte fälten nedan — en CopcInfo som byggts på annat
    // sätt beter sig alltså som förut.
    bool     root_from_extent = false;
    double   root_min_x = 0, root_min_y = 0, root_min_z = 0;
    double   root_size_x = 0, root_size_y = 0, root_size_z = 0;
};

// Hur många av de djupaste noderna vars seed-punkt läses innan rotlådan byts
// till filens utbredning. Bara för filer där kuben inte går ihop med
// utbredningen — en konform fil läses med lika många läsningar som förut.
inline constexpr size_t kCopcRootProbes = 8;

// Tak för en enskild hierarkisida. 32 byte per nod ger 2 miljoner noder, vilket
// räcker för punktmoln långt bortom vad som är rimligt — Autzens 10,6 miljoner
// punkter ryms i 278 noder. Taket finns för att en fjärrkälla kan ljuga om sin
// storlek: utan det skulle en elak server kunna be oss allokera godtyckligt mycket.
inline constexpr uint64_t kCopcMaxHierarchyBytes = 64ull * 1024 * 1024;

// Tak för hur många hierarkisidor som följs, och hur många noder indexet får
// innehålla totalt. En fil kan peka ut undersidor i en kedja eller en cykel;
// utan tak skulle vi hämta i all oändlighet. Autzen har 1 sida och 278 noder,
// en nationell laserruta 1 sida och 4827.
inline constexpr uint32_t kCopcMaxHierarchyPages = 4096;

// Största glapp som slås ihop när flera hierarkisidor hämtas i en läsning.
//
// Sidorna ligger i praktiken direkt efter varandra — skrivaren lägger dem
// tillsammans. Mätt på en nationell laserruta: 154 sidor, 391 456 byte, noll
// glapp. Att hämta dem i en läsning i stället för 154 tar öppningstiden från
// 33 sekunder till en rundtur, och belastar servern med en förfrågan i stället
// för 154.
//
// Taket finns för filer där de inte är det: då hämtas de i flera körningar i
// stället för att vi drar hem allt däremellan.
inline constexpr uint64_t kCopcMaxPageGap = 256 * 1024;
inline constexpr uint64_t kCopcMaxNodes          = 8ull * 1024 * 1024;

inline constexpr uint16_t kCopcInfoRecordId      = 1;
inline constexpr uint16_t kCopcHierarchyRecordId = 1000;
inline constexpr size_t   kCopcInfoSize          = 160;

// Letar upp och tolkar copc info-VLR:n.
// Returnerar NotCopc om den saknas, CorruptVlr om storleken är fel.
ErrorCode parse_copc_info(const std::vector<Vlr>& vlrs, CopcInfo& out);

// Tolkar en hierarkisida. `data` pekar på sidans första byte, `size` är dess
// längd — alltså root_hier_size för rotsidan, eller byte_size för en undersida.
// Posterna läggs till i `out`; befintligt innehåll behålls, så flera sidor kan
// samlas i samma vektor.
ErrorCode parse_copc_hierarchy(const uint8_t* data, size_t size,
                               std::vector<CopcEntry>& out);

// Läser hela octree-indexet genom en ByteSource — utan att hämta filen.
//
// Tre riktade läsningar:
//   1. LAS-headern (375 byte på offset 0) — signatur, version, header_size
//   2. copc info-VLR:n (54 + 160 byte på offset 375) — spec garanterar att den
//      ligger först, vilket är just det som gör en riktad läsning möjlig
//   3. rot-hierarkin (root_hier_size byte på root_hier_offset)
//
// Undersidor följs. En hierarki kan vara uppdelad på flera sidor, där en post
// med `point_count == -1` pekar ut var nästa sida ligger. Sidorna hämtas tills
// inga fler återstår, och `out_nodes` innehåller sedan bara riktiga datanoder —
// pekarposterna är ett format-internt mellansteg och tas bort.
//
// Utan det här skulle en uppdelad hierarki tyst ge ett ofullständigt index:
// noder saknas, punkter saknas, och ingenting felar.
//
// Rotlådan prövas sist. Ryms alla punkter i noder vars kub skär
// headerns utbredning är filen konform och inget mer läses. Annars läses
// seed-posten — chunkens första punkt, okomprimerad — i upp till
// kCopcRootProbes av de djupaste noderna. Ligger varenda en inom sin nods ruta
// räknad från utbredningen sätts `root_from_extent`; annars behålls kuben och
// anroparen får avgöra (punkterna i indexet går då inte ihop med headern).
//
// `out_nodes` töms först. Vid fel lämnas den tom.
ErrorCode read_copc_index(ByteSource& src, CopcInfo& out_info,
                          std::vector<CopcEntry>& out_nodes);

} // namespace HLas::detail
