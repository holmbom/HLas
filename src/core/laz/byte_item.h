#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// BYTE item decoder — extra bytes appended to LAS formats 0-5 (spec 13.4)
class ByteDecoder {
public:
    void init(ArithmeticDecoder& dec, uint16_t num_bytes);
    void decompress(const uint8_t* seed, uint8_t* dest);

private:
    ArithmeticDecoder* dec_ = nullptr;
    uint16_t num_bytes_ = 0;

    // One 256-symbol model per byte
    std::vector<SymbolModel> models_;

    std::vector<uint8_t> prev_;
    bool first_ = true;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
