#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// Per-scanner-channel context for Byte14
struct Byte14Context {
    bool unused = true;
    std::vector<uint8_t> last_item;
    std::vector<SymbolModel> m_bytes; // one model per byte, 256 symbols each
};

// Byte14 v3/v4 decoder (extra bytes for LAS 1.4, LayerChunked)
class Byte14Decoder {
public:
    void read_chunk_sizes(const uint8_t*& ptr);

    static constexpr uint64_t kChunkSizeHeaderBytes = 4;
    uint64_t declared_layer_bytes() const { return num_bytes_bytes_; }
    void init_chunk(const uint8_t*& ptr, const uint8_t* seed,
                    uint16_t num_extra_bytes, uint32_t context);
    void decompress(uint32_t context, uint8_t* dest);

private:
    void init_context(uint32_t ctx, const uint8_t* seed_item);

    uint32_t num_bytes_bytes_ = 0;
    uint16_t num_extra_bytes_ = 0;
    std::vector<uint8_t> layer_bytes_;
    ArithmeticDecoder dec_bytes_;
    bool changed_bytes_ = false;
    uint32_t current_context_ = 0;
    Byte14Context contexts_[4];
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
