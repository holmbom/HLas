#ifdef HLAS_HAVE_LAZ
#include "rgb12.h"
#include <cstring>

namespace HLas::detail::laz {

// Byte fold and clamp, as the LAZ format defines them
static inline uint8_t u8_fold(int32_t n) {
    if (n < 0) return static_cast<uint8_t>(n + 256);
    if (n > 255) return static_cast<uint8_t>(n - 256);
    return static_cast<uint8_t>(n);
}

static inline uint8_t u8_clamp(int32_t n) {
    if (n <= 0) return 0;
    if (n >= 255) return 255;
    return static_cast<uint8_t>(n);
}

void RGB12Decoder::init(ArithmeticDecoder& dec) {
    dec_ = &dec;
    first_ = true;

    changed_model_.init(128);
    red_lo_model_.init(256);
    red_hi_model_.init(256);
    green_lo_model_.init(256);
    green_hi_model_.init(256);
    blue_lo_model_.init(256);
    blue_hi_model_.init(256);

    std::memset(prev_, 0, sizeof(prev_));
}

void RGB12Decoder::decompress(const uint8_t* seed, uint8_t* dest) {
    if (first_) {
        std::memcpy(prev_, seed, 6);
        std::memcpy(dest, seed, 6);
        first_ = false;
        return;
    }

    // prev_ stores last_item as 3 x uint16_t (R, G, B) in little-endian
    uint16_t last_r, last_g, last_b;
    std::memcpy(&last_r, prev_, 2);
    std::memcpy(&last_g, prev_ + 2, 2);
    std::memcpy(&last_b, prev_ + 4, 2);

    // Decode changed values (7 bits → 128 symbols)
    uint32_t sym = dec_->decodeSymbol(changed_model_);

    // item is built as uint16_t[3]
    uint16_t item[3];

    // Bit 0: Red low byte
    if (sym & (1 << 0)) {
        uint8_t corr = static_cast<uint8_t>(dec_->decodeSymbol(red_lo_model_));
        item[0] = static_cast<uint16_t>(u8_fold(corr + (last_r & 0xFF)));
    } else {
        item[0] = last_r & 0xFF;
    }

    // Bit 1: Red high byte
    if (sym & (1 << 1)) {
        uint8_t corr = static_cast<uint8_t>(dec_->decodeSymbol(red_hi_model_));
        item[0] |= static_cast<uint16_t>(u8_fold(corr + (last_r >> 8))) << 8;
    } else {
        item[0] |= last_r & 0xFF00;
    }

    if (sym & (1 << 6)) {
        // Bit 6 SET: G and B decoded separately with prediction from R diff
        int32_t diff;

        // --- Low bytes ---
        diff = static_cast<int32_t>(item[0] & 0xFF) - static_cast<int32_t>(last_r & 0xFF);

        // Bit 2: Green low byte
        if (sym & (1 << 2)) {
            uint8_t corr = static_cast<uint8_t>(dec_->decodeSymbol(green_lo_model_));
            item[1] = static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_g & 0xFF))));
        } else {
            item[1] = last_g & 0xFF;
        }

        // Bit 4: Blue low byte (uses average of red and green diffs)
        if (sym & (1 << 4)) {
            uint8_t corr = static_cast<uint8_t>(dec_->decodeSymbol(blue_lo_model_));
            int32_t green_diff = static_cast<int32_t>(item[1] & 0xFF) - static_cast<int32_t>(last_g & 0xFF);
            diff = (diff + green_diff) / 2;
            item[2] = static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_b & 0xFF))));
        } else {
            item[2] = last_b & 0xFF;
        }

        // --- High bytes ---
        diff = static_cast<int32_t>(item[0] >> 8) - static_cast<int32_t>(last_r >> 8);

        // Bit 3: Green high byte
        if (sym & (1 << 3)) {
            uint8_t corr = static_cast<uint8_t>(dec_->decodeSymbol(green_hi_model_));
            item[1] |= static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_g >> 8)))) << 8;
        } else {
            item[1] |= last_g & 0xFF00;
        }

        // Bit 5: Blue high byte (uses average of red and green diffs)
        if (sym & (1 << 5)) {
            uint8_t corr = static_cast<uint8_t>(dec_->decodeSymbol(blue_hi_model_));
            int32_t green_diff = static_cast<int32_t>(item[1] >> 8) - static_cast<int32_t>(last_g >> 8);
            diff = (diff + green_diff) / 2;
            item[2] |= static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_b >> 8)))) << 8;
        } else {
            item[2] |= last_b & 0xFF00;
        }
    } else {
        // Bit 6 NOT set: G = B = R (shortcut)
        item[1] = item[0];
        item[2] = item[0];
    }

    std::memcpy(dest, item, 6);
    std::memcpy(prev_, item, 6);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
