#pragma once
#ifdef HLAS_HAVE_LAZ

#include "header_parser.h"
#include "vlr_parser.h"
#include "las_error.h"
#include "laz_vlr.h"
#include "point_source.h"
#include <vector>
#include <cstdint>

namespace HLas::detail {

class LazReader {
public:
    ErrorCode init(const ParsedHeader& header, const uint8_t* file_buffer,
                   size_t file_size, const std::vector<Vlr>& vlrs);

    // Läser bara ut punktlayouten ur LAZ-VLR:n — ingen chunk-tabell, ingen
    // dekompression. Det är allt som behövs för att packa upp en enskild chunk,
    // och är förutsättningen för COPC: att hämta en nod ska inte kosta 8 sekunder
    // av dekompression av resten av filen.
    ErrorCode init_layout(const ParsedHeader& header, const std::vector<Vlr>& vlrs);

    // Packar upp en ensam LayerChunked-chunk till `out`, som måste rymma
    // `expected_points * record_length()` byte.
    //
    // En chunk är självständig: seed-posten, punktantalet och alla lagerlängder
    // ligger i chunken själv, och avkodarna konstrueras om per chunk. Därför går
    // en nod att packa upp utan att röra resten av filen — vilket är hela grunden
    // för partiell hämtning.
    //
    // `expected_points` = 0 betyder "lita på antalet i chunken". Annars måste de
    // stämma överens, annars returneras CorruptChunk.
    //
    // `out_capacity` är obligatorisk: punktantalet står i chunken och kommer
    // alltså från filen. En trasig eller elak fil får inte kunna skriva utanför
    // bufferten genom att påstå fler punkter än den innehåller.
    ErrorCode decompress_chunk(const uint8_t* data, size_t len,
                               uint32_t expected_points,
                               uint8_t* out, size_t out_capacity,
                               uint32_t* out_points = nullptr) const;

    uint64_t count() const { return point_count_; }
    uint16_t record_length() const { return record_length_; }
    uint8_t  format() const { return format_; }
    uint16_t classification_offset() const { return class_offset_; }

    PointSource point_source() const {
        return PointSource{ decompressed_.data(), point_count_, record_length_, class_offset_ };
    }

    const uint8_t* raw_point(uint64_t index) const { return point_source().raw_point(index); }
    uint64_t read_raw(uint64_t start_index, uint64_t count, uint8_t* dest) const {
        return point_source().read_raw(start_index, count, dest);
    }

private:
    ErrorCode parse_chunk_table(const uint8_t* file_buffer, size_t file_size,
                                uint64_t offset_to_point_data,
                                const LazVlrData& laz);

    ErrorCode decompress_all(const uint8_t* file_buffer, size_t file_size,
                             uint64_t offset_to_point_data,
                             const LazVlrData& laz);

    ErrorCode decompress_all_layered(const uint8_t* file_buffer, size_t file_size,
                                      uint64_t offset_to_point_data,
                                      const LazVlrData& laz);

    ErrorCode layout_from_vlr(const LazVlrData& laz);

    // Punktlayout ur LAZ-VLR:n. Sätts av init_layout() och av init().
    struct Layout {
        bool     valid = false;
        bool     has_rgb = false, has_byte = false;
        uint16_t point14_size = 0, rgb_size = 0, byte_size = 0;
    } layout_;

    std::vector<uint8_t>    decompressed_;
    std::vector<uint64_t>   chunk_offsets_;  // byte offsets into file for each chunk
    uint64_t  point_count_   = 0;
    uint16_t  record_length_ = 0;
    uint8_t   format_        = 0;
    uint16_t  class_offset_  = 0;
};

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
