/// Unit tests for InlineMemEqual in column_marshaller.h
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <vector>

#include "column_marshaller.h"

using namespace taper;

// Helper: create a buffer with specific content
static std::vector<uint8_t> make_buf(const char* s) {
    size_t len = strlen(s);
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(s),
                                reinterpret_cast<const uint8_t*>(s) + len);
}

// Helper: create unaligned pointer (offset by 1 from aligned allocation)
static uint8_t* make_unaligned(const uint8_t* src, size_t len) {
    // Allocate extra byte and offset by 1 to guarantee misalignment
    uint8_t* buf = static_cast<uint8_t*>(malloc(len + 16));
    uint8_t* unaligned = buf + 1; // guaranteed not 8-byte aligned if buf is
    memcpy(unaligned, src, len);
    return buf; // caller frees this, use unaligned = buf+1 for access
}

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #expr); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

static void test_length_0() {
    uint8_t a[] = {0xFF};
    uint8_t b[] = {0x00};
    CHECK(InlineMemEqual(a, b, 0) == true, "len=0 always equal");
    CHECK(InlineMemEqual(nullptr, nullptr, 0) == true, "len=0 nullptr");
}

static void test_length_1_to_3() {
    // len=1: equal
    {
        uint8_t a[] = {0x42};
        uint8_t b[] = {0x42};
        CHECK(InlineMemEqual(a, b, 1) == true, "len=1 equal");
    }
    // len=1: different
    {
        uint8_t a[] = {0x42};
        uint8_t b[] = {0x43};
        CHECK(InlineMemEqual(a, b, 1) == false, "len=1 different");
    }
    // len=2: equal
    {
        uint8_t a[] = {0x01, 0x02};
        uint8_t b[] = {0x01, 0x02};
        CHECK(InlineMemEqual(a, b, 2) == true, "len=2 equal");
    }
    // len=2: last byte different
    {
        uint8_t a[] = {0x01, 0x02};
        uint8_t b[] = {0x01, 0xFF};
        CHECK(InlineMemEqual(a, b, 2) == false, "len=2 last diff");
    }
    // len=3: equal
    {
        uint8_t a[] = {0xAA, 0xBB, 0xCC};
        uint8_t b[] = {0xAA, 0xBB, 0xCC};
        CHECK(InlineMemEqual(a, b, 3) == true, "len=3 equal");
    }
    // len=3: middle different
    {
        uint8_t a[] = {0xAA, 0xBB, 0xCC};
        uint8_t b[] = {0xAA, 0xFF, 0xCC};
        CHECK(InlineMemEqual(a, b, 3) == false, "len=3 middle diff");
    }
    // len=3: first different
    {
        uint8_t a[] = {0xAA, 0xBB, 0xCC};
        uint8_t b[] = {0xFF, 0xBB, 0xCC};
        CHECK(InlineMemEqual(a, b, 3) == false, "len=3 first diff");
    }
}

static void test_length_4_to_7() {
    // len=4: equal
    {
        auto a = make_buf("abcd");
        auto b = make_buf("abcd");
        CHECK(InlineMemEqual(a.data(), b.data(), 4) == true, "len=4 equal");
    }
    // len=7: equal
    {
        auto a = make_buf("abcdefg");
        auto b = make_buf("abcdefg");
        CHECK(InlineMemEqual(a.data(), b.data(), 7) == true, "len=7 equal");
    }
    // len=5: last byte different
    {
        uint8_t a[] = {1,2,3,4,5};
        uint8_t b[] = {1,2,3,4,9};
        CHECK(InlineMemEqual(a, b, 5) == false, "len=5 last diff");
    }
    // len=6: first byte different
    {
        uint8_t a[] = {1,2,3,4,5,6};
        uint8_t b[] = {9,2,3,4,5,6};
        CHECK(InlineMemEqual(a, b, 6) == false, "len=6 first diff");
    }
}

static void test_length_8() {
    // len=8: equal
    {
        auto a = make_buf("12345678");
        auto b = make_buf("12345678");
        CHECK(InlineMemEqual(a.data(), b.data(), 8) == true, "len=8 equal");
    }
    // len=8: first byte different
    {
        auto a = make_buf("12345678");
        auto b = make_buf("X2345678");
        CHECK(InlineMemEqual(a.data(), b.data(), 8) == false, "len=8 first diff");
    }
    // len=8: last byte different
    {
        auto a = make_buf("12345678");
        auto b = make_buf("1234567X");
        CHECK(InlineMemEqual(a.data(), b.data(), 8) == false, "len=8 last diff");
    }
}

static void test_length_9() {
    // len=9: equal
    {
        auto a = make_buf("123456789");
        auto b = make_buf("123456789");
        CHECK(InlineMemEqual(a.data(), b.data(), 9) == true, "len=9 equal");
    }
    // len=9: middle (byte 4) different
    {
        auto a = make_buf("123456789");
        auto b = make_buf("1234X6789");
        CHECK(InlineMemEqual(a.data(), b.data(), 9) == false, "len=9 middle diff");
    }
}

static void test_length_13() {
    // len=13: equal (typical key string like "key_1234_c0")
    {
        auto a = make_buf("key_12345_c0!");
        auto b = make_buf("key_12345_c0!");
        CHECK(InlineMemEqual(a.data(), b.data(), 13) == true, "len=13 equal");
    }
    // len=13: byte 6 different
    {
        auto a = make_buf("key_12345_c0!");
        auto b = make_buf("key_12X45_c0!");
        CHECK(InlineMemEqual(a.data(), b.data(), 13) == false, "len=13 middle diff");
    }
    // len=13: last byte different
    {
        auto a = make_buf("key_12345_c0!");
        auto b = make_buf("key_12345_c0X");
        CHECK(InlineMemEqual(a.data(), b.data(), 13) == false, "len=13 last diff");
    }
}

static void test_length_16() {
    // len=16: equal
    {
        auto a = make_buf("0123456789ABCDEF");
        auto b = make_buf("0123456789ABCDEF");
        CHECK(InlineMemEqual(a.data(), b.data(), 16) == true, "len=16 equal");
    }
    // len=16: byte 8 different (boundary between first-8 and last-8)
    {
        auto a = make_buf("0123456789ABCDEF");
        auto b = make_buf("01234567X9ABCDEF");
        CHECK(InlineMemEqual(a.data(), b.data(), 16) == false, "len=16 byte8 diff");
    }
}

static void test_length_17() {
    // len=17: first byte over the 8-16 range, enters 17-32 path
    {
        auto a = make_buf("0123456789ABCDEFG");
        auto b = make_buf("0123456789ABCDEFG");
        CHECK(InlineMemEqual(a.data(), b.data(), 17) == true, "len=17 equal");
    }
    // len=17: middle different
    {
        auto a = make_buf("0123456789ABCDEFG");
        auto b = make_buf("01234567X9ABCDEFG");
        CHECK(InlineMemEqual(a.data(), b.data(), 17) == false, "len=17 middle diff");
    }
}

static void test_long_string() {
    // len=50: falls back to memcmp
    {
        const char* s = "This is a longer string used to test the memcmp fb";
        auto a = make_buf(s);
        auto b = make_buf(s);
        CHECK(InlineMemEqual(a.data(), b.data(), a.size()) == true, "len=50 equal");
    }
    // len=50: byte 25 different
    {
        auto a = make_buf("This is a longer string used to test the memcmp fb");
        auto b = a;
        b[25] = 'X';
        CHECK(InlineMemEqual(a.data(), b.data(), a.size()) == false, "len=50 middle diff");
    }
}

static void test_unaligned() {
    // Test with deliberately unaligned addresses (offset by 1)
    const char* s = "key_9999_c3"; // 11 bytes — uses 8-16 path
    size_t len = strlen(s);

    uint8_t* buf_a = static_cast<uint8_t*>(malloc(len + 16));
    uint8_t* buf_b = static_cast<uint8_t*>(malloc(len + 16));
    uint8_t* a = buf_a + 1; // unaligned
    uint8_t* b = buf_b + 3; // differently unaligned
    memcpy(a, s, len);
    memcpy(b, s, len);

    CHECK(InlineMemEqual(a, b, len) == true, "unaligned equal");

    b[len - 1] = 'X';
    CHECK(InlineMemEqual(a, b, len) == false, "unaligned last diff");

    free(buf_a);
    free(buf_b);
}

static void test_compare_varchar_from_row() {
    // Verify CompareVarcharFromRow integration works end-to-end
    // Serialize "hello_world" (11 bytes) and compare
    uint8_t arena[64];
    const char* key = "hello_world";
    size_t len = 11;
    SerializeVarcharToBuffer(arena, reinterpret_cast<const uint8_t*>(key), len);

    CHECK(CompareVarcharFromRow(arena, reinterpret_cast<const uint8_t*>(key), len) == true,
          "CompareVarcharFromRow equal");
    CHECK(CompareVarcharFromRow(arena, reinterpret_cast<const uint8_t*>("hello_worlX"), len) == false,
          "CompareVarcharFromRow last diff");
    CHECK(CompareVarcharFromRow(arena, reinterpret_cast<const uint8_t*>("Xello_world"), len) == false,
          "CompareVarcharFromRow first diff");
    CHECK(CompareVarcharFromRow(arena, reinterpret_cast<const uint8_t*>("hello"), 5) == false,
          "CompareVarcharFromRow length mismatch");
}

int main() {
    test_length_0();
    test_length_1_to_3();
    test_length_4_to_7();
    test_length_8();
    test_length_9();
    test_length_13();
    test_length_16();
    test_length_17();
    test_long_string();
    test_unaligned();
    test_compare_varchar_from_row();

    printf("\n=== InlineMemEqual Tests ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    if (tests_failed > 0) {
        printf("SOME TESTS FAILED!\n");
        return 1;
    }
    printf("ALL TESTS PASSED.\n");
    return 0;
}
