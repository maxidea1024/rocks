#include <AssetNugget/NuggetKey.h>
#include <Base/Endian.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Math/GenericPlatformMath.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 에셋 너겟 키 기록 시험이다(Docs/Design/AssetSystem 9.1절 결정 2–4): 태그·인코딩 golden,
// NaN 단일화·-0, 경계(None·길이 접두), 유효하지 않은 UTF-8, buffer 기록기·Finish 규칙·Fail, 추적
// 줄, 공통 머리 항목과 순서, 인코더, SCC 키(순서 무관·중복·null·역할 접두·Merkle), 구성원 키, 할당
// 실패.
using namespace rocks;
namespace nugget_key_tests_file {

namespace {

// buffer 기록기로 쓴 byte다.
template <class Body>
Array<uint8> Encode(Allocator& allocator, Body&& body) noexcept {
    Array<uint8> bytes(allocator);
    NuggetKeyWriter writer(bytes);
    body(writer);
    CHECK(writer.Error());
    return bytes;
}

template <class Body>
Sha256Digest KeyOf(Body&& body) noexcept {
    NuggetKeyWriter writer;
    body(writer);
    auto key = writer.Finish();
    CHECK(key);
    return key ? key.Value() : Sha256Digest{};
}

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 추적 줄을 모은다.
class LineTrace final : public NuggetKeyTrace {
public:
    explicit LineTrace(Allocator& allocator) noexcept : text(allocator) {
    }

    void Line(StringView line) noexcept override {
        CHECK(text.TryAppend(line) && text.TryAppend("\n"));
    }

    String text;
};

constexpr uint8 TagOf(NuggetKeyTag tag) noexcept {
    return static_cast<uint8>(tag);
}

} // namespace

// 태그·값 인코딩 golden(정수 LE, Guid big-endian high→low, 길이 접두), 태그 번호 고정.
TEST(NuggetKey_EncodingGolden) {
    SystemAllocator allocator;
    // 태그 번호는 고정된다(바꾸면 키 형식 버전을 올린다).
    CHECK(TagOf(NuggetKeyTag::None) == 1 && TagOf(NuggetKeyTag::Bool) == 2 &&
          TagOf(NuggetKeyTag::U32) == 3 && TagOf(NuggetKeyTag::U64) == 4 &&
          TagOf(NuggetKeyTag::I64) == 5 && TagOf(NuggetKeyTag::F32) == 6 &&
          TagOf(NuggetKeyTag::F64) == 7 && TagOf(NuggetKeyTag::Guid) == 8 &&
          TagOf(NuggetKeyTag::Digest) == 9 && TagOf(NuggetKeyTag::String) == 10 &&
          TagOf(NuggetKeyTag::Bytes) == 11 && TagOf(NuggetKeyTag::List) == 12 &&
          TagOf(NuggetKeyTag::Map) == 13 && TagOf(NuggetKeyTag::Set) == 14 &&
          TagOf(NuggetKeyTag::Field) == 15 && TagOf(NuggetKeyTag::Struct) == 16 &&
          TagOf(NuggetKeyTag::End) == 17);
    const Array<uint8> bytes = Encode(allocator, [](NuggetKeyWriter& writer) noexcept {
        writer.WriteNone();
        writer.WriteBool(true);
        writer.WriteBool(false);
        writer.WriteU32(0x01020304u);
        writer.WriteU64(5);
        writer.WriteI64(-2);
        writer.WriteGuid(Guid{0x0102030405060708ull, 0x090a0b0c0d0e0f10ull});
        writer.WriteString("ab");
        uint8 raw[] = {7, 8};
        writer.WriteBytes(raw);
        writer.WriteList(2);
        writer.WriteMap(1);
        writer.WriteSet(0);
        writer.WriteField("f");
        writer.WriteStructBegin();
        writer.WriteStructEnd();
        uint8 pre[] = {0xaa};
        writer.WriteRaw(pre);
    });
    const uint8 expected[] = {
        1,                                                   // None
        2,   1,                                              // Bool true
        2,   0,                                              // Bool false
        3,   4,    3,    2,    1,                            // U32 LE
        4,   5,    0,    0,    0,    0,    0,    0,    0,    // U64 LE
        5,   0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // I64 -2
        8,   1,    2,    3,    4,    5,    6,    7,    8,    9,   10,
        11,  12,   13,   14,   15,   16,                               // Guid
        10,  2,    0,    0,    0,    0,    0,    0,    0,    'a', 'b', // String
        11,  2,    0,    0,    0,    0,    0,    0,    0,    7,   8,   // Bytes
        12,  2,    0,    0,    0,    0,    0,    0,    0,              // List 2
        13,  1,    0,    0,    0,    0,    0,    0,    0,              // Map 1
        14,  0,    0,    0,    0,    0,    0,    0,    0,              // Set 0
        15,  1,    0,    0,    0,    0,    0,    0,    0,    'f',      // Field
        16,  17,                                                       // Struct·End
        0xaa};                                                         // Raw
    CHECK(Same(bytes.View(), expected));

    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(index);
    }

    const Array<uint8> digestBytes =
        Encode(allocator, [&](NuggetKeyWriter& writer) noexcept { writer.WriteDigest(digest); });
    REQUIRE(digestBytes.Size() == 1 + Sha256Digest::Bytes);
    CHECK(digestBytes[0] == 9 && digestBytes[1] == 0 && digestBytes[32] == 31);

    // 해시 기록기의 키 = 같은 byte의 SHA-256.
    const Sha256Digest key = KeyOf([](NuggetKeyWriter& writer) noexcept {
        writer.WriteNone();
        writer.WriteBool(true);
        writer.WriteBool(false);
        writer.WriteU32(0x01020304u);
        writer.WriteU64(5);
        writer.WriteI64(-2);
        writer.WriteGuid(Guid{0x0102030405060708ull, 0x090a0b0c0d0e0f10ull});
        writer.WriteString("ab");
        uint8 raw[] = {7, 8};
        writer.WriteBytes(raw);
        writer.WriteList(2);
        writer.WriteMap(1);
        writer.WriteSet(0);
        writer.WriteField("f");
        writer.WriteStructBegin();
        writer.WriteStructEnd();
        uint8 pre[] = {0xaa};
        writer.WriteRaw(pre);
    });
    CHECK(key == Sha256::Hash(expected));
}

// 실수는 bit 패턴이다: NaN은 부호·payload와 무관하게 quiet NaN 하나, -0과 +0은 다르다, 무한대 둘.
TEST(NuggetKey_FloatsAreBitPatterns) {
    SystemAllocator allocator;
    const uint32 nans32[] = {0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x7fbfffffu, 0xffffffffu};
    for (uint32 bits : nans32) {
        const Array<uint8> bytes = Encode(allocator, [bits](NuggetKeyWriter& writer) noexcept {
            writer.WriteF32(platform::BitCast<float>(bits));
        });
        REQUIRE(bytes.Size() == 5);
        CHECK(bytes[0] == 6 && LoadLittleEndian<uint32>(bytes.Data() + 1) == 0x7fc00000u);
    }

    const uint64 nans64[] = {0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff0000000000001ull,
                             0xffffffffffffffffull};
    for (uint64 bits : nans64) {
        const Array<uint8> bytes = Encode(allocator, [bits](NuggetKeyWriter& writer) noexcept {
            writer.WriteF64(platform::BitCast<double>(bits));
        });
        REQUIRE(bytes.Size() == 9);
        CHECK(bytes[0] == 7 && LoadLittleEndian<uint64>(bytes.Data() + 1) == 0x7ff8000000000000ull);
    }

    const uint32 kept32[] = {0x00000000u, 0x80000000u, 0x7f800000u,
                             0xff800000u, 0x00000001u, 0x3f800000u};
    for (uint32 bits : kept32) {
        const Array<uint8> bytes = Encode(allocator, [bits](NuggetKeyWriter& writer) noexcept {
            writer.WriteF32(platform::BitCast<float>(bits));
        });
        CHECK(LoadLittleEndian<uint32>(bytes.Data() + 1) == bits);
    }

    const uint64 kept64[] = {0ull, 0x8000000000000000ull, 0x7ff0000000000000ull,
                             0xfff0000000000000ull, 1ull};
    for (uint64 bits : kept64) {
        const Array<uint8> bytes = Encode(allocator, [bits](NuggetKeyWriter& writer) noexcept {
            writer.WriteF64(platform::BitCast<double>(bits));
        });
        CHECK(LoadLittleEndian<uint64>(bytes.Data() + 1) == bits);
    }

    CHECK(!(KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteF32(0.0f); }) ==
            KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteF32(-0.0f); })));
}

// 경계: 값 없음(None)과 다음 필드가 섞이지 않는다, 길이 접두가 이웃 문자열을 가른다, 태그가 같은
// 값의 다른 종류를 가른다.
TEST(NuggetKey_BoundariesAreUnambiguous) {
    // Optional = 5, 다음 필드 없음 vs Optional 없음, 다음 필드 = 5.
    CHECK(!(KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteU32(5); }) ==
            KeyOf([](NuggetKeyWriter& writer) noexcept {
                writer.WriteNone();
                writer.WriteU32(5);
            })));
    CHECK(!(KeyOf([](NuggetKeyWriter& writer) noexcept {
                writer.WriteString("ab");
                writer.WriteString("c");
            }) == KeyOf([](NuggetKeyWriter& writer) noexcept {
                writer.WriteString("a");
                writer.WriteString("bc");
            })));
    CHECK(!(KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteU64(5); }) ==
            KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteI64(5); })));
    CHECK(!(KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteString("x"); }) ==
            KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteField("x"); })));
    uint8 x[] = {'x'};
    CHECK(!(KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteString("x"); }) ==
            KeyOf([&](NuggetKeyWriter& writer) noexcept { writer.WriteBytes(x); })));
    CHECK(!(KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteList(0); }) ==
            KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteSet(0); })));
    // 같은 기록은 같은 키다(결정적).
    CHECK(KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteString("same"); }) ==
          KeyOf([](NuggetKeyWriter& writer) noexcept { writer.WriteString("same"); }));
}

// 오류 상태: 유효하지 않은 UTF-8 String·Field는 InvalidArgument(뒤 기록 무시), Fail은 첫 오류만,
// None은 무시, buffer 기록기의 Finish는 InvalidArgument, Finish 두 번째는 InvalidArgument, buffer
// 할당 실패는 OutOfMemory.
TEST(NuggetKey_ErrorStates) {
    SystemAllocator allocator;
    const char bad[] = {'a', static_cast<char>(0xc3)};
    {
        NuggetKeyWriter writer;
        writer.WriteString(StringView(bad, 2));
        writer.WriteU32(1);
        CHECK(writer.Error().Error() == ErrorCode::InvalidArgument &&
              writer.Finish().Error() == ErrorCode::InvalidArgument);
    }

    {
        NuggetKeyWriter writer;
        writer.WriteField(StringView(bad, 2));
        CHECK(writer.Finish().Error() == ErrorCode::InvalidArgument);
    }

    {
        NuggetKeyWriter writer;
        writer.Fail(ErrorCode::None);
        CHECK(writer.Error());
        writer.Fail(ErrorCode::Busy);
        writer.Fail(ErrorCode::NotFound);
        CHECK(writer.Error().Error() == ErrorCode::Busy);
    }

    {
        NuggetKeyWriter writer;
        writer.WriteU32(1);
        CHECK(writer.Finish());
        CHECK(writer.Finish().Error() == ErrorCode::InvalidArgument);
        // 끝난 뒤 기록은 무시한다.
        writer.WriteU32(2);
        CHECK(writer.Error());
    }

    {
        Array<uint8> bytes(allocator);
        NuggetKeyWriter writer(bytes);
        writer.WriteU32(1);
        CHECK(writer.Finish().Error() == ErrorCode::InvalidArgument && bytes.Size() == 5);
    }

    {
        test::NFailAllocator failing(allocator);
        Array<uint8> bytes(failing);
        NuggetKeyWriter writer(bytes);
        failing.FailOnAttempt(1);
        writer.WriteU64(9);
        CHECK(writer.Error().Error() == ErrorCode::OutOfMemory);
        failing.DisableFailure();
        writer.WriteU64(10);
        CHECK(bytes.IsEmpty() && writer.Error().Error() == ErrorCode::OutOfMemory);
    }
}

// 추적 sink: 기록마다 사람이 읽는 줄(오류 뒤는 남기지 않는다), sink 없으면 비용 없음(키 같음).
TEST(NuggetKey_TraceLines) {
    SystemAllocator allocator;
    LineTrace trace(allocator);
    NuggetKeyWriter traced;
    traced.SetTrace(&trace);
    auto body = [](NuggetKeyWriter& writer) noexcept {
        writer.WriteNone();
        writer.WriteBool(true);
        writer.WriteBool(false);
        writer.WriteU32(7);
        writer.WriteU64(8);
        writer.WriteI64(-9);
        writer.WriteF32(1.0f);
        writer.WriteF64(-0.0);
        writer.WriteGuid(Guid{1, 2});
        writer.WriteDigest(Sha256Digest{});
        writer.WriteString("s");
        uint8 raw[] = {1, 2, 3};
        writer.WriteBytes(raw);
        writer.WriteList(1);
        writer.WriteMap(2);
        writer.WriteSet(3);
        writer.WriteField("f");
        writer.WriteStructBegin();
        writer.WriteStructEnd();
        writer.WriteRaw(raw);
    };
    body(traced);
    auto tracedKey = traced.Finish();
    REQUIRE(tracedKey);
    CHECK(tracedKey.Value() == KeyOf(body));
    const StringView expected =
        "none\nbool true\nbool false\nu32 7\nu64 8\ni64 -9\nf32 0x3f800000\n"
        "f64 0x8000000000000000\nguid 00000000-0000-0001-0000-000000000002\n"
        "digest 0000000000000000000000000000000000000000000000000000000000000000\n"
        "string\ns\nbytes 3\nlist 1\nmap 2\nset 3\nfield\nf\nstruct\nend\nraw 3\n  010203\n";
    CHECK(trace.text.View() == expected);
    // raw 내용은 32 byte씩 16진 줄로 남는다(손으로 정렬한 Map·Set 진단).
    {
        LineTrace lines(allocator);
        NuggetKeyWriter longRaw;
        longRaw.SetTrace(&lines);
        uint8 bytes[40];
        for (usize index = 0; index < sizeof(bytes); ++index) {
            bytes[index] = static_cast<uint8>(index * 7);
        }

        longRaw.WriteRaw(bytes);
        longRaw.WriteRaw(Span<const uint8>());
        REQUIRE(longRaw.Finish());
        CHECK(lines.text.View() ==
              "raw 40\n  00070e151c232a31383f464d545b626970777e858c939aa1a8afb6bdc4cbd2d9\n"
              "  e0e7eef5fc030a11\nraw 0\n");
    }

    // 오류 뒤에는 줄을 남기지 않는다.
    LineTrace after(allocator);
    NuggetKeyWriter failed;
    failed.SetTrace(&after);
    failed.Fail(ErrorCode::Busy);
    failed.WriteU32(1);
    CHECK(after.text.IsEmpty());
}

// 공통 머리: "RocksAssetNugget"·키 형식 버전·엔진 세대·프로젝트 세대·처리기·처리기 버전·결정적
// 수학 버전·툴체인 지문 순서(golden), 바꿀 수 있는 항목(프로젝트 세대·처리기·버전)은 하나씩 바꾸면
// 키가 바뀐다. 인코더 기록.
TEST(NuggetKey_HeaderAndEncoder) {
    SystemAllocator allocator;
    const NuggetProcessorId processor{Guid{0x11, 0x22}, Guid{0x33, 0x44}};
    const Guid project{0x55, 0x66};
    const Array<uint8> header = Encode(allocator, [&](NuggetKeyWriter& writer) noexcept {
        WriteNuggetKeyHeader(writer, processor, project);
    });
    const Array<uint8> manual = Encode(allocator, [&](NuggetKeyWriter& writer) noexcept {
        writer.WriteString("RocksAssetNugget");
        writer.WriteU32(NuggetKeyFormatVersion);
        writer.WriteGuid(EngineNuggetGeneration);
        writer.WriteGuid(project);
        writer.WriteGuid(processor.processor);
        writer.WriteGuid(processor.version);
        writer.WriteU32(deterministic::Version);
        writer.WriteDigest(build::ToolchainFingerprint());
    });
    CHECK(Same(header.View(), manual.View()) && NuggetKeyFormatVersion == 1);

    auto keyWith = [](const NuggetProcessorId& id, Guid generation) noexcept {
        return KeyOf([&](NuggetKeyWriter& writer) noexcept {
            WriteNuggetKeyHeader(writer, id, generation);
        });
    };
    const Sha256Digest base = keyWith(processor, project);
    CHECK(!(base == keyWith(processor, Guid{})));
    CHECK(!(base == keyWith({Guid{0x12, 0x22}, processor.version}, project)));
    CHECK(!(base == keyWith({processor.processor, Guid{0x34, 0x44}}, project)));
    CHECK(base == keyWith(processor, project));

    uint8 settings[] = {1, 2};
    const Array<uint8> encoder = Encode(allocator, [&](NuggetKeyWriter& writer) noexcept {
        WriteNuggetEncoder(writer, "zstd", "1.5.7", settings);
    });
    const Array<uint8> encoderManual = Encode(allocator, [&](NuggetKeyWriter& writer) noexcept {
        writer.WriteString("zstd");
        writer.WriteString("1.5.7");
        writer.WriteBytes(settings);
    });
    CHECK(Same(encoder.View(), encoderManual.View()));
}

namespace {

AssetId Id(uint64 low) noexcept {
    return AssetId{Guid{0xa55e7, low}};
}

Sha256Digest Digest(uint8 seed) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index);
    }

    return digest;
}

} // namespace

// SCC 키: 구성원·튜플 입력 순서와 무관, 구성원 지역 키·외부 의존 key(Merkle)·역할·구성원 집합이
// 바뀌면 바뀐다, 튜플 완전 중복은 합친다, 같은 접두의 역할은 다르다, 구성원 중복·null·유효하지 않은
// UTF-8 역할은 InvalidArgument. 구성원 키는 SCC key·AssetId마다 다르다.
TEST(NuggetKey_SccKeys) {
    SystemAllocator allocator;
    const NuggetSccMember a{Id(2), Digest(1)};
    const NuggetSccMember b{Id(1), Digest(2)};
    const NuggetSccMember c{Id(3), Digest(3)};
    const NuggetSccDependency x{Id(2), "albedo", Id(9), Digest(9)};
    const NuggetSccDependency y{Id(1), "normal", Id(8), Digest(8)};
    auto key = [&](Span<const NuggetSccMember> members,
                   Span<const NuggetSccDependency> external) noexcept {
        auto made = NuggetSccKey(allocator, members, external);
        CHECK(made);
        return made ? made.Value() : Sha256Digest{};
    };
    const NuggetSccMember ab[] = {a, b};
    const NuggetSccMember ba[] = {b, a};
    const NuggetSccDependency xy[] = {x, y};
    const NuggetSccDependency yx[] = {y, x};
    const Sha256Digest base = key(ab, xy);
    CHECK(base == key(ba, yx) && base == key(ab, yx));
    // 중복 튜플은 합친다.
    const NuggetSccDependency xyx[] = {x, y, x};
    CHECK(base == key(ba, xyx));
    // 바뀌면 다르다: 지역 키·외부 key(Merkle)·역할·구성원 추가·튜플 빼기.
    const NuggetSccMember changedLocal[] = {{a.asset, Digest(7)}, b};
    CHECK(!(base == key(changedLocal, xy)));
    const NuggetSccDependency changedDependency[] = {{x.member, x.role, x.dependency, Digest(4)},
                                                     y};
    CHECK(!(base == key(ab, changedDependency)));
    const NuggetSccDependency changedRole[] = {{x.member, "albedo2", x.dependency, x.dependencyKey},
                                               y};
    CHECK(!(base == key(ab, changedRole)));
    const NuggetSccDependency prefixA[] = {{x.member, "ab", x.dependency, x.dependencyKey}};
    const NuggetSccDependency prefixB[] = {{x.member, "abc", x.dependency, x.dependencyKey}};
    CHECK(!(key(ab, prefixA) == key(ab, prefixB)));
    const NuggetSccMember abc[] = {a, b, c};
    CHECK(!(base == key(abc, xy)));
    const NuggetSccDependency onlyX[] = {x};
    CHECK(!(base == key(ab, onlyX)));
    // 구성원 사이의 AssetId를 가진 의존은 SCC 밖이다(정렬 표 검색이 이웃과 혼동하지 않는다).
    const NuggetSccMember gapped[] = {b, c};
    const NuggetSccDependency between[] = {{Id(1), "r", Id(2), Digest(6)}};
    CHECK(NuggetSccKey(allocator, gapped, between));
    // 구성원 하나·외부 의존 없음도 같은 식이다.
    const NuggetSccMember single[] = {a};
    CHECK(!(key(single, {}) == key(ab, {})));

    // 거부: 빈 구성원·중복·null, 튜플의 구성원이 구성원 아님·의존이 구성원(SCC 안 간선).
    CHECK(NuggetSccKey(allocator, {}, {}).Error() == ErrorCode::InvalidArgument);
    const NuggetSccDependency outsider[] = {{Id(7), "r", Id(9), Digest(1)}};
    CHECK(NuggetSccKey(allocator, ab, outsider).Error() == ErrorCode::InvalidArgument);
    const NuggetSccDependency inside[] = {{Id(2), "r", Id(1), Digest(1)}};
    CHECK(NuggetSccKey(allocator, ab, inside).Error() == ErrorCode::InvalidArgument);
    const NuggetSccMember duplicate[] = {a, {a.asset, Digest(5)}};
    CHECK(NuggetSccKey(allocator, duplicate, {}).Error() == ErrorCode::InvalidArgument);
    const NuggetSccMember nullMember[] = {{AssetId{}, Digest(1)}};
    CHECK(NuggetSccKey(allocator, nullMember, {}).Error() == ErrorCode::InvalidArgument);
    const NuggetSccDependency nullDependency[] = {{Id(2), "r", AssetId{}, Digest(1)}};
    CHECK(NuggetSccKey(allocator, ab, nullDependency).Error() == ErrorCode::InvalidArgument);
    const NuggetSccDependency nullOwner[] = {{AssetId{}, "r", Id(9), Digest(1)}};
    CHECK(NuggetSccKey(allocator, ab, nullOwner).Error() == ErrorCode::InvalidArgument);
    const char bad[] = {static_cast<char>(0xff)};
    const NuggetSccDependency badRole[] = {{Id(2), StringView(bad, 1), Id(9), Digest(1)}};
    CHECK(NuggetSccKey(allocator, ab, badRole).Error() == ErrorCode::InvalidArgument);

    // 구성원 키.
    CHECK(!(NuggetMemberKey(base, a.asset) == NuggetMemberKey(base, b.asset)));
    CHECK(!(NuggetMemberKey(base, a.asset) == NuggetMemberKey(Digest(1), a.asset)));
    CHECK(NuggetMemberKey(base, a.asset) == NuggetMemberKey(base, a.asset));
}

// SCC 키의 할당 실패(구성원 복사·튜플 표·튜플 인코딩)는 모두 OutOfMemory이고 실패 없는 결과는 같다.
TEST(NuggetKey_SccAllocationFailures) {
    SystemAllocator system;
    const NuggetSccMember members[] = {{Id(2), Digest(1)}, {Id(1), Digest(2)}};
    const NuggetSccDependency external[] = {{Id(2), "albedo", Id(9), Digest(9)},
                                            {Id(1), "normal", Id(8), Digest(8)}};
    auto expected = NuggetSccKey(system, members, external);
    REQUIRE(expected);
    test::NFailAllocator failing(system);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto key = NuggetSccKey(failing, members, external);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(key);
            CHECK(key.Value() == expected.Value());
            break;
        }

        CHECK(key.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace nugget_key_tests_file
