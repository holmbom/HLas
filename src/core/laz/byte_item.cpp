#ifdef HLAS_HAVE_LAZ
#include "byte_item.h"
#include <cstring>

namespace HLas::detail::laz {

void ByteDecoder::init(ArithmeticDecoder& dec, uint16_t num_bytes) {
    dec_ = &dec;
    num_bytes_ = num_bytes;
    first_ = true;

    models_.resize(num_bytes);
    for (auto& m : models_) m.init(256);

    prev_.assign(num_bytes, 0);
}

void ByteDecoder::decompress(const uint8_t* seed, uint8_t* dest) {
    if (first_) {
        std::memcpy(dest, seed, num_bytes_);
        std::memcpy(prev_.data(), seed, num_bytes_);
        first_ = false;
        return;
    }

    for (uint16_t i = 0; i < num_bytes_; ++i) {
        uint8_t d = static_cast<uint8_t>(dec_->decodeSymbol(models_[i]));
        dest[i] = static_cast<uint8_t>((prev_[i] + d) & 0xFF);
    }

    std::memcpy(prev_.data(), dest, num_bytes_);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
