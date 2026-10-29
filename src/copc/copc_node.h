#pragma once
#ifdef HLAS_HAVE_LAZ

#include "byte_source.h"
#include "copc_index.h"
#include "laz_reader.h"
#include <cstdint>
#include <vector>

namespace HLas::detail {

// Hämtar och packar upp en enda COPC-nod.
//
// Det här är hela poängen med COPC: noden är en självständig LAZ-chunk, så den
// går att läsa utan att röra resten av filen. Jämför med Reader::open() på samma
// fil, som packar upp alla 10,6 miljoner punkterna.
//
// `laz` måste ha kört init_layout() — den behöver bara punktlayouten, inte
// filens chunk-tabell och inte dess innehåll.
//
// `out` får exakt node.point_count * laz.record_length() byte vid framgång, och
// töms vid fel.
//
// Taket på nodstorlek finns för att byte_size kommer ur indexet, som i sin tur
// kan komma från en fjärrkälla som ljuger.
inline constexpr uint64_t kCopcMaxNodeBytes = 256ull * 1024 * 1024;

// Tak även på det UPPACKADE resultatet. byte_size är komprimerad storlek och
// säger ingenting om hur mycket som kommer ut — point_count är en egen int32 som
// kan påstå 2 miljarder punkter, alltså ~60 GB uppackat, med en liten chunk.
inline constexpr uint64_t kCopcMaxNodePoints = 64ull * 1024 * 1024;

ErrorCode read_copc_node(ByteSource& src, const LazReader& laz,
                         const CopcEntry& node, std::vector<uint8_t>& out);

// Hämtningen och uppackningen var för sig.
//
// Delade eftersom de belastar olika saker: hämtningen belastar en främmande
// server, uppackningen den egna processorn. Vill man packa upp på åtta trådar
// men bara hålla fyra uppkopplingar mot servern måste stegen gå att styra var
// för sig.

// Validerar noden och ger dess byteintervall i källan.
ErrorCode copc_node_range(const CopcEntry& node, uint64_t src_size,
                          uint64_t& out_offset, uint64_t& out_size);

// Packar upp en redan hämtad chunk. `out` får exakt
// node.point_count * laz.record_length() byte vid framgång.
ErrorCode decompress_copc_node(const LazReader& laz, const CopcEntry& node,
                               const uint8_t* chunk, size_t chunk_len,
                               std::vector<uint8_t>& out);

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
