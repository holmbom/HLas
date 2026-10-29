#pragma once
#ifdef HLAS_HAVE_LAZ

#include <cstdint>
#include <vector>

namespace HLas::detail {

// LAZ:s chunk-tabell — spegel av LazReader::parse_chunk_table.
//
//   uint32 version    (0)
//   uint32 antal chunkar
//   aritmetiskt kodade byte-längder, en per chunk
//
// Längderna kodas med IntegerCompressor(32 bitar, 2 instanser) på kontext 1.
// Kontext 0 används bara av den ADAPTIVA varianten, där chunkarna har olika
// punktantal och tabellen även bär antalet. Vi skriver fast chunkstorlek, så
// den strömmen ska inte finnas — skriver man den ändå läser en avkodare som
// följer chunk_size i VLR:n fel symboler.
//
// Prediktorn är föregående chunks BYTE-LÄNGD, inte en kumulativ offset;
// avkodaren ackumulerar själv. Första chunken predikteras mot 0.
//
// Längden mäts från chunkens start till nästa chunks start och får aldrig
// innehålla tabellen själv.
//
// Returnerar false om något gick fel; då är `out` orörd.
bool write_laz_chunk_table(const std::vector<uint32_t>& chunk_sizes,
                           std::vector<uint8_t>& out);

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
