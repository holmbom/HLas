#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include <cstdint>

namespace HLas::detail::laz {

// RGB12 decoder — Red, Green, Blue (6 bytes) for LAS formats 2, 3, 5 (spec 13.3)
class RGB12Decoder {
public:
    void init(ArithmeticDecoder& dec);
    void decompress(const uint8_t* seed, uint8_t* dest);

private:
    ArithmeticDecoder* dec_ = nullptr;

    // Changed values: 128 symbols (7 bits)
    SymbolModel changed_model_;

    // dRed/dGreen/dBlue (low and high): 256 symbols each
    SymbolModel red_lo_model_;
    SymbolModel red_hi_model_;
    SymbolModel green_lo_model_;
    SymbolModel green_hi_model_;
    SymbolModel blue_lo_model_;
    SymbolModel blue_hi_model_;

    uint8_t prev_[6] = {};
    bool first_ = true;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
