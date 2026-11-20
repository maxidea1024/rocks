#include "EngineAssetsTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// EngineAssets 콘텐츠 서명 정책 시험이다(Docs/Design/ContentSigning.ko.md 7절 4·7 — A1f): 쿠킹
// 루트의 봉투·rman·신뢰 등록 조합, 봉투 판정 단계(버전·scope·잘못된 신뢰 정보)가 엔진 초기화 결과와
// 기록 한 줄로 드러나는지, Release 구성은 봉투만 받는지를 본다. 구성(Release 여부)은 컴파일 때
// 정해지므로 기대값을 IsReleaseBuild로 고르고 Debug·Release 시험 실행이 양쪽을 덮는다.
using namespace rocks;

namespace engine_assets_signing_tests_file {

using namespace rocks::asset_test;
using namespace rocks::engine_assets_test;
using rocks::test::config::ConfigFixture;
using rocks::test::config::DevelopmentRules;

namespace {

constexpr const char* NotSignedLog =
    "Cooked content is not signed: '{Root}' has no {File}; reading {Manifest}";

enum class EnvelopeState : uint8 {
    None,
    Valid,
    Tampered
};

struct Outcome {
    ErrorCode error = ErrorCode::None;
    const char* stage = "";
    const char* diagnostic = "";
};

// 초기화 결과와 기록을 확인한다. 실패면 단계·오류·진단 이름을 담은 Error 한 줄, 성공이면 시작 Info
// 한 줄이다. notSignedWarnings는 서명 안 됨 Warning 수다.
bool LoggedAs(const Outcome& want, uint32 notSignedWarnings) noexcept {
    bool same = CapturedLogs.Count(LogLevel::Warning, NotSignedLog) == notSignedWarnings;
    if (want.error == ErrorCode::None) {
        same = same && CapturedLogs.Count(LogLevel::Error, InitFailedLog) == 0 &&
               CapturedLogs.Count(LogLevel::Info, StartedLog) == 1;
    } else {
        char prefix[160] = {};
        snprintf(prefix, sizeof(prefix),
                 "EngineAssets initialization failed at %s: %s (diagnostic %s,", want.stage,
                 ErrorCodeName(want.error), want.diagnostic);
        const char* message = CapturedLogs.Message(LogLevel::Error, InitFailedLog);
        same = same && CapturedLogs.Count(LogLevel::Error, InitFailedLog) == 1 &&
               strncmp(message, prefix, strlen(prefix)) == 0 &&
               CapturedLogs.Count(LogLevel::Info, StartedLog) == 0;
        if (!same) {
            printf("  expected prefix: %s\n", prefix);
        }
    }

    if (!same) {
        CapturedLogs.Print();
    }

    return same;
}

} // namespace

// 서명 정책 조합(7절 4·7, 14 완료 조건 2): 봉투 3(없음·시험 키로 서명·서명 변조) × 신뢰 등록
// 2(있음· 없음) × rman 2(없음·있음) = 12. 봉투가 있을 때의 rman은 깨진 byte라 읽으면
// 실패한다(봉투만 쓰는지 확인). 기대:
//   - 봉투 없음: Release는 NotFound(단계 content signature, 신뢰·rman과 무관). 그 밖은 서명 안 됨
//     Warning 한 번 뒤 rman을 읽는다 — rman이 있으면 성공(서명 아님), 없으면 NotFound(manifest
//     read).
//   - 봉투 있음·신뢰 없음: InvalidArgument(content trust).
//   - 서명 봉투·신뢰 있음: 성공, contentSigned·contentVersion.
//   - 변조 봉투·신뢰 있음: AuthenticationFailed(content signature (Signature), 진단
//   ContentNotTrusted).
TEST(EngineAssets_SigningPolicyCombinations) {
    ManualEngineAssetsLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    uint32 cases = 0;
    for (uint32 envelopeIndex = 0; envelopeIndex < 3; ++envelopeIndex) {
        const auto envelope = static_cast<EnvelopeState>(envelopeIndex);
        for (uint32 trusted = 0; trusted < 2; ++trusted) {
            for (uint32 withManifest = 0; withManifest < 2; ++withManifest) {
                char tag[32] = {};
                snprintf(tag, sizeof(tag), "sign-%u%u%u", envelopeIndex, trusted, withManifest);
                DiskScene disk(tag);
                REQUIRE(disk.Write(false));
                Array<uint8> manifest(disk.env.allocator);
                REQUIRE(WriteCookedManifest(disk.env.allocator, disk.scene.manifest, manifest));
                if (envelope != EnvelopeState::None) {
                    Array<uint8> signedBytes(disk.env.allocator);
                    REQUIRE(SignTestManifest(disk.env.allocator, manifest.View(), signedBytes));
                    if (envelope == EnvelopeState::Tampered) {
                        // 서명 칸의 마지막 byte를 바꾼다(헤더 판정은 모두 통과한다).
                        uint8& last = signedBytes[ContentEnvelopeManifestOffset - 1];
                        last = static_cast<uint8>(last ^ 1u);
                    }

                    REQUIRE(disk.WriteRootFile(ContentEnvelopeFileName, signedBytes.View()));
                }

                if (withManifest != 0) {
                    const uint8 garbage[] = {'R', 'M', 'A', 'N', 9, 9, 9, 9};
                    REQUIRE(disk.WriteRootFile(CookedManifestFileName,
                                               envelope == EnvelopeState::None
                                                   ? manifest.View()
                                                   : Span<const uint8>(garbage)));
                }

                if (trusted == 0) {
                    EngineAssetsSubsystem::SetContentTrust(nullptr);
                }

                ConfigFixture fixture(DevelopmentRules());
                REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
                logs.Pump();
                CapturedLogs.Reset();
                Outcome want;
                uint32 warnings = 0;
                bool signedContent = false;
                if (envelope == EnvelopeState::None) {
                    if constexpr (IsReleaseBuild) {
                        want = {ErrorCode::NotFound, "content signature", "None"};
                    } else {
                        warnings = 1;
                        if (withManifest == 0) {
                            want = {ErrorCode::NotFound, "manifest read", "None"};
                        }
                    }
                } else if (trusted == 0) {
                    want = {ErrorCode::InvalidArgument, "content trust", "None"};
                } else if (envelope == EnvelopeState::Tampered) {
                    want = {ErrorCode::AuthenticationFailed, "content signature (Signature)",
                            "ContentNotTrusted"};
                } else {
                    signedContent = true;
                }

                Engine engine(allocator);
                const auto initialized =
                    engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr));
                CHECK((initialized ? ErrorCode::None : initialized.Error()) == want.error);
                logs.Pump();
                CHECK(LoggedAs(want, warnings));
                if (initialized) {
                    const EngineAssetsStats stats =
                        engine.GetSubsystem<EngineAssetsSubsystem>()->Stats();
                    CHECK(stats.contentSigned == signedContent);
                    CHECK(stats.contentVersion == (signedContent ? TestContentVersion : 0));
                    CHECK(engine.Shutdown());
                } else {
                    CHECK(engine.State() == EngineState::Failed);
                }

                ++cases;
            }
        }
    }

    CHECK(cases == 12);
}

// 봉투 판정 단계(7절 2·4): 시험 키로 서명했어도 contentVersion이 최소보다 낮으면 ContentVersion,
// 다른 scope면 Scope, 같은 버전(최소와 같음)은 통과한다. 등록한 신뢰 정보 자체가 잘못됐으면(키
// 없음) 봉투를 보기 전에 InvalidArgument이고 단계 이름의 칸은 None이다. 매니페스트가 봉투 안에서
// 깨졌으면 봉투는 통과하고 매니페스트 판정이 거부한다(CorruptPackage).
TEST(EngineAssets_SignedContentRejections) {
    ManualEngineAssetsLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    auto otherScope = MakeContentScope("rocks.engine-assets.test", "beta");
    REQUIRE(otherScope);
    const ContentTrust empty{};
    enum class Kind : uint8 {
        LowVersion,
        MinimumVersion,
        OtherScope,
        InvalidTrust,
        CorruptManifest
    };
    struct Case {
        Kind kind;
        Outcome want;
    };
    const Case cases[] = {
        {Kind::LowVersion,
         {ErrorCode::AuthenticationFailed, "content signature (ContentVersion)",
          "ContentNotTrusted"}},
        {Kind::MinimumVersion, {}},
        {Kind::OtherScope,
         {ErrorCode::AuthenticationFailed, "content signature (Scope)", "ContentNotTrusted"}},
        {Kind::InvalidTrust, {ErrorCode::InvalidArgument, "content signature (None)", "None"}},
        {Kind::CorruptManifest, {ErrorCode::InvalidArgument, "manifest parse", "CorruptPackage"}},
    };
    for (const Case& expected : cases) {
        char tag[32] = {};
        snprintf(tag, sizeof(tag), "reject-%u", static_cast<unsigned>(expected.kind));
        DiskScene disk(tag);
        REQUIRE(disk.Write(false));
        Array<uint8> manifest(disk.env.allocator);
        REQUIRE(WriteCookedManifest(disk.env.allocator, disk.scene.manifest, manifest));
        if (expected.kind == Kind::CorruptManifest) {
            manifest[manifest.Size() - 1] = static_cast<uint8>(manifest[manifest.Size() - 1] ^ 1u);
        }

        const uint64 version = expected.kind == Kind::LowVersion ? TestMinimumContentVersion - 1
                               : expected.kind == Kind::MinimumVersion ? TestMinimumContentVersion
                                                                       : TestContentVersion;
        Array<uint8> envelope(disk.env.allocator);
        REQUIRE(
            SignTestManifest(disk.env.allocator, manifest.View(), envelope, version,
                             expected.kind == Kind::OtherScope ? &otherScope.Value() : nullptr));
        REQUIRE(disk.WriteRootFile(ContentEnvelopeFileName, envelope.View()));
        if (expected.kind == Kind::InvalidTrust) {
            EngineAssetsSubsystem::SetContentTrust(&empty);
        }

        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
        logs.Pump();
        CapturedLogs.Reset();
        Engine engine(allocator);
        const auto initialized =
            engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr));
        CHECK((initialized ? ErrorCode::None : initialized.Error()) == expected.want.error);
        logs.Pump();
        CHECK(LoggedAs(expected.want, 0));
        if (initialized) {
            CHECK(engine.GetSubsystem<EngineAssetsSubsystem>()->Stats().contentVersion ==
                  TestMinimumContentVersion);
            CHECK(engine.Shutdown());
        }
    }
}

} // namespace engine_assets_signing_tests_file
