#ifdef HLAS_HAVE_LAZ
#include "arithmetic.h"

namespace HLas::detail::laz {

// ============================================================
// SymbolModel  (spec 10.2)
// ============================================================

void SymbolModel::init(uint32_t symbols) {
    num_symbols = symbols;
    symbol_count.assign(symbols, 1);   // each count = 1 initially
    distribution.resize(symbols);

    update_distribution();             // initializes distribution[]

    // Set after update_distribution (spec 10.2.2)
    update_cycle = static_cast<uint16_t>((symbols + 6) / 2);
    symbols_until_update = update_cycle;
}

void SymbolModel::update_distribution() {
    // Halve counts when total > 2^15 (spec 10.2.3)
    uint32_t total = 0;
    for (uint32_t i = 0; i < num_symbols; ++i)
        total += symbol_count[i];

    if (total > DM_MAX_COUNT) {
        total = 0;
        for (uint32_t i = 0; i < num_symbols; ++i) {
            symbol_count[i] = static_cast<uint16_t>((symbol_count[i] + 1) / 2);
            total += symbol_count[i];
        }
    }

    // Recalculate distribution (cumulative scaled lower bounds)
    // scale = floor(2^31 / total_count)
    // distribution[n] = floor(scale * sum / 2^16)
    uint32_t scale = 0x80000000u / total;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < num_symbols; ++i) {
        distribution[i] = static_cast<uint16_t>((scale * sum) >> 16);
        sum += symbol_count[i];
    }

    // Update cycle growth (spec 10.2.3)
    update_cycle = static_cast<uint16_t>((5u * update_cycle) / 4u);
    uint16_t max_cycle = static_cast<uint16_t>(8u * (num_symbols + 6));
    if (update_cycle > max_cycle)
        update_cycle = max_cycle;
    symbols_until_update = update_cycle;
}

// ============================================================
// BitModel  (spec 10.3)
// ============================================================

void BitModel::init() {
    bit_0_count = 1;
    bit_count = 2;
    bit_0_prob = 4096;     // 4096/8192 = 0.5
    update_cycle = 4;
    bits_until_update = 4;
}

void BitModel::update_bit_distribution() {
    // spec 10.3.3
    bit_count += update_cycle;

    if (bit_count > BM_MAX_COUNT) {
        bit_count = static_cast<uint16_t>((bit_count + 1) / 2);
        bit_0_count = static_cast<uint16_t>((bit_0_count + 1) / 2);
        if (bit_0_count == bit_count)
            bit_count++;
    }

    // bit_0_prob = floor(floor(2^31 / bit_count) * bit_0_count / 2^18)
    uint32_t scale = 0x80000000u / bit_count;
    bit_0_prob = static_cast<uint16_t>((scale * bit_0_count) >> 18);

    // Update cycle growth, max 64
    update_cycle = static_cast<uint16_t>((5u * update_cycle) / 4u);
    if (update_cycle > 64)
        update_cycle = 64;
    bits_until_update = update_cycle;
}

// ============================================================
// ArithmeticDecoder  (spec 9.3.1, 9.4.1)
// ============================================================

void ArithmeticDecoder::init(const uint8_t* data, size_t size) {
    ptr_ = data;
    end_ = data + size;

    // Read first 4 bytes big-endian → value (spec 9.3.1)
    value_ = static_cast<uint32_t>(readByte()) << 24
           | static_cast<uint32_t>(readByte()) << 16
           | static_cast<uint32_t>(readByte()) << 8
           | static_cast<uint32_t>(readByte());

    length_ = AC_MAX_LENGTH;
}

void ArithmeticDecoder::renorm() {
    // spec 9.4.1: shift value left by 1 byte, append byte from stream
    // repeat until length >= 2^24
    do {
        value_ = (value_ << 8) | readByte();
        length_ <<= 8;
    } while (length_ < AC_MIN_LENGTH);
}

uint32_t ArithmeticDecoder::decodeSymbol(SymbolModel& m) {
    // spec 10.2.4
    uint32_t ltmp = length_ >> DM_LENGTH_SHIFT;

    // Find symbol: highest s where distribution[s] * ltmp <= value
    // Binary search for better performance
    uint32_t sym = 0;
    uint32_t lo = 0, hi = m.num_symbols;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (static_cast<uint32_t>(m.distribution[mid]) * ltmp <= value_)
            lo = mid;
        else
            hi = mid;
    }
    sym = lo;

    // Adjust value and length
    uint32_t lower = static_cast<uint32_t>(m.distribution[sym]) * ltmp;
    value_ -= lower;

    if (sym < m.num_symbols - 1) {
        length_ = static_cast<uint32_t>(m.distribution[sym + 1]) * ltmp - lower;
    } else {
        length_ = length_ - lower;
    }

    if (length_ < AC_MIN_LENGTH)
        renorm();

    // Update counts
    m.symbol_count[sym]++;
    if (--m.symbols_until_update == 0)
        m.update_distribution();

    return sym;
}

uint32_t ArithmeticDecoder::decodeBit(BitModel& m) {
    // spec 10.3.4
    uint32_t ltmp = length_ >> BM_LENGTH_SHIFT;
    uint32_t threshold = static_cast<uint32_t>(m.bit_0_prob) * ltmp;

    uint32_t bit;
    if (value_ >= threshold) {
        bit = 1;
        value_ -= threshold;
        length_ -= threshold;  // spec 10.3.4: length := length - threshold
    } else {
        bit = 0;
        length_ = threshold;
    }

    if (length_ < AC_MIN_LENGTH)
        renorm();

    if (bit == 0)
        m.bit_0_count++;

    if (--m.bits_until_update == 0)
        m.update_bit_distribution();

    return bit;
}

uint32_t ArithmeticDecoder::readBits(int bit_count) {
    // spec 10.4.2
    if (bit_count > 19) {
        uint32_t low = readBits(16);
        uint32_t high = readBits(bit_count - 16);
        return low | (high << 16);
    }

    uint32_t divisor = length_ >> bit_count;
    uint32_t raw = value_ / divisor;
    length_ = divisor;
    value_ -= length_ * raw;

    if (length_ < AC_MIN_LENGTH)
        renorm();

    return raw;
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
