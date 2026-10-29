#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// Per-scanner-channel context for RGB14
struct RGB14Context {
    bool unused = true;
    uint16_t last_item[3] = {};  // R, G, B
    SymbolModel m_byte_used;     // 128 symbols
    SymbolModel m_rgb_diff[6];   // red_lo, red_hi, green_lo, green_hi, blue_lo, blue_hi
};

// RGB14 v3/v4 decoder (LAS 1.4, LayerChunked)
class RGB14Decoder {
public:
    void read_chunk_sizes(const uint8_t*& ptr);

    static constexpr uint64_t kChunkSizeHeaderBytes = 4;
    uint64_t declared_layer_bytes() const { return num_bytes_RGB_; }
    void init_chunk(const uint8_t*& ptr, const uint8_t* seed, uint32_t context);
    void decompress(uint32_t context, uint8_t* dest);

private:
    void init_context(uint32_t ctx, const uint16_t* seed_item);

    uint32_t num_bytes_RGB_ = 0;
    std::vector<uint8_t> layer_bytes_;
    ArithmeticDecoder dec_RGB_;
    bool changed_RGB_ = false;
    uint32_t current_context_ = 0;
    RGB14Context contexts_[4];
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
