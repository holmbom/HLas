#include "vlr_parser.h"
#include "header_parser.h"
#include "file_io.h"
#include "test_data.h"
#include <cstdio>
#include <cstring>

using namespace HLas::detail;

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

int test_vlr() {
    int fails = 0;
    printf("test_vlr:\n");

    // 1. Parse synthetic VLRs
    {
        // Build buffer: 2 VLRs back to back
        // VLR 1: user_id="LASF_Projection", record_id=34735, 8 bytes data
        // VLR 2: user_id="LASF_Spec", record_id=4, 4 bytes data
        uint8_t buf[54 + 8 + 54 + 4];
        std::memset(buf, 0, sizeof(buf));

        // VLR 1 header
        std::strncpy(reinterpret_cast<char*>(buf + 2), "LASF_Projection", 16);
        uint16_t rid1 = 34735;
        std::memcpy(buf + 18, &rid1, 2);
        uint16_t len1 = 8;
        std::memcpy(buf + 20, &len1, 2);
        // VLR 1 data
        uint8_t data1[8] = {1,2,3,4,5,6,7,8};
        std::memcpy(buf + 54, data1, 8);

        // VLR 2 header (at offset 62)
        std::strncpy(reinterpret_cast<char*>(buf + 62 + 2), "LASF_Spec", 16);
        uint16_t rid2 = 4;
        std::memcpy(buf + 62 + 18, &rid2, 2);
        uint16_t len2 = 4;
        std::memcpy(buf + 62 + 20, &len2, 2);
        // VLR 2 data
        uint8_t data2[4] = {10,20,30,40};
        std::memcpy(buf + 62 + 54, data2, 4);

        std::vector<Vlr> vlrs;
        ErrorCode err = parse_vlrs(buf, sizeof(buf), 0, 2, vlrs);
        CHECK(err == ErrorCode::Ok);
        CHECK(vlrs.size() == 2);
        CHECK(vlrs[0].record_id == 34735);
        CHECK(vlrs[0].data_size == 8);
        CHECK(vlrs[0].data[0] == 1);
        CHECK(vlrs[1].record_id == 4);
        CHECK(vlrs[1].data_size == 4);
        CHECK(vlrs[1].data[0] == 10);

        // find_vlr
        const Vlr* found = find_vlr(vlrs, "LASF_Projection", 34735);
        CHECK(found != nullptr);
        CHECK(found->record_id == 34735);

        const Vlr* not_found = find_vlr(vlrs, "NoSuch", 0);
        CHECK(not_found == nullptr);
    }

    // 2. VLR overflow
    {
        uint8_t buf[54];
        std::memset(buf, 0, sizeof(buf));
        uint16_t big_len = 1000;
        std::memcpy(buf + 20, &big_len, 2);  // record_length = 1000

        std::vector<Vlr> vlrs;
        ErrorCode err = parse_vlrs(buf, sizeof(buf), 0, 1, vlrs);
        CHECK(err == ErrorCode::CorruptVlr);
    }

    // 3. Real file VLRs — autzen.laz har 5 (3x LASF_Projection, en verktygs-VLR, laszip encoded).
    //    Den okomprimerade autzen.las har inga, så LAZ-filen är det som testar det här.
    {
        MappedFile f;
        f.open(testdata::kLaz);
        if (f.is_open()) {
            ParsedHeader h;
            parse_header(f.data(), f.size(), h);

            std::vector<Vlr> vlrs;
            size_t vlr_offset = h.header_size;
            ErrorCode err = parse_vlrs(f.data(), f.size(),
                                       vlr_offset, h.num_vlrs, vlrs);
            CHECK(err == ErrorCode::Ok);
            CHECK(vlrs.size() == 5);
            for (const auto& v : vlrs)
                CHECK(v.data_size > 0);

            // LAZ-VLR:n måste finnas — utan den går filen inte att dekomprimera
            const Vlr* laz_vlr = find_vlr(vlrs, "laszip encoded", 22204);
            CHECK(laz_vlr != nullptr);
        } else {
            printf("  SKIP (riktig fil) — %s saknas\n", testdata::kLaz);
        }
    }

    // 4. COPC-VLR:n finns och ligger först — grunden för Fas 15
    {
        MappedFile f;
        f.open(testdata::kCopc);
        if (f.is_open()) {
            ParsedHeader h;
            parse_header(f.data(), f.size(), h);

            std::vector<Vlr> vlrs;
            ErrorCode err = parse_vlrs(f.data(), f.size(),
                                       h.header_size, h.num_vlrs, vlrs);
            CHECK(err == ErrorCode::Ok);
            CHECK(vlrs.size() == 3);

            const Vlr* copc = find_vlr(vlrs, "copc", 1);
            CHECK(copc != nullptr);
            if (copc) CHECK(copc->data_size == 160);
        }
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}
