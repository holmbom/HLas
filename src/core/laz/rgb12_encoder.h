#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic_encoder.h"
#include <cstdint>

namespace HLas::detail::laz {

// RGB12-KODARE — spegel av RGB12Decoder, för LAS punktformat 2, 3, 5.
//
// Enklare än RGB14Encoder på en punkt: inga scannerkanaler, alltså inget
// kontextbyte och inget av det egendomliga beteende som RGB14 måste härma.
// Modellen är annars densamma: en sjubitars mask säger vilka av de sex
// bytehalvorna — låg och hög för R, G och B — som ändrats, grönt och blått
// predikteras ur rödens ändring, och är R, G och B lika kodas bara rött. Det är
// bit 6 i masken. Mätt på de gyllene vektorerna kostar en grå chunk 282 byte mot
// 414 för en färgad, alltså 68 %.
class RGB12Encoder {
public:
    void init(ArithmeticEncoder& enc);

    // Chunkens första färg. De sex byten skrivs RÅTT av anroparen.
    void start_chunk(const uint8_t* seed);

    // `item` är sex byte: R, G, B som uint16 var.
    void encode(const uint8_t* item);

    bool failed() const { return failed_ || (enc_ && enc_->failed()); }

private:
    ArithmeticEncoder* enc_ = nullptr;

    SymbolModel changed_model_;    // 128 symboler
    SymbolModel red_lo_model_;
    SymbolModel red_hi_model_;
    SymbolModel green_lo_model_;
    SymbolModel green_hi_model_;
    SymbolModel blue_lo_model_;
    SymbolModel blue_hi_model_;

    uint16_t prev_[3] = {};
    bool     first_ = true;
    bool     failed_ = false;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
