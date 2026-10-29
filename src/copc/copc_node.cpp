#ifdef HLAS_HAVE_LAZ
#include "copc_node.h"
#include <cstdint>

namespace HLas::detail {

ErrorCode copc_node_range(const CopcEntry& node, uint64_t src_size,
                          uint64_t& out_offset, uint64_t& out_size) {
    if (node.is_subpage())
        return ErrorCode::CorruptEvlr;   // en undersida är index, inte punkter
    if (node.point_count <= 0 || node.byte_size <= 0)
        return ErrorCode::CorruptEvlr;

    const uint64_t byte_size = static_cast<uint64_t>(node.byte_size);
    if (byte_size > kCopcMaxNodeBytes)
        return ErrorCode::CorruptEvlr;
    if (static_cast<uint64_t>(node.point_count) > kCopcMaxNodePoints)
        return ErrorCode::CorruptEvlr;

    // Hela intervallet valideras innan vi allokerar — samma regel som för
    // hierarkin. ByteSource::read kontrollerar också, men då har vi redan
    // allokerat, och det är just det en elak källa vill få oss att göra.
    if (!ByteSource::range_ok(node.offset, byte_size, src_size))
        return ErrorCode::CorruptEvlr;

    out_offset = node.offset;
    out_size   = byte_size;
    return ErrorCode::Ok;
}

ErrorCode decompress_copc_node(const LazReader& laz, const CopcEntry& node,
                               const uint8_t* chunk, size_t chunk_len,
                               std::vector<uint8_t>& out) {
    out.clear();
    if (!chunk) return ErrorCode::CorruptChunk;

    const uint64_t need = static_cast<uint64_t>(node.point_count) * laz.record_length();
#if SIZE_MAX < UINT64_MAX
    if (need > static_cast<uint64_t>(SIZE_MAX))
        return ErrorCode::CorruptEvlr;
#endif
    out.resize(static_cast<size_t>(need));

    uint32_t written = 0;
    ErrorCode ec = laz.decompress_chunk(chunk, chunk_len,
                                        static_cast<uint32_t>(node.point_count),
                                        out.data(), out.size(), &written);
    if (ec != ErrorCode::Ok) {
        out.clear();
        return ec;
    }
    if (written != static_cast<uint32_t>(node.point_count)) {
        out.clear();
        return ErrorCode::CorruptChunk;
    }
    return ErrorCode::Ok;
}

ErrorCode read_copc_node(ByteSource& src, const LazReader& laz,
                         const CopcEntry& node, std::vector<uint8_t>& out) {
    out.clear();

    uint64_t off = 0, len = 0;
    ErrorCode ec = copc_node_range(node, src.size(), off, len);
    if (ec != ErrorCode::Ok)
        return ec;

    std::vector<uint8_t> chunk(static_cast<size_t>(len));
    if (!src.read(off, chunk.size(), chunk.data()))
        return ErrorCode::ReadPastEnd;

    return decompress_copc_node(laz, node, chunk.data(), chunk.size(), out);
}

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
