#ifdef HLAS_HAVE_LAZ
#include "byte14.h"
#include <cstring>

namespace HLas::detail::laz {

void Byte14Decoder::read_chunk_sizes(const uint8_t*& ptr) {
    std::memcpy(&num_bytes_bytes_, ptr, 4);
    ptr += 4;
}

void Byte14Decoder::init_context(uint32_t ctx, const uint8_t* seed_item) {
    auto& c = contexts_[ctx];
    c.unused = false;
    c.last_item.resize(num_extra_bytes_);
    std::memcpy(c.last_item.data(), seed_item, num_extra_bytes_);
    c.m_bytes.resize(num_extra_bytes_);
    for (auto& m : c.m_bytes) m.init(256);
}

void Byte14Decoder::init_chunk(const uint8_t*& ptr, const uint8_t* seed,
                                uint16_t num_extra_bytes, uint32_t context) {
    num_extra_bytes_ = num_extra_bytes;

    if (num_bytes_bytes_ > 0) {
        layer_bytes_.resize(num_bytes_bytes_);
        std::memcpy(layer_bytes_.data(), ptr, num_bytes_bytes_);
        ptr += num_bytes_bytes_;
        dec_bytes_.init(layer_bytes_.data(), num_bytes_bytes_);
        changed_bytes_ = true;
    } else {
        changed_bytes_ = false;
    }

    for (auto& c : contexts_) c.unused = true;
    current_context_ = context;
    init_context(current_context_, seed);
}

void Byte14Decoder::decompress(uint32_t context, uint8_t* dest) {
    auto* last_item = contexts_[current_context_].last_item.data();

    if (current_context_ != context) {
        current_context_ = context;
        if (contexts_[current_context_].unused) {
            init_context(current_context_, last_item);
        }
        last_item = contexts_[current_context_].last_item.data();
    }

    if (changed_bytes_) {
        auto& ctx = contexts_[current_context_];
        for (uint16_t i = 0; i < num_extra_bytes_; ++i) {
            uint32_t diff = dec_bytes_.decodeSymbol(ctx.m_bytes[i]);
            last_item[i] = static_cast<uint8_t>((last_item[i] + diff) & 0xFF);
        }
        std::memcpy(dest, last_item, num_extra_bytes_);
    } else {
        std::memcpy(dest, last_item, num_extra_bytes_);
    }
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
