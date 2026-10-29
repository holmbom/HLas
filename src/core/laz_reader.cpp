#ifdef HLAS_HAVE_LAZ
#include "laz_reader.h"
#include "laz/arithmetic.h"
#include "laz/integer_compressor.h"
#include "laz/point10.h"
#include "laz/gpstime11.h"
#include "laz/rgb12.h"
#include "laz/byte_item.h"
#include "laz/point14.h"
#include "laz/rgb14.h"
#include "laz/byte14.h"
#include <cstring>

namespace HLas::detail {

using namespace laz;

ErrorCode LazReader::layout_from_vlr(const LazVlrData& laz) {
    bool has_point14 = false, has_rgbnir14 = false;
    Layout l{};
    for (const auto& item : laz.items) {
        switch (item.type) {
            case LazItemType::Point14:   has_point14 = true; l.point14_size = item.size; break;
            case LazItemType::RGB14:     l.has_rgb = true; l.rgb_size = item.size; break;
            case LazItemType::RGBNIR14:  has_rgbnir14 = true; l.has_rgb = true; l.rgb_size = item.size; break;
            case LazItemType::Byte14:    l.has_byte = true; l.byte_size = item.size; break;
            default: break;
        }
    }
    if (!has_point14)
        return ErrorCode::LazNotSupported;

    // Objektstorlekarna kommer ur LAZ-VLR:n, alltså ur filen, men avkodarna
    // skriver fasta antal byte: Point14 alltid 30, RGB alltid 6. Utan den här
    // kontrollen kan en trasig VLR få en avkodad post att spilla ur sin plats
    // trots att kapacitetskontrollen på punktantalet gick igenom.
    if (l.point14_size != 30)
        return ErrorCode::CorruptVlr;
    if (l.has_rgb && l.rgb_size != (has_rgbnir14 ? 8 : 6))
        return ErrorCode::CorruptVlr;

    const uint32_t item_total = uint32_t(l.point14_size) +
                                (l.has_rgb ? l.rgb_size : 0) +
                                (l.has_byte ? l.byte_size : 0);
    if (item_total > record_length_)
        return ErrorCode::CorruptVlr;

    l.valid = true;
    layout_ = l;
    return ErrorCode::Ok;
}

ErrorCode LazReader::init_layout(const ParsedHeader& header,
                                 const std::vector<Vlr>& vlrs) {
    format_ = header.point_format_id;
    record_length_ = header.point_record_length;
    point_count_ = header.point_count;
    class_offset_ = (format_ <= 5) ? 15 : 16;

    LazVlrData laz{};
    ErrorCode ec = parse_laz_vlr(vlrs, laz);
    if (ec != ErrorCode::Ok)
        return ec;
    return layout_from_vlr(laz);
}

ErrorCode LazReader::decompress_chunk(const uint8_t* data, size_t len,
                                      uint32_t expected_points,
                                      uint8_t* out, size_t out_capacity,
                                      uint32_t* out_points) const {
    if (!layout_.valid)
        return ErrorCode::LazNotSupported;
    if (!data || !out)
        return ErrorCode::CorruptChunk;

    // Seed-post + uint32 punktantal måste rymmas
    if (len < static_cast<size_t>(record_length_) + 4)
        return ErrorCode::ReadPastEnd;

    const uint8_t* ptr = data;
    const uint8_t* seed = ptr;
    ptr += record_length_;

    uint32_t pts_in_chunk;
    std::memcpy(&pts_in_chunk, ptr, 4);
    ptr += 4;

    if (pts_in_chunk == 0)
        return ErrorCode::CorruptChunk;
    if (expected_points != 0 && pts_in_chunk != expected_points)
        return ErrorCode::CorruptChunk;

    // Punktantalet kommer ur filen. Utan den här kontrollen kan en chunk som
    // påstår fler punkter än bufferten rymmer skriva utanför den.
    const uint64_t needed = static_cast<uint64_t>(pts_in_chunk) * record_length_;
    if (needed > out_capacity)
        return ErrorCode::CorruptChunk;

    if (out_points)
        *out_points = pts_in_chunk;

    // Dela seed-posten i sina delar
    uint16_t seed_off = 0;
    const uint8_t* seed_point14 = seed + seed_off;
    seed_off += layout_.point14_size;

    const uint8_t* seed_rgb14 = layout_.has_rgb ? (seed + seed_off) : nullptr;
    if (layout_.has_rgb) seed_off += layout_.rgb_size;

    const uint8_t* seed_byte14 = layout_.has_byte ? (seed + seed_off) : nullptr;

    // Lagerhuvudena måste rymmas innan vi läser dem. Avkodarnas read_chunk_sizes
    // tar ingen slutpekare — de litar på att anroparen har kontrollerat.
    const uint64_t hdr_bytes = Point14Decoder::kChunkSizeHeaderBytes +
                               (layout_.has_rgb ? RGB14Decoder::kChunkSizeHeaderBytes : 0) +
                               (layout_.has_byte ? Byte14Decoder::kChunkSizeHeaderBytes : 0);
    const uint64_t consumed = uint64_t(record_length_) + 4 + hdr_bytes;
    if (len < consumed)
        return ErrorCode::ReadPastEnd;

    Point14Decoder p14;
    p14.read_chunk_sizes(ptr);

    RGB14Decoder rgb_dec;
    if (layout_.has_rgb)
        rgb_dec.read_chunk_sizes(ptr);

    Byte14Decoder byte_dec;
    if (layout_.has_byte)
        byte_dec.read_chunk_sizes(ptr);

    // Lagerlängderna kommer också ur chunken. init_chunk kopierar blint så många
    // byte som de påstår — de måste rymmas i det som är kvar av indata.
    const uint64_t payload = p14.declared_layer_bytes() +
                             (layout_.has_rgb ? rgb_dec.declared_layer_bytes() : 0) +
                             (layout_.has_byte ? byte_dec.declared_layer_bytes() : 0);
    if (payload > len - consumed)
        return ErrorCode::CorruptChunk;

    uint32_t context = (seed_point14[15] >> 4) & 0x03;

    p14.init_chunk(ptr, seed_point14);
    if (layout_.has_rgb)
        rgb_dec.init_chunk(ptr, seed_rgb14, context);
    if (layout_.has_byte)
        byte_dec.init_chunk(ptr, seed_byte14, layout_.byte_size, context);

    for (uint32_t pi = 0; pi < pts_in_chunk; ++pi) {
        uint8_t* dst = out + static_cast<size_t>(pi) * record_length_;
        if (pi == 0) {
            std::memcpy(dst, seed, record_length_);
            continue;
        }
        uint16_t off = 0;
        p14.decompress(context, dst + off);
        off += layout_.point14_size;
        if (layout_.has_rgb) {
            rgb_dec.decompress(context, dst + off);
            off += layout_.rgb_size;
        }
        if (layout_.has_byte)
            byte_dec.decompress(context, dst + off);
    }

    return ErrorCode::Ok;
}

ErrorCode LazReader::init(const ParsedHeader& header, const uint8_t* file_buffer,
                          size_t file_size, const std::vector<Vlr>& vlrs) {
    format_ = header.point_format_id;
    record_length_ = header.point_record_length;
    point_count_ = header.point_count;
    class_offset_ = (format_ <= 5) ? 15 : 16;

    // Parse LAZ VLR
    LazVlrData laz{};
    ErrorCode ec = parse_laz_vlr(vlrs, laz);
    if (ec != ErrorCode::Ok)
        return ec;

    if (laz.compressor == LazCompressor::None) {
        // Uncompressed LAZ file — just copy point data directly
        uint64_t data_end = static_cast<uint64_t>(header.offset_to_point_data)
                          + point_count_ * record_length_;
        if (data_end > file_size)
            return ErrorCode::ReadPastEnd;

        decompressed_.resize(point_count_ * record_length_);
        std::memcpy(decompressed_.data(),
                    file_buffer + header.offset_to_point_data,
                    decompressed_.size());
        return ErrorCode::Ok;
    }

    // Parse chunk table (for chunked compression)
    ec = parse_chunk_table(file_buffer, file_size,
                           header.offset_to_point_data, laz);
    if (ec != ErrorCode::Ok)
        return ec;

    // Decompress all chunks
    if (laz.compressor == LazCompressor::LayerChunked) {
        ec = decompress_all_layered(file_buffer, file_size,
                                     header.offset_to_point_data, laz);
    } else {
        ec = decompress_all(file_buffer, file_size,
                            header.offset_to_point_data, laz);
    }
    return ec;
}

ErrorCode LazReader::parse_chunk_table(const uint8_t* file_buffer, size_t file_size,
                                       uint64_t offset_to_point_data,
                                       const LazVlrData& laz) {
    if (laz.compressor == LazCompressor::Pointwise) {
        // Single chunk, no chunk table
        chunk_offsets_.clear();
        chunk_offsets_.push_back(offset_to_point_data);
        return ErrorCode::Ok;
    }

    // Chunked: first 8 bytes at point data offset = chunk table start position
    if (offset_to_point_data + 8 > file_size)
        return ErrorCode::ReadPastEnd;

    int64_t ct_start;
    std::memcpy(&ct_start, file_buffer + offset_to_point_data, 8);

    // If -1, actual position is at end of file
    if (ct_start == -1) {
        if (file_size < 8)
            return ErrorCode::CorruptVlr;
        std::memcpy(&ct_start, file_buffer + file_size - 8, 8);
    }

    if (ct_start < 0 || static_cast<uint64_t>(ct_start) + 8 > file_size)
        return ErrorCode::CorruptVlr;

    const uint8_t* ct = file_buffer + ct_start;
    uint32_t version, num_chunks;
    std::memcpy(&version, ct, 4);
    std::memcpy(&num_chunks, ct + 4, 4);

    if (version != 0)
        return ErrorCode::CorruptVlr;

    if (num_chunks == 0) {
        chunk_offsets_.clear();
        return ErrorCode::Ok;
    }

    // Decompress chunk sizes using IC 32-bit, 2 instances
    ArithmeticDecoder dec;
    dec.init(ct + 8, file_size - static_cast<size_t>(ct_start) - 8);

    IntegerCompressor ic;
    ic.init(dec, 32, 2);

    bool adaptive = (laz.chunk_size == 0xFFFFFFFFu);

    // The chunk table is read in two passes:
    //   Pass 1: decompress individual chunk byte sizes
    //   Pass 2: accumulate into cumulative offsets

    uint64_t data_start = offset_to_point_data + 8;

    // Pass 1: decompress individual chunk sizes
    //
    // Storleken räknas i size_t: num_chunks kommer ur filen, och `num_chunks + 1`
    // i uint32 wrappar till noll vid UINT32_MAX — varefter varje indexering går
    // utanför vektorn.
    // Taket är ett allokeringstak: antalet kommer ur filen och styr en resize
    // innan något lästs. 16 miljoner chunkar är 800 miljarder punkter med
    // standardstorleken — långt bortom vad som kan finnas i en giltig fil.
    constexpr uint32_t kMaxChunks = 16u * 1024u * 1024u;
    if (num_chunks > kMaxChunks)
        return ErrorCode::CorruptVlr;
    std::vector<uint32_t> chunk_bytes(static_cast<size_t>(num_chunks) + 1, 0);
    std::vector<uint32_t> chunk_totals;
    if (adaptive)
        chunk_totals.resize(static_cast<size_t>(num_chunks) + 1, 0);

    for (uint32_t i = 1; i <= num_chunks; ++i) {
        if (adaptive) {
            int32_t pred_t = (i > 1) ? static_cast<int32_t>(chunk_totals[i - 1]) : 0;
            chunk_totals[i] = static_cast<uint32_t>(ic.decompress(pred_t, 0));
        }
        int32_t pred_s = (i > 1) ? static_cast<int32_t>(chunk_bytes[i - 1]) : 0;
        chunk_bytes[i] = static_cast<uint32_t>(ic.decompress(pred_s, 1));
    }

    // Pass 2: kumulera till absoluta positioner.
    //
    // De ENSKILDA längderna är uint32 — det är formatet — men SUMMAN är det
    // inte. Ackumuleringen gjordes tidigare i uint32 och wrappade när den
    // komprimerade punktdatan passerade 4 GiB, vilket gav chunk-positioner
    // långt fram i filen som pekade i början. Det märktes inte förrän vi fick
    // en egen skrivare som kan producera sådana filer.
    chunk_offsets_.resize(num_chunks);
    uint64_t running = 0;
    for (uint32_t i = 0; i < num_chunks; ++i) {
        chunk_offsets_[i] = data_start + running;
        running += chunk_bytes[i + 1];
        // Punktdatan slutar där chunk-tabellen börjar, inte där filen gör.
        // Med file_size som gräns kan korrupta längder löpa in i tabellen och
        // ändå godtas.
        if (data_start + running > static_cast<uint64_t>(ct_start))
            return ErrorCode::CorruptVlr;
    }

    return ErrorCode::Ok;
}

ErrorCode LazReader::decompress_all(const uint8_t* file_buffer, size_t file_size,
                                    uint64_t offset_to_point_data,
                                    const LazVlrData& laz) {
    if (point_count_ == 0) {
        decompressed_.clear();
        return ErrorCode::Ok;
    }

    decompressed_.resize(point_count_ * record_length_);

    // Determine which item decoders are needed
    bool has_gps = false, has_rgb = false, has_byte = false;
    uint16_t byte_size = 0;
    uint16_t point10_size = 0;
    uint16_t gps_size = 0;
    uint16_t rgb_size = 0;

    bool has_point10 = false;
    for (const auto& item : laz.items) {
        switch (item.type) {
            case LazItemType::Point10:  has_point10 = true; point10_size = item.size; break;
            case LazItemType::GPSTime11: has_gps = true; gps_size = item.size; break;
            case LazItemType::RGB12:    has_rgb = true; rgb_size = item.size; break;
            case LazItemType::Byte:     has_byte = true; byte_size = item.size; break;
            default:
                // Ett OKÄNT item fick tidigare bara passera. Items delar EN
                // aritmetisk ström och ligger efter varandra i posten, så ett
                // item vi inte avkodar flyttar allt som kommer efter det —
                // resultatet blir en fil som läses UTAN FEL men till skräp.
                // Det gäller i praktiken Wavepacket13, alltså punktformat 4
                // och 5, som vi varken kan läsa eller skriva.
                return ErrorCode::LazNotSupported;
        }
    }

    // Samma kontroller som layout_from_vlr gör för det lagerindelade formatet.
    // Storlekarna kommer ur filen men avkodarna skriver fasta antal byte, så en
    // trasig VLR kan annars få en avkodad post att spilla ur sin plats.
    if (!has_point10 || point10_size != 20)
        return ErrorCode::LazNotSupported;
    if (has_gps && gps_size != 8)
        return ErrorCode::CorruptVlr;
    if (has_rgb && rgb_size != 6)
        return ErrorCode::CorruptVlr;

    const uint32_t item_total = uint32_t(point10_size)
                              + (has_gps ? gps_size : 0)
                              + (has_rgb ? rgb_size : 0)
                              + (has_byte ? byte_size : 0);
    if (item_total > record_length_)
        return ErrorCode::CorruptVlr;

    uint32_t chunk_size = laz.chunk_size;
    uint32_t num_chunks = static_cast<uint32_t>(chunk_offsets_.size());
    uint64_t points_remaining = point_count_;
    uint64_t out_offset = 0;

    for (uint32_t ci = 0; ci < num_chunks; ++ci) {
        uint32_t pts_in_chunk;
        if (laz.compressor == LazCompressor::Pointwise) {
            pts_in_chunk = static_cast<uint32_t>(point_count_);
        } else {
            pts_in_chunk = chunk_size;
            if (pts_in_chunk > points_remaining)
                pts_in_chunk = static_cast<uint32_t>(points_remaining);
        }

        if (pts_in_chunk == 0)
            break;

        uint64_t chunk_pos = chunk_offsets_[ci];
        if (chunk_pos >= file_size)
            return ErrorCode::ReadPastEnd;

        const uint8_t* ptr = file_buffer + chunk_pos;
        size_t remaining = file_size - static_cast<size_t>(chunk_pos);

        // Read uncompressed seed record (first point per chunk)
        uint16_t seed_size = record_length_;
        if (remaining < seed_size)
            return ErrorCode::ReadPastEnd;

        // Split seed into item parts
        const uint8_t* seed_ptr = ptr;
        uint16_t seed_offset = 0;

        // Offsets into the uncompressed seed
        const uint8_t* seed_point10 = seed_ptr + seed_offset;
        seed_offset += point10_size;

        const uint8_t* seed_gps = has_gps ? (seed_ptr + seed_offset) : nullptr;
        if (has_gps) seed_offset += gps_size;

        const uint8_t* seed_rgb = has_rgb ? (seed_ptr + seed_offset) : nullptr;
        if (has_rgb) seed_offset += rgb_size;

        const uint8_t* seed_byte = has_byte ? (seed_ptr + seed_offset) : nullptr;
        if (has_byte) seed_offset += byte_size;

        ptr += seed_size;
        remaining -= seed_size;

        // Initialize arithmetic decoder and item decoders
        ArithmeticDecoder dec;
        dec.init(ptr, remaining);

        Point10Decoder p10;
        p10.init(dec);

        GPSTime11Decoder gps_dec;
        if (has_gps) gps_dec.init(dec);

        RGB12Decoder rgb_dec;
        if (has_rgb) rgb_dec.init(dec);

        ByteDecoder byte_dec;
        if (has_byte) byte_dec.init(dec, byte_size);

        // Decompress each point in chunk
        for (uint32_t pi = 0; pi < pts_in_chunk; ++pi) {
            uint8_t* out = decompressed_.data() + out_offset;
            uint16_t off = 0;

            if (pi == 0) {
                // First point: use seed
                p10.decompress(seed_point10, out + off);
            } else {
                p10.decompress(nullptr, out + off);
            }
            off += point10_size;

            if (has_gps) {
                if (pi == 0)
                    gps_dec.decompress(seed_gps, out + off);
                else
                    gps_dec.decompress(nullptr, out + off);
                off += gps_size;
            }

            if (has_rgb) {
                if (pi == 0)
                    rgb_dec.decompress(seed_rgb, out + off);
                else
                    rgb_dec.decompress(nullptr, out + off);
                off += rgb_size;
            }

            if (has_byte) {
                if (pi == 0)
                    byte_dec.decompress(seed_byte, out + off);
                else
                    byte_dec.decompress(nullptr, out + off);
                off += byte_size;
            }

            out_offset += record_length_;
        }

        points_remaining -= pts_in_chunk;
    }

    // Utan den här kan en chunk-tabell med FÖR FÅ chunkar ge Ok med rätt prefix
    // och en nollfylld svans — alltså en fil som läses utan fel men saknar
    // punkter. Den lagerindelade vägen har haft kontrollen sedan den skrevs;
    // den här hade den inte.
    if (points_remaining != 0 || out_offset != decompressed_.size())
        return ErrorCode::CorruptChunk;

    return ErrorCode::Ok;
}

ErrorCode LazReader::decompress_all_layered(const uint8_t* file_buffer, size_t file_size,
                                             uint64_t offset_to_point_data,
                                             const LazVlrData& laz) {
    (void)offset_to_point_data;

    if (point_count_ == 0) {
        decompressed_.clear();
        return ErrorCode::Ok;
    }

    ErrorCode ec = layout_from_vlr(laz);
    if (ec != ErrorCode::Ok)
        return ec;

    decompressed_.resize(point_count_ * record_length_);

    const uint32_t num_chunks = static_cast<uint32_t>(chunk_offsets_.size());
    uint64_t out_offset = 0;

    for (uint32_t ci = 0; ci < num_chunks; ++ci) {
        const uint64_t chunk_pos = chunk_offsets_[ci];
        if (chunk_pos >= file_size)
            return ErrorCode::ReadPastEnd;

        // Chunkens längd: fram till nästa chunk, eller filslut för den sista.
        const uint64_t chunk_end = (ci + 1 < num_chunks) ? chunk_offsets_[ci + 1]
                                                         : file_size;
        if (chunk_end <= chunk_pos || chunk_end > file_size)
            return ErrorCode::ReadPastEnd;

        uint32_t written = 0;
        ec = decompress_chunk(file_buffer + chunk_pos,
                              static_cast<size_t>(chunk_end - chunk_pos),
                              /*expected_points=*/0,
                              decompressed_.data() + out_offset,
                              decompressed_.size() - out_offset,
                              &written);
        if (ec != ErrorCode::Ok)
            return ec;

        out_offset += static_cast<uint64_t>(written) * record_length_;
    }

    // För få chunkar, eller chunkar med för få punkter, skulle annars ge Ok med
    // en nollfylld svans i stället för ett fel.
    if (out_offset != decompressed_.size())
        return ErrorCode::CorruptChunk;

    return ErrorCode::Ok;
}

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
