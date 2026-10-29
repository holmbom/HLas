#ifdef HLAS_HAVE_LAZ
#include "laz_chunk_table.h"
#include "laz/arithmetic_encoder.h"
#include "laz/integer_compressor.h"
#include <cstring>
#include <new>

namespace HLas::detail {

namespace {
void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}
} // namespace

bool write_laz_chunk_table(const std::vector<uint32_t>& chunk_sizes,
                           std::vector<uint8_t>& out) try {
    if (chunk_sizes.size() > 0xFFFFFFFFull)
        return false;

    std::vector<uint8_t> table;
    put_u32(table, 0);                                             // version
    put_u32(table, static_cast<uint32_t>(chunk_sizes.size()));     // antal

    if (!chunk_sizes.empty()) {
        laz::ArithmeticEncoder enc;
        enc.init();
        laz::IntegerCompressor ic;
        ic.init(enc, 32, 2);

        int32_t pred = 0;
        for (uint32_t s : chunk_sizes) {
            // uint32 -> int32 med samma bitmönster. En rak static_cast av ett
            // värde över INT32_MAX är implementation-defined före C++20; här är
            // den väldefinierad, och bitmönstret är vad kompressorn ska se.
            const int32_t sv = static_cast<int32_t>(s);
            ic.compress(pred, sv, 1);
            pred = sv;
        }
        enc.done();
        if (enc.failed())
            return false;

        table.insert(table.end(), enc.bytes().begin(), enc.bytes().end());
    }

    out.insert(out.end(), table.begin(), table.end());
    return true;
} catch (const std::bad_alloc&) {
    // Kontraktet är "returnerar false, out orörd". Utan den här skulle ett
    // allokeringsfel kasta ut genom finalize() — och finalize() anropas från
    // destruktorn, där ett undantag betyder std::terminate.
    return false;
}

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
