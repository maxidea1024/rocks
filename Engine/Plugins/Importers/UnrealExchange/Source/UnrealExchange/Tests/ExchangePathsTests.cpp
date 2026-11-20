#include <Core/Encoding/Sha256.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangePaths.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// UE 객체 경로 → Rocks 논리 경로 대응 시험이다(Docs/Design/UnrealExchange.ko.md 9.1절, 10.4절
// 7·13번): objectPath 문법, root 검사, 구성 요소 정리 규칙(받지 않는 문자·첫 문자·예약
// 이름·NFC·길이·해시 접미), 전체 길이, 할당 실패, 퍼징(결과는 언제나 논리 경로 규칙을 지킨다).
using namespace rocks;
namespace exchange_paths_tests_file {

constexpr uint64 PathFuzzSeed = 0x5558316150415448ull;
constexpr uint32 PathFuzzCount = 2000;
constexpr StringView DefaultRoot = "/Game/Unreal";

// 원래 경로 접두의 해시 접미("-" + SHA-256 앞 8자)다.
void AppendHashSuffix(StringView prefix, StringBuilderBase& out) noexcept {
    const Sha256Digest digest = Sha256::Hash(
        Span<const uint8>(reinterpret_cast<const uint8*>(prefix.Data()), prefix.ByteCount()));
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    (void)out.TryAppendAscii('-');
    (void)out.TryAppend(StringView(hex, UnrealPathHashChars));
}

// objectPath의 대응 경로가 expected이고 renamed가 맞는지 본다.
bool Maps(Allocator& allocator, StringView objectPath, StringView expected, bool renamed) noexcept {
    StringBuilder<256> out(allocator);
    ExchangePathOutcome outcome;
    if (!MapUnrealObjectPath(objectPath, DefaultRoot, out, outcome)) {
        return false;
    }

    return out.View() == expected && outcome.renamed == renamed &&
           outcome.failure == ExchangePathOutcome::Failure::None;
}

TEST(UnrealExchange_ObjectPathSyntax) {
    const StringView valid[] = {"/Game/Rock.Rock", "/Game/Props/SM_Rock.SM_Rock",
                                "/Game/My Props/_Shared._Shared", "/Paper2D/A/B/C.Other",
                                "/Game/\xEA\xB0\x80.\xEA\xB0\x80"};
    for (const StringView& path : valid) {
        CHECK(IsValidUnrealObjectPath(path));
    }

    const StringView invalid[] = {"",
                                  "Game/Rock.Rock",
                                  "/Rock.Rock",
                                  "/Game/Rock",
                                  "/Game/Rock.",
                                  "/Game//Rock.Rock",
                                  "/Game/.Rock",
                                  "/Game/Rock/.Rock",
                                  "/Game/Ro.ck.Rock",
                                  "/Game/Rock.Ro/ck",
                                  "/Game/Rock.Rock:Sub",
                                  "/Game\\Rock.Rock",
                                  "/Game/Ro\x01"
                                  "ck.Rock",
                                  "/Game/Ro\x7f"
                                  "ck.Rock",
                                  "/Game/\xC0\xAF.x",
                                  "/Game/.."};
    for (const StringView& path : invalid) {
        CHECK(!IsValidUnrealObjectPath(path));
        CHECK(UnrealPackagePath(path).IsEmpty());
    }

    SystemAllocator allocator;
    {
        StringBuilder<1100> longest(allocator);
        REQUIRE(longest.TryAppend("/Game/"));
        REQUIRE(longest.TryAppendRepeated('a', UnrealObjectPathMaxBytes - 8));
        REQUIRE(longest.TryAppend(".a"));
        CHECK(longest.ByteCount() == UnrealObjectPathMaxBytes);
        CHECK(IsValidUnrealObjectPath(longest.View()));
        REQUIRE(longest.TryAppendAscii('a'));
        CHECK(!IsValidUnrealObjectPath(longest.View()));
    }

    CHECK(UnrealPackagePath("/Game/Props/SM_Rock.Other") == StringView("/Game/Props/SM_Rock"));
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(UnrealExchange_ImportRoot) {
    SystemAllocator allocator;
    const StringView valid[] = {"/Game/Unreal", "/Game", "/Game/\xEA\xB0\x80-_1"};
    for (const StringView& root : valid) {
        CHECK(ValidateUnrealImportRoot(root, allocator));
    }

    const StringView invalid[] = {"",           "/",        "Game/Unreal", "/Game/",   "/Game//X",
                                  "/Game/My X", "/Game/_X", "/Game/CON",   "/Game\\X", "/Game/A.B"};
    for (const StringView& root : invalid) {
        CHECK(ValidateUnrealImportRoot(root, allocator).Error() == ErrorCode::InvalidArgument);
    }

    {
        StringBuilder<300> tooLong(allocator);
        for (uint32 index = 0; index < 5; ++index) {
            REQUIRE(tooLong.TryAppendAscii('/'));
            REQUIRE(tooLong.TryAppendRepeated('a', 60));
        }

        CHECK(ValidateUnrealImportRoot(tooLong.View(), allocator).Error() ==
              ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 정리 규칙(9.1절 1–4): 받지 않는 문자, 첫 문자, 예약 이름, 바뀐 구성 요소의 해시 접미.
TEST(UnrealExchange_MapPathCleanup) {
    SystemAllocator allocator;
    {
        CHECK(Maps(allocator, "/Game/Props/SM_Rock.SM_Rock", "/Game/Unreal/Game/Props/SM_Rock",
                   false));
        CHECK(Maps(allocator, "/Paper2D/Sprites/S-1.Other", "/Game/Unreal/Paper2D/Sprites/S-1",
                   false));

        // 공백 → '_', '_'로 시작 → 'U' 접두. 접미 해시 입력은 그 구성 요소까지의 원래 경로다.
        StringBuilder<256> expected(allocator);
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/My_Props"));
        AppendHashSuffix("/Game/My Props", expected);
        REQUIRE(expected.TryAppend("/U_Shared"));
        AppendHashSuffix("/Game/My Props/_Shared", expected);
        CHECK(Maps(allocator, "/Game/My Props/_Shared._Shared", expected.View(), true));

        // Windows 예약 이름(대소문자 무시, 위첨자 숫자 포함).
        expected.Clear();
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/UCON"));
        AppendHashSuffix("/Game/CON", expected);
        REQUIRE(expected.TryAppend("/Uaux"));
        AppendHashSuffix("/Game/CON/aux", expected);
        CHECK(Maps(allocator, "/Game/CON/aux.aux", expected.View(), true));
        expected.Clear();
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/UCOM\xC2\xB9"));
        AppendHashSuffix("/Game/COM\xC2\xB9", expected);
        REQUIRE(expected.TryAppend("/Rock"));
        CHECK(Maps(allocator, "/Game/COM\xC2\xB9/Rock.Rock", expected.View(), true));

        // 기호·Default_Ignorable(ZWSP는 Cf, CGJ는 Mn이지만 보이지 않음)는 '_'다. 숫자 시작은
        // 그대로.
        expected.Clear();
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/A_B_C_"));
        AppendHashSuffix("/Game/A\xE2\x80\x8B"
                         "B\xCD\x8F"
                         "C+",
                         expected);
        REQUIRE(expected.TryAppend("/9Lives"));
        CHECK(Maps(allocator,
                   "/Game/A\xE2\x80\x8B"
                   "B\xCD\x8F"
                   "C+/9Lives.9Lives",
                   expected.View(), true));

        // NFD 입력은 NFC로 바꾸고 바꿈으로 치지 않는다(é = e + U+0301).
        CHECK(Maps(allocator,
                   "/Game/Caf"
                   "e\xCC\x81"
                   "/Rock.Rock",
                   "/Game/Unreal/Game/Caf\xC3\xA9/Rock", false));

        // 첫 문자가 결합 문자면 'U'와 합성된다(U + U+0301 → Ú).
        expected.Clear();
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/\xC3\x9A"
                                   "abc"));
        AppendHashSuffix("/Game/\xCC\x81"
                         "abc",
                         expected);
        REQUIRE(expected.TryAppend("/Rock"));
        CHECK(Maps(allocator,
                   "/Game/\xCC\x81"
                   "abc/Rock.Rock",
                   expected.View(), true));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 길이(9.1절 4·5): 64 byte 초과 구성 요소는 code point 경계에서 잘라 해시를 붙이고, 64 byte
// 정확히는 그대로, 전체 256 byte 초과는 UnrealPathTooLong.
TEST(UnrealExchange_MapPathLength) {
    SystemAllocator allocator;
    {
        StringBuilder<512> path(allocator);
        REQUIRE(path.TryAppend("/Game/"));
        REQUIRE(path.TryAppendRepeated('a', 64));
        REQUIRE(path.TryAppend(".x"));
        StringBuilder<256> expected(allocator);
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/"));
        REQUIRE(expected.TryAppendRepeated('a', 64));
        CHECK(Maps(allocator, path.View(), expected.View(), false));

        path.Clear();
        REQUIRE(path.TryAppend("/Game/"));
        REQUIRE(path.TryAppendRepeated('a', 70));
        REQUIRE(path.TryAppend(".x"));
        expected.Clear();
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/"));
        REQUIRE(expected.TryAppendRepeated('a', 55));
        AppendHashSuffix(StringView(path.View().Data(), path.ByteCount() - 2), expected);
        CHECK(Maps(allocator, path.View(), expected.View(), true));

        // 3 byte 글자 30개(90 byte) → 18글자(54 byte) + 접미 = 63 byte.
        path.Clear();
        REQUIRE(path.TryAppend("/Game/"));
        for (uint32 index = 0; index < 30; ++index) {
            REQUIRE(path.TryAppend("\xEA\xB0\x80"));
        }

        REQUIRE(path.TryAppend(".x"));
        expected.Clear();
        REQUIRE(expected.TryAppend("/Game/Unreal/Game/"));
        for (uint32 index = 0; index < 18; ++index) {
            REQUIRE(expected.TryAppend("\xEA\xB0\x80"));
        }

        AppendHashSuffix(StringView(path.View().Data(), path.ByteCount() - 2), expected);
        CHECK(Maps(allocator, path.View(), expected.View(), true));

        // 전체 길이: 256 byte는 받고 257 byte는 TooLong(out은 그대로).
        path.Clear();
        REQUIRE(path.TryAppend("/Game"));
        for (uint32 index = 0; index < 3; ++index) {
            REQUIRE(path.TryAppendAscii('/'));
            REQUIRE(path.TryAppendRepeated('b', 64));
        }

        REQUIRE(path.TryAppendAscii('/'));
        REQUIRE(path.TryAppendRepeated('c', 256 - 12 - 5 - 3 * 65 - 1));
        REQUIRE(path.TryAppend(".x"));
        StringBuilder<512> out(allocator);
        ExchangePathOutcome outcome;
        REQUIRE(MapUnrealObjectPath(path.View(), DefaultRoot, out, outcome));
        CHECK(out.ByteCount() == UnrealPathMaxBytes);
        path.Clear();
        REQUIRE(path.TryAppend("/Game"));
        for (uint32 index = 0; index < 3; ++index) {
            REQUIRE(path.TryAppendAscii('/'));
            REQUIRE(path.TryAppendRepeated('b', 64));
        }

        REQUIRE(path.TryAppendAscii('/'));
        REQUIRE(path.TryAppendRepeated('c', 256 - 12 - 5 - 3 * 65));
        REQUIRE(path.TryAppend(".x"));
        out.Clear();
        REQUIRE(out.TryAppend("keep"));
        CHECK(MapUnrealObjectPath(path.View(), DefaultRoot, out, outcome).Error() ==
              ErrorCode::CapacityExceeded);
        CHECK(outcome.failure == ExchangePathOutcome::Failure::TooLong);
        CHECK(out.View() == StringView("keep"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 잘못된 objectPath·root와 할당 실패: 오류 이유, out 보존, 누수 없음.
TEST(UnrealExchange_MapPathFailures) {
    SystemAllocator allocator;
    {
        StringBuilder<64> out(allocator);
        ExchangePathOutcome outcome;
        CHECK(MapUnrealObjectPath("/Game/Rock", DefaultRoot, out, outcome).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(outcome.failure == ExchangePathOutcome::Failure::InvalidObjectPath);
        CHECK(MapUnrealObjectPath("/Game/Rock.Rock", "/Game/", out, outcome).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(outcome.failure == ExchangePathOutcome::Failure::InvalidRoot);
        CHECK(out.IsEmpty());

        StringBuilder<256> expected(allocator);
        REQUIRE(MapUnrealObjectPath("/Game/My Props/\xCC\x81 x.x", DefaultRoot, expected, outcome));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            StringBuilder<8> small(failing);
            failing.FailOnAttempt(attempt);
            ExchangePathOutcome result;
            auto made =
                MapUnrealObjectPath("/Game/My Props/\xCC\x81 x.x", DefaultRoot, small, result);
            if (!made) {
                CHECK(made.Error() == ErrorCode::OutOfMemory && small.IsEmpty());
                ++failures;
                continue;
            }

            CHECK(small.View() == expected.View());
            if (failing.Attempts() < attempt) {
                break;
            }
        }

        CHECK(failures > 3);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 퍼징: 구성 요소를 까다로운 code point로 만들면 대응 결과는 결정적이고, 성공하면 구성 요소마다
// ValidateNameComponent를 통과하며 전체는 256 byte 이하다.
TEST(UnrealExchange_MapPathFuzz) {
    SystemAllocator allocator;
    {
        const StringView pieces[] = {"a",
                                     "Z",
                                     "7",
                                     "_",
                                     "-",
                                     " ",
                                     "+",
                                     "!",
                                     "\xCC\x81",
                                     "\xCD\x8F",
                                     "\xE2\x80\x8B",
                                     "\xEA\xB0\x80",
                                     "\xE1\x84\x80\xE1\x85\xA1",
                                     "\xF0\x9F\x98\x80",
                                     "\xC2\xB9",
                                     "CON",
                                     "lpt",
                                     "e\xCC\x81",
                                     "\xE1\x85\x9F",
                                     "\xEF\xBC\xA1"};
        constexpr uint32 PieceCount = sizeof(pieces) / sizeof(pieces[0]);
        RandomStream random(PathFuzzSeed);
        uint32 mapped = 0;
        uint32 renamed = 0;
        for (uint32 iteration = 0; iteration < PathFuzzCount; ++iteration) {
            StringBuilder<512> path(allocator);
            const uint32 components = 2 + random.NextUInt32Below(3);
            for (uint32 component = 0; component < components; ++component) {
                REQUIRE(path.TryAppendAscii('/'));
                const uint32 length = 1 + random.NextUInt32Below(component == 0 ? 4 : 30);
                for (uint32 at = 0; at < length; ++at) {
                    REQUIRE(path.TryAppend(pieces[random.NextUInt32Below(PieceCount)]));
                }
            }

            REQUIRE(path.TryAppend(".o"));
            StringBuilder<256> first(allocator);
            StringBuilder<256> second(allocator);
            ExchangePathOutcome outcome;
            ExchangePathOutcome again;
            auto made = MapUnrealObjectPath(path.View(), DefaultRoot, first, outcome);
            auto remade = MapUnrealObjectPath(path.View(), DefaultRoot, second, again);
            CHECK(static_cast<bool>(made) == static_cast<bool>(remade));
            if (!made) {
                CHECK(made.Error() == ErrorCode::CapacityExceeded &&
                      outcome.failure == ExchangePathOutcome::Failure::TooLong);
                continue;
            }

            ++mapped;
            renamed += outcome.renamed ? 1 : 0;
            CHECK(first.View() == second.View() && outcome.renamed == again.renamed);
            CHECK(first.ByteCount() <= UnrealPathMaxBytes);
            const StringView result = first.View();
            usize start = DefaultRoot.ByteCount() + 1;
            for (usize at = start; at <= result.ByteCount(); ++at) {
                if (at != result.ByteCount() && result.Data()[at] != '/') {
                    continue;
                }

                NameComponentError reason = NameComponentError::None;
                CHECK(ValidateNameComponent(StringView(result.Data() + start, at - start),
                                            allocator, &reason));
                CHECK(reason == NameComponentError::None);
                start = at + 1;
            }
        }

        CHECK(mapped > PathFuzzCount / 2 && renamed > PathFuzzCount / 4);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_paths_tests_file
