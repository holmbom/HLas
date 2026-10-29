#ifdef HLAS_HAVE_LAZ
#include "rgb14_encoder.h"
#include <cassert>
#include <cstring>

namespace HLas::detail::laz {

namespace {

// Samma klämning som avkodarens u8_clamp. Prediktionen kan hamna utanför
// [0,255] och måste då dras in, annars blir korrektionen inte den avkodaren
// räknar med.
inline uint8_t u8_clamp(int32_t n) {
    if (n <= 0) return 0;
    if (n >= 255) return 255;
    return static_cast<uint8_t>(n);
}

inline uint16_t get_u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

// Vilka halvbyte som skiljer, plus bit 6: "grönt eller blått skiljer sig från
// rött i den NYA punkten". Är bit 6 släckt kodas bara rött och avkodaren sätter
// G = B = R.
uint32_t color_diff_bits(const uint16_t* now, const uint16_t* last) {
    auto differs = [](uint16_t a, uint16_t b, uint16_t mask) -> uint32_t {
        return ((a ^ b) & mask) ? 1u : 0u;
    };
    return (differs(last[0], now[0], 0x00FF) << 0)
         | (differs(last[0], now[0], 0xFF00) << 1)
         | (differs(last[1], now[1], 0x00FF) << 2)
         | (differs(last[1], now[1], 0xFF00) << 3)
         | (differs(last[2], now[2], 0x00FF) << 4)
         | (differs(last[2], now[2], 0xFF00) << 5)
         | ((differs(now[0], now[1], 0x00FF) | differs(now[0], now[2], 0x00FF) |
             differs(now[0], now[1], 0xFF00) | differs(now[0], now[2], 0xFF00)) << 6);
}

} // namespace

void RGB14Encoder::init_context(uint32_t ctx, const uint16_t* seed) {
    auto& c = contexts_[ctx];
    c.unused = false;
    std::memcpy(c.last_item, seed, 6);
    c.m_byte_used.init(128);
    for (auto& m : c.m_rgb_diff) m.init(256);
}

void RGB14Encoder::start_chunk(const uint8_t* seed_rgb, uint32_t context) {
    assert(!in_chunk_);
    if (in_chunk_) { failed_ = true; return; }

    enc_.init();
    for (auto& c : contexts_) c.unused = true;
    current_context_ = context & 3u;

    uint16_t seed[3];
    std::memcpy(seed, seed_rgb, 6);
    init_context(current_context_, seed);

    valid_ = false;
    in_chunk_ = true;
    failed_ = false;
}

void RGB14Encoder::encode(uint32_t context, const uint8_t* item) {
    assert(in_chunk_);
    if (!in_chunk_ || failed_) { failed_ = true; return; }

    const uint32_t sc = context & 3u;
    RGB14Context& c = contexts_[sc];

    // SPEGLAR ETT FEL I FORMATET, avsiktligt.
    //
    // `last` pekar på FÖREGÅENDE kanals färg, inte på den här kanalens — och
    // ändras bara om kanalen är oanvänd. Byter man till en kanal som redan
    // använts fortsätter alltså både prediktionen och tillbakaskrivningen att
    // gå mot den andra kanalens färg.
    //
    // Det är formatets beteende i v3. Att göra det "rätt" här skulle
    // producera filer som inga andra läsare tolkar likadant.
    uint16_t* last = contexts_[current_context_].last_item;
    if (c.unused) {
        init_context(sc, last);
        last = c.last_item;
    }

    uint16_t now[3];
    std::memcpy(now, item, 6);

    const uint32_t sym = color_diff_bits(now, last);
    if (sym)
        valid_ = true;
    enc_.encodeSymbol(c.m_byte_used, sym);

    int32_t diff_l = 0, diff_h = 0;

    if (sym & (1u << 0)) {
        diff_l = static_cast<int32_t>(now[0] & 0xFF) - static_cast<int32_t>(last[0] & 0xFF);
        enc_.encodeSymbol(c.m_rgb_diff[0], static_cast<uint8_t>(diff_l));
    }
    if (sym & (1u << 1)) {
        diff_h = static_cast<int32_t>(now[0] >> 8) - static_cast<int32_t>(last[0] >> 8);
        enc_.encodeSymbol(c.m_rgb_diff[1], static_cast<uint8_t>(diff_h));
    }

    if (sym & (1u << 6)) {
        // Ordningen är avkodarens: låga byten för grönt och blått, sedan de
        // höga. Blått predikteras ur MEDELVÄRDET av rödens och grönens ändring,
        // och därför måste grönt kodas först.
        if (sym & (1u << 2)) {
            const int32_t corr = static_cast<int32_t>(now[1] & 0xFF)
                               - u8_clamp(diff_l + static_cast<int32_t>(last[1] & 0xFF));
            enc_.encodeSymbol(c.m_rgb_diff[2], static_cast<uint8_t>(corr));
        }
        if (sym & (1u << 4)) {
            diff_l = (diff_l + static_cast<int32_t>(now[1] & 0xFF)
                             - static_cast<int32_t>(last[1] & 0xFF)) / 2;
            const int32_t corr = static_cast<int32_t>(now[2] & 0xFF)
                               - u8_clamp(diff_l + static_cast<int32_t>(last[2] & 0xFF));
            enc_.encodeSymbol(c.m_rgb_diff[4], static_cast<uint8_t>(corr));
        }
        if (sym & (1u << 3)) {
            const int32_t corr = static_cast<int32_t>(now[1] >> 8)
                               - u8_clamp(diff_h + static_cast<int32_t>(last[1] >> 8));
            enc_.encodeSymbol(c.m_rgb_diff[3], static_cast<uint8_t>(corr));
        }
        if (sym & (1u << 5)) {
            diff_h = (diff_h + static_cast<int32_t>(now[1] >> 8)
                             - static_cast<int32_t>(last[1] >> 8)) / 2;
            const int32_t corr = static_cast<int32_t>(now[2] >> 8)
                               - u8_clamp(diff_h + static_cast<int32_t>(last[2] >> 8));
            enc_.encodeSymbol(c.m_rgb_diff[5], static_cast<uint8_t>(corr));
        }
    }

    // Skrivs tillbaka dit `last` pekar — se kommentaren ovan.
    std::memcpy(last, now, 6);
    current_context_ = sc;

    if (enc_.failed())
        failed_ = true;
}

void RGB14Encoder::finish_chunk() {
    assert(in_chunk_);
    if (!in_chunk_) { failed_ = true; return; }
    in_chunk_ = false;

    enc_.done();
    if (enc_.failed())
        failed_ = true;
    if (enc_.size() > 0xFFFFFFFFull)
        failed_ = true;
}

void RGB14Encoder::append_size(std::vector<uint8_t>& out) const {
    const uint32_t n = (failed_ || !valid_) ? 0u : static_cast<uint32_t>(enc_.size());
    out.push_back(static_cast<uint8_t>(n));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n >> 16));
    out.push_back(static_cast<uint8_t>(n >> 24));
}

void RGB14Encoder::append_data(std::vector<uint8_t>& out) const {
    if (failed_ || !valid_)
        return;
    out.insert(out.end(), enc_.bytes().begin(), enc_.bytes().end());
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
