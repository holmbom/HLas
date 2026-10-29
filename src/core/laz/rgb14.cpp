#ifdef HLAS_HAVE_LAZ
#include "rgb14.h"
#include <cstring>

namespace HLas::detail::laz {

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

void RGB14Decoder::read_chunk_sizes(const uint8_t*& ptr) {
    std::memcpy(&num_bytes_RGB_, ptr, 4);
    ptr += 4;
}

void RGB14Decoder::init_context(uint32_t ctx, const uint16_t* seed_item) {
    auto& c = contexts_[ctx];
    c.unused = false;
    std::memcpy(c.last_item, seed_item, 6);
    c.m_byte_used.init(128);
    for (auto& m : c.m_rgb_diff) m.init(256);
}

void RGB14Decoder::init_chunk(const uint8_t*& ptr, const uint8_t* seed, uint32_t context) {
    if (num_bytes_RGB_ > 0) {
        layer_bytes_.resize(num_bytes_RGB_);
        std::memcpy(layer_bytes_.data(), ptr, num_bytes_RGB_);
        ptr += num_bytes_RGB_;
        dec_RGB_.init(layer_bytes_.data(), num_bytes_RGB_);
        changed_RGB_ = true;
    } else {
        changed_RGB_ = false;
    }

    for (auto& c : contexts_) c.unused = true;
    current_context_ = context & 3u;
    uint16_t seed_rgb[3];
    std::memcpy(seed_rgb, seed, 6);
    init_context(current_context_, seed_rgb);
}

void RGB14Decoder::decompress(uint32_t context, uint8_t* dest) {
    // Tomt lager: färgen upprepas ur FÖREGÅENDE kanals tillstånd, och kanalen
    // byts inte. Kontrollen ligger före kanalhanteringen av just det skälet —
    // referensen returnerar här innan den rör last_channel_.
    if (!changed_RGB_) {
        std::memcpy(dest, contexts_[current_context_].last_item, 6);
        return;
    }

    auto& ctx = contexts_[context & 3u];

    // FORMATETS EGENDOMLIGHET, och den måste speglas.
    //
    // `last` pekar på FÖREGÅENDE kanals färg. Bara om den nya kanalen är
    // oanvänd flyttas pekaren till den egna. Byter man till en kanal som REDAN
    // använts fortsätter alltså både prediktionen och tillbakaskrivningen mot
    // den andra kanalens färg — medan MODELLERNA hämtas ur den nya kanalen.
    //
    // Det är formatets beteende i v3, och andra läsare tolkar filerna så.
    // HLas gjorde "rätt" fram till 1.1.2 och avkodade därför flerkanaliga
    // format 7-filer FEL. Hittat genom att skriva kodaren och jämföra mot en
    // gyllene vektor med både RGB och kanalbyten.
    uint16_t* last_item = contexts_[current_context_].last_item;
    if ((context & 3u) != current_context_) {
        current_context_ = context & 3u;
        if (ctx.unused) {
            uint16_t seed[3];
            std::memcpy(seed, last_item, 6);
            init_context(current_context_, seed);
            last_item = ctx.last_item;
        }
    }

    {
        uint32_t sym = dec_RGB_.decodeSymbol(ctx.m_byte_used);

        uint16_t item[3];

        // Bit 0: Red low byte
        if (sym & (1 << 0)) {
            uint8_t corr = static_cast<uint8_t>(dec_RGB_.decodeSymbol(ctx.m_rgb_diff[0]));
            item[0] = static_cast<uint16_t>(u8_fold(corr + (last_item[0] & 0xFF)));
        } else {
            item[0] = last_item[0] & 0xFF;
        }

        // Bit 1: Red high byte
        if (sym & (1 << 1)) {
            uint8_t corr = static_cast<uint8_t>(dec_RGB_.decodeSymbol(ctx.m_rgb_diff[1]));
            item[0] |= static_cast<uint16_t>(u8_fold(corr + (last_item[0] >> 8))) << 8;
        } else {
            item[0] |= last_item[0] & 0xFF00;
        }

        if (sym & (1 << 6)) {
            // Bit 6 SET: G and B decoded separately
            int32_t diff;

            // --- Low bytes ---
            diff = static_cast<int32_t>(item[0] & 0xFF) - static_cast<int32_t>(last_item[0] & 0xFF);

            // Bit 2: Green low
            if (sym & (1 << 2)) {
                uint8_t corr = static_cast<uint8_t>(dec_RGB_.decodeSymbol(ctx.m_rgb_diff[2]));
                item[1] = static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_item[1] & 0xFF))));
            } else {
                item[1] = last_item[1] & 0xFF;
            }

            // Bit 4: Blue low (average of red and green diffs)
            if (sym & (1 << 4)) {
                uint8_t corr = static_cast<uint8_t>(dec_RGB_.decodeSymbol(ctx.m_rgb_diff[4]));
                int32_t green_diff = static_cast<int32_t>(item[1] & 0xFF) - static_cast<int32_t>(last_item[1] & 0xFF);
                diff = (diff + green_diff) / 2;
                item[2] = static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_item[2] & 0xFF))));
            } else {
                item[2] = last_item[2] & 0xFF;
            }

            // --- High bytes ---
            diff = static_cast<int32_t>(item[0] >> 8) - static_cast<int32_t>(last_item[0] >> 8);

            // Bit 3: Green high
            if (sym & (1 << 3)) {
                uint8_t corr = static_cast<uint8_t>(dec_RGB_.decodeSymbol(ctx.m_rgb_diff[3]));
                item[1] |= static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_item[1] >> 8)))) << 8;
            } else {
                item[1] |= last_item[1] & 0xFF00;
            }

            // Bit 5: Blue high (average of red and green diffs)
            if (sym & (1 << 5)) {
                uint8_t corr = static_cast<uint8_t>(dec_RGB_.decodeSymbol(ctx.m_rgb_diff[5]));
                int32_t green_diff = static_cast<int32_t>(item[1] >> 8) - static_cast<int32_t>(last_item[1] >> 8);
                diff = (diff + green_diff) / 2;
                item[2] |= static_cast<uint16_t>(u8_fold(corr + u8_clamp(diff + (last_item[2] >> 8)))) << 8;
            } else {
                item[2] |= last_item[2] & 0xFF00;
            }
        } else {
            // Bit 6 NOT set: G = B = R
            item[1] = item[0];
            item[2] = item[0];
        }

        std::memcpy(dest, item, 6);
        // Skrivs tillbaka dit `last_item` pekar — se kommentaren ovan.
        std::memcpy(last_item, item, 6);
    }
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
