#include <Core/Math/VertexPacking.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace math_vertex_packing_tests_file {

using namespace rocks::math;

TEST(VertexPacking_NormalizedBoundariesClampAndNaN) {
    CHECK(PackSnorm8(1.0f) == 127 && PackSnorm8(-1.0f) == -127 && PackSnorm8(0.0f) == 0);
    CHECK(PackSnorm8(2.0f) == 127 && PackSnorm8(-2.0f) == -127);
    CHECK(PackUnorm8(1.0f) == 255 && PackUnorm8(0.0f) == 0);
    CHECK(PackUnorm8(1.5f) == 255 && PackUnorm8(-0.5f) == 0);
    CHECK(PackSnorm16(1.0f) == 32767 && PackSnorm16(-1.0f) == -32767);
    CHECK(PackUnorm16(1.0f) == 65535 && PackUnorm16(-3.0f) == 0);
    const float nan = platform::PlatformMath::QuietNaN<float>();
    CHECK(PackSnorm8(nan) == 0 && PackUnorm8(nan) == 0 && PackSnorm16(nan) == 0 &&
          PackUnorm16(nan) == 0);
    // 가장 가까운 정수로 반올림한다(가운데는 0에서 먼 쪽). 0.5/255는 float로 정확히 가운데 위에
    // 있어 1이고, 음수 쪽은 가운데 양옆 값으로 본다(−0.5/127은 float로 표현하면 가운데보다 0에
    // 가깝다).
    CHECK(PackUnorm8(0.5f / 255.0f) == 1);
    CHECK(PackUnorm8(0.49f / 255.0f) == 0);
    CHECK(PackSnorm8(-0.51f / 127.0f) == -1);
    CHECK(PackSnorm8(-0.49f / 127.0f) == 0);
    CHECK(PackSnorm16(-1.6f / 32767.0f) == -2 && PackSnorm16(-1.4f / 32767.0f) == -1);
    // GPU는 snorm의 가장 작은 정수도 −1로 읽는다.
    CHECK(UnpackSnorm8(-128) == -1.0f && UnpackSnorm8(-127) == -1.0f);
    CHECK(UnpackSnorm16(-32768) == -1.0f);
}

TEST(VertexPacking_EveryEncodedValueRoundTrips) {
    for (int32 value = -127; value <= 127; ++value) {
        CHECK(PackSnorm8(UnpackSnorm8(static_cast<int8>(value))) == value);
    }

    for (uint32 value = 0; value <= 255; ++value) {
        CHECK(PackUnorm8(UnpackUnorm8(static_cast<uint8>(value))) == value);
    }

    uint32 snormMismatches = 0;
    uint32 unormMismatches = 0;
    for (int32 value = -32767; value <= 32767; ++value) {
        snormMismatches += PackSnorm16(UnpackSnorm16(static_cast<int16>(value))) == value ? 0u : 1u;
    }

    for (uint32 value = 0; value <= 65535; ++value) {
        unormMismatches +=
            PackUnorm16(UnpackUnorm16(static_cast<uint16>(value))) == value ? 0u : 1u;
    }

    CHECK(snormMismatches == 0);
    CHECK(unormMismatches == 0);
}

TEST(VertexPacking_Rgb10A2LayoutAndClamp) {
    // r가 bit 0–9, g가 10–19, b가 20–29, a가 30–31이다.
    CHECK(PackRgb10A2Unorm(1.0f, 0.0f, 0.0f, 0.0f) == 0x3ffu);
    CHECK(PackRgb10A2Unorm(0.0f, 1.0f, 0.0f, 0.0f) == 0x3ffu << 10);
    CHECK(PackRgb10A2Unorm(0.0f, 0.0f, 1.0f, 0.0f) == 0x3ffu << 20);
    CHECK(PackRgb10A2Unorm(0.0f, 0.0f, 0.0f, 1.0f) == 3u << 30);
    CHECK(PackRgb10A2Unorm(2.0f, -1.0f, 0.5f, 0.4f) == (0x3ffu | (512u << 20) | (1u << 30)));
    const uint32 packed = PackRgb10A2Unorm(0.25f, 0.5f, 0.75f, 1.0f);
    CHECK(UnpackRgb10A2Unorm(packed, 0) == 256.0f / 1023.0f);
    CHECK(UnpackRgb10A2Unorm(packed, 1) == 512.0f / 1023.0f);
    CHECK(UnpackRgb10A2Unorm(packed, 2) == 767.0f / 1023.0f);
    CHECK(UnpackRgb10A2Unorm(packed, 3) == 1.0f);
    CHECK(UnpackRgb10A2Unorm(packed, 4) == 0.0f);
}

TEST(VertexPacking_NormalAndTangentInByte4Snorm) {
    const PackedNormal up = PackNormal(Vec3(0.0f, 0.0f, 1.0f));
    CHECK(up == (PackedNormal{0, 0, 127, 127}));
    const PackedNormal tangent = PackTangent(Vec3(-1.0f, 0.0f, 0.0f), -1.0f);
    CHECK(tangent == (PackedNormal{-127, 0, 0, -127}));
    CHECK(PackTangent(Vec3(0.0f, 1.0f, 0.0f), 0.0f).w == 127);
    const PackedNormal diagonal = PackNormal(Vec3(0.6f, -0.8f, 0.0f));
    CHECK(diagonal.x == 76 && diagonal.y == -102 && diagonal.z == 0);
}

TEST(VertexPacking_HalfBoundaries) {
    // 반올림 경계(1 + 2^-11은 짝수 쪽 1로, 1 + 3·2^-11은 1 + 2^-9로), 비정규수, 무한대, NaN이다.
    // 65536개 전체 왕복은 Float16_EveryHalfRoundTripsExactly가 본다.
    CHECK(Float16::FromFloat(1.0f + 1.0f / 2048.0f).bits == 0x3c00u);
    CHECK(Float16::FromFloat(1.0f + 3.0f / 2048.0f).bits == 0x3c02u);
    CHECK(Float16::FromFloat(5.9604644775390625e-8f).bits == 0x0001u);
    CHECK(Float16::FromFloat(2.98023223876953125e-8f).bits == 0x0000u);
    CHECK(Float16::FromFloat(65504.0f).bits == 0x7bffu);
    CHECK(Float16::FromFloat(65520.0f).bits == 0x7c00u);
    CHECK(Float16::FromFloat(-platform::PlatformMath::Infinity<float>()).bits == 0xfc00u);
    CHECK(Float16::FromFloat(platform::PlatformMath::QuietNaN<float>()).IsNaN());
    CHECK(Float16::FromBits(0x0001u).ToFloat() == 5.9604644775390625e-8f);
    uint32 mismatches = 0;
    for (uint32 bits = 0; bits <= 0xffffu; ++bits) {
        const Float16 half = Float16::FromBits(static_cast<uint16>(bits));
        const Float16 back = Float16::FromFloat(half.ToFloat());
        mismatches += (half.IsNaN() ? back.IsNaN() : back == half) ? 0u : 1u;
    }

    CHECK(mismatches == 0);
}
} // namespace math_vertex_packing_tests_file
