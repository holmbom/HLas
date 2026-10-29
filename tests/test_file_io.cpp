#include "file_io.h"
#include "test_data.h"
#include <cstdio>
#include <cstring>
#include <utility>

using namespace HLas::detail;

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

static const char* kTestFile = testdata::kLas;

int test_file_io() {
    int fails = 0;
    printf("test_file_io:\n");

    // Open nonexistent file — kräver ingen testdata
    {
        MappedFile f;
        ErrorCode err = f.open("nonexistent_file.xyz");
        CHECK(err == ErrorCode::FileNotFound);
        CHECK(!f.is_open());
        CHECK(f.data() == nullptr);
        CHECK(f.size() == 0);
    }

    if (!testdata::exists(kTestFile)) {
        printf("  SKIP (resten) — %s saknas\n", kTestFile);
        return fails;
    }

    // Open existing LAS file
    {
        MappedFile f;
        CHECK(!f.is_open());
        ErrorCode err = f.open(kTestFile);
        CHECK(err == ErrorCode::Ok);
        CHECK(f.is_open());
        CHECK(f.size() > 0);
        CHECK(f.data() != nullptr);
        CHECK(std::memcmp(f.data(), "LASF", 4) == 0);
    }

    // Move constructor
    {
        MappedFile a;
        a.open(kTestFile);
        const uint8_t* ptr = a.data();
        size_t sz = a.size();

        MappedFile b(std::move(a));
        CHECK(!a.is_open());
        CHECK(a.data() == nullptr);
        CHECK(b.is_open());
        CHECK(b.data() == ptr);
        CHECK(b.size() == sz);
    }

    // Move assignment
    {
        MappedFile a;
        a.open(kTestFile);
        const uint8_t* ptr = a.data();
        size_t sz = a.size();

        MappedFile b;
        b = std::move(a);
        CHECK(!a.is_open());
        CHECK(b.is_open());
        CHECK(b.data() == ptr);
        CHECK(b.size() == sz);
    }

    // Double close is safe
    {
        MappedFile f;
        f.open(kTestFile);
        f.close();
        f.close(); // should not crash
        CHECK(!f.is_open());
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}
