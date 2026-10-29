#include "vlr_parser.h"
#include <cstring>

namespace HLas::detail {

ErrorCode parse_vlrs(const uint8_t* buffer, size_t buffer_size,
                     size_t offset, uint32_t count,
                     std::vector<Vlr>& out) {
    out.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        if (offset + sizeof(VlrHeader) > buffer_size)
            return ErrorCode::CorruptVlr;

        VlrHeader hdr;
        std::memcpy(&hdr, buffer + offset, sizeof(hdr));
        offset += sizeof(VlrHeader);

        if (offset + hdr.record_length > buffer_size)
            return ErrorCode::CorruptVlr;

        Vlr vlr;
        std::memcpy(vlr.user_id, hdr.user_id, 16);
        vlr.record_id = hdr.record_id;
        vlr.data = buffer + offset;
        vlr.data_size = hdr.record_length;
        out.push_back(vlr);

        offset += hdr.record_length;
    }

    return ErrorCode::Ok;
}

ErrorCode parse_evlrs(const uint8_t* buffer, size_t buffer_size,
                      size_t offset, uint32_t count,
                      std::vector<Vlr>& out) {
    out.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        if (offset + sizeof(EvlrHeader) > buffer_size)
            return ErrorCode::CorruptEvlr;

        EvlrHeader hdr;
        std::memcpy(&hdr, buffer + offset, sizeof(hdr));
        offset += sizeof(EvlrHeader);

        if (offset + hdr.record_length > buffer_size)
            return ErrorCode::CorruptEvlr;

        Vlr vlr;
        std::memcpy(vlr.user_id, hdr.user_id, 16);
        vlr.record_id = hdr.record_id;
        vlr.data = buffer + offset;
        vlr.data_size = hdr.record_length;
        out.push_back(vlr);

        offset += hdr.record_length;
    }

    return ErrorCode::Ok;
}

const Vlr* find_vlr(const std::vector<Vlr>& vlrs,
                     const char* user_id, uint16_t record_id) {
    for (const auto& v : vlrs) {
        if (std::strncmp(v.user_id, user_id, 16) == 0 &&
            v.record_id == record_id)
            return &v;
    }
    return nullptr;
}

} // namespace HLas::detail
