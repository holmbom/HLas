#ifdef HLAS_HAVE_LAZ
#include "rgb12_encoder.h"
#include <cassert>
#include <cstring>

namespace HLas::detail::laz {

namespace {

// Formatets klämning till en byte. Avkodaren har samma funktion; den behöver dessutom
// U8_FOLD, som är motstycket till att kodaren skickar korrektorn som en uint8.
uint8_t u8_clamp(int32_t n) {
    if (n <= 0) return 0;
    if (n >= 255) return 255;
    return static_cast<uint8_t>(n);
}

// Vilka av de sex halvbyten som ändrats, plus bit 6: är R, G och B INTE lika
// hos den nya punkten måste grönt och blått kodas var för sig. Är de lika
// räcker rött, och avkodaren kopierar det till de andra två.
uint32_t color_diff_bits(const uint16_t* a, const uint16_t* last) {
    auto d = [](uint16_t x, uint16_t y, uint16_t mask) -> uint32_t {
        return ((x ^ y) & mask) != 0;
    };
    return (d(last[0], a[0], 0x00FF) << 0)
         | (d(last[0], a[0], 0xFF00) << 1)
         | (d(last[1], a[1], 0x00FF) << 2)
         | (d(last[1], a[1], 0xFF00) << 3)
         | (d(last[2], a[2], 0x00FF) << 4)
         | (d(last[2], a[2], 0xFF00) << 5)
         | ((d(a[0], a[1], 0x00FF) || d(a[0], a[2], 0x00FF) ||
             d(a[0], a[1], 0xFF00) || d(a[0], a[2], 0xFF00)) << 6);
}

} // namespace

void RGB12Encoder::init(ArithmeticEncoder& enc) {
    enc_ = &enc;
    first_ = true;
    failed_ = false;

    changed_model_.init(128);
    red_lo_model_.init(256);
    red_hi_model_.init(256);
    green_lo_model_.init(256);
    green_hi_model_.init(256);
    blue_lo_model_.init(256);
    blue_hi_model_.init(256);

    prev_[0] = prev_[1] = prev_[2] = 0;
}

void RGB12Encoder::start_chunk(const uint8_t* seed) {
    assert(enc_ != nullptr);
    if (!enc_) { failed_ = true; return; }
    assert(first_);
    if (!first_) { failed_ = true; return; }

    std::memcpy(prev_, seed, 6);
    first_ = false;
}

void RGB12Encoder::encode(const uint8_t* item) {
    if (first_) { failed_ = true; return; }
    if (failed()) return;

    uint16_t v[3];
    std::memcpy(v, item, 6);

    const uint32_t sym = color_diff_bits(v, prev_);
    enc_->encodeSymbol(changed_model_, sym);

    int32_t diff_l = 0, diff_h = 0;

    if (sym & (1u << 0)) {
        diff_l = static_cast<int32_t>(v[0] & 0xFF) - static_cast<int32_t>(prev_[0] & 0xFF);
        enc_->encodeSymbol(red_lo_model_, static_cast<uint8_t>(diff_l));
    }
    if (sym & (1u << 1)) {
        diff_h = static_cast<int32_t>(v[0] >> 8) - static_cast<int32_t>(prev_[0] >> 8);
        enc_->encodeSymbol(red_hi_model_, static_cast<uint8_t>(diff_h));
    }

    if (sym & (1u << 6)) {
        // Ordningen är låg-grön, låg-blå, hög-grön, hög-blå — inte grön följt av
        // blå. Blått predikteras ur MEDELVÄRDET av rödens och grönens ändring,
        // så grönt måste vara känt först inom varje halva.
        if (sym & (1u << 2)) {
            const int32_t corr = static_cast<int32_t>(v[1] & 0xFF)
                               - u8_clamp(diff_l + static_cast<int32_t>(prev_[1] & 0xFF));
            enc_->encodeSymbol(green_lo_model_, static_cast<uint8_t>(corr));
        }
        if (sym & (1u << 4)) {
            diff_l = (diff_l + static_cast<int32_t>(v[1] & 0xFF)
                             - static_cast<int32_t>(prev_[1] & 0xFF)) / 2;
            const int32_t corr = static_cast<int32_t>(v[2] & 0xFF)
                               - u8_clamp(diff_l + static_cast<int32_t>(prev_[2] & 0xFF));
            enc_->encodeSymbol(blue_lo_model_, static_cast<uint8_t>(corr));
        }
        if (sym & (1u << 3)) {
            const int32_t corr = static_cast<int32_t>(v[1] >> 8)
                               - u8_clamp(diff_h + static_cast<int32_t>(prev_[1] >> 8));
            enc_->encodeSymbol(green_hi_model_, static_cast<uint8_t>(corr));
        }
        if (sym & (1u << 5)) {
            diff_h = (diff_h + static_cast<int32_t>(v[1] >> 8)
                             - static_cast<int32_t>(prev_[1] >> 8)) / 2;
            const int32_t corr = static_cast<int32_t>(v[2] >> 8)
                               - u8_clamp(diff_h + static_cast<int32_t>(prev_[2] >> 8));
            enc_->encodeSymbol(blue_hi_model_, static_cast<uint8_t>(corr));
        }
    }

    std::memcpy(prev_, v, 6);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
