#pragma once
#ifdef HLAS_HAVE_LAZ

#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// Constants from LAZ spec sections 09-10
constexpr uint32_t AC_MIN_LENGTH = 0x01000000u;  // 2^24
constexpr uint32_t AC_MAX_LENGTH = 0xFFFFFFFFu;

// Symbol encoder constants (spec 10.2)
constexpr uint32_t DM_LENGTH_SHIFT = 15;
constexpr uint32_t DM_MAX_COUNT    = 1u << 15;  // 32768

// Bit symbol encoder constants (spec 10.3)
constexpr uint32_t BM_LENGTH_SHIFT = 13;
constexpr uint32_t BM_MAX_COUNT    = 1u << 13;  // 8192

// ---- SymbolModel ----
// Adaptive multi-symbol probability model (spec 10.2)
struct SymbolModel {
    uint32_t num_symbols;
    std::vector<uint16_t> symbol_count;
    std::vector<uint16_t> distribution;
    uint16_t update_cycle;
    uint16_t symbols_until_update;

    void init(uint32_t symbols);
    void update_distribution();
};

// ---- BitModel ----
// Adaptive single-bit probability model (spec 10.3)
struct BitModel {
    uint16_t bit_0_count;
    uint16_t bit_count;
    uint16_t bit_0_prob;
    uint16_t update_cycle;
    uint16_t bits_until_update;

    void init();
    void update_bit_distribution();
};

// ---- ArithmeticDecoder ----
// LAZ arithmetic decoder (spec sections 09-10)
class ArithmeticDecoder {
public:
    void init(const uint8_t* data, size_t size);

    // Symbol decoder (spec 10.2.4)
    uint32_t decodeSymbol(SymbolModel& m);

    // Bit decoder (spec 10.3.4)
    uint32_t decodeBit(BitModel& m);

    // Raw bit reader (spec 10.4.2)
    uint32_t readBits(int bit_count);
    uint16_t readShort() { return static_cast<uint16_t>(readBits(16)); }
    uint32_t readInt()   { uint32_t lo = readBits(16); return lo | (readBits(16) << 16); }

    const uint8_t* position() const { return ptr_; }

private:
    void renorm();

    uint8_t readByte() {
        if (ptr_ < end_)
            return *ptr_++;
        return 0;
    }

    uint32_t value_  = 0;
    uint32_t length_ = 0;
    const uint8_t* ptr_ = nullptr;
    const uint8_t* end_ = nullptr;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
