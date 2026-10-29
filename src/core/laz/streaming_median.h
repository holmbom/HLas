#pragma once
#ifdef HLAS_HAVE_LAZ

#include <cstdint>

namespace HLas::detail::laz {

// Streaming median over 5 values (exactly as the LAZ format requires)
// Used for dX and dY prediction in Point10 and Point14.
struct StreamingMedian {
    int32_t values[5] = {0, 0, 0, 0, 0};
    bool high = true;

    int32_t median() const { return values[2]; }

    void insert(int32_t v) {
        if (high) {
            if (v < values[2]) {
                // v below median: remove values[4], insert v in lower portion
                values[4] = values[3];
                values[3] = values[2];
                if (v < values[0]) {
                    values[2] = values[1];
                    values[1] = values[0];
                    values[0] = v;
                } else if (v < values[1]) {
                    values[2] = values[1];
                    values[1] = v;
                } else {
                    values[2] = v;
                }
                // high stays true (inserted on same side as removal)
            } else {
                // v at or above median: insert in upper portion
                if (v < values[3]) {
                    values[4] = values[3];
                    values[3] = v;
                } else {
                    values[4] = v;
                }
                high = false;  // toggle: next time remove low
            }
        } else {
            if (values[2] < v) {
                // v above median: remove values[0], insert v in upper portion
                values[0] = values[1];
                values[1] = values[2];
                if (values[4] < v) {
                    values[2] = values[3];
                    values[3] = values[4];
                    values[4] = v;
                } else if (values[3] < v) {
                    values[2] = values[3];
                    values[3] = v;
                } else {
                    values[2] = v;
                }
                // high stays false (inserted on same side as removal)
            } else {
                // v at or below median: insert in lower portion
                if (values[1] < v) {
                    values[0] = values[1];
                    values[1] = v;
                } else {
                    values[0] = v;
                }
                high = true;  // toggle: next time remove high
            }
        }
    }

    void reset() {
        for (auto& v : values) v = 0;
        high = true;
    }
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
