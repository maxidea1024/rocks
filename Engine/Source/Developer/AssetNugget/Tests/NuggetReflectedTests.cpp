#include "../../../Runtime/AssetRuntime/Tests/AssetTestSupport.h"

#include <AssetNugget/NuggetReflected.h>
#include <AssetTestFixtures/NuggetAssets.generated.h>
#include <AssetTestFixtures/NuggetAssets.h>
#include <Platform/Math/GenericPlatformMath.h>
#include <TestSupport/NFailAllocator.h>

// 리플렉션 값의 너겟 키 기록 시험이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 2 — WriteReflected):
// 모든 필드의 변이(값을 바꾸면 키가 바뀐다 — 필드 하나씩), Transient는 키 밖, Map·Set 저장 순서
// 무관, NaN payload 무관·-0 구분, 필드 이름이 키에 들어감, Optional unset·set, InstancedStruct,
// 추적 golden, 거부(Text·객체 참조·이미 실패한 기록기), 할당 실패.
using namespace rocks;
namespace nugget_reflected_tests_file {

using namespace rocks::asset_test;
using asset_fixture::KeyedAlpha;
using asset_fixture::KeyedBeta;
using asset_fixture::KeyedMode;
using asset_fixture::KeyedPoint;
using asset_fixture::KeyedSettings;
using asset_fixture::KeyedUnsupported;
using reflection_text::AssignmentTarget;

namespace {

// struct 값 T의 키다(Value 대상).
template <class T>
Result<Sha256Digest> KeyOf(AssetEnv& env, T& value, Allocator& allocator,
                           NuggetKeyTrace* trace = nullptr) noexcept {
    auto lease = env.types.AcquireValueType(GeneratedType<T>::Id(), TypeKind::Struct);
    if (!lease) {
        return Result<Sha256Digest>::Failure(lease.Error());
    }

    NuggetKeyWriter writer;
    writer.SetTrace(trace);
    const AssignmentTarget target = AssignmentTarget::Value(*env.source, env.source->OwnerAccess(),
                                                            lease.Value(), &value, allocator);
    if (auto written = WriteReflected(writer, target); !written) {
        CHECK(writer.Finish().Error() == written.Error());
        return Result<Sha256Digest>::Failure(written.Error());
    }

    return writer.Finish();
}

class LineTrace final : public NuggetKeyTrace {
public:
    explicit LineTrace(Allocator& allocator) noexcept : text(allocator) {
    }

    void Line(StringView line) noexcept override {
        CHECK(text.TryAppend(line) && text.TryAppend("\n"));
    }

    String text;
};

Name Intern(AssetEnv& env, StringView text) noexcept {
    auto name = env.names.TryIntern(text);
    CHECK(name);
    return name ? name.Value() : Name{};
}

} // namespace

// 필드 하나씩 바꾸면 키가 바뀐다(모든 값 종류), 같은 값이면 같은 키, Transient는 키 밖.
TEST(NuggetReflected_EveryFieldChangesTheKey) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyedSettings base(env.allocator);
    auto baseKey = KeyOf(env, base, env.allocator);
    REQUIRE(baseKey);
    {
        KeyedSettings same(env.allocator);
        auto again = KeyOf(env, same, env.allocator);
        REQUIRE(again);
        CHECK(again.Value() == baseKey.Value());
    }

    using Mutation = void (*)(AssetEnv&, KeyedSettings&) noexcept;
    const Mutation mutations[] = {
        [](AssetEnv&, KeyedSettings& s) noexcept { s.enabled = true; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.tiny = -1; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.narrow = -2; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.offset = -3; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.small = 4; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.count = 5; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.medium = 1; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.wide = -5; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.big = 1; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.ratio = 0.5f; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.precise = 0.25; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.id = Guid{1, 2}; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.asset = FixedId(7); },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.digest.bytes[3] = 1; },
        [](AssetEnv&, KeyedSettings& s) noexcept { CHECK(s.label.TryAssign("x")); },
        [](AssetEnv& e, KeyedSettings& s) noexcept { s.tag = Intern(e, "hero"); },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.mode = KeyedMode::Best; },
        [](AssetEnv&, KeyedSettings& s) noexcept { CHECK(s.source.TryAssign(FixedId(9), "")); },
        [](AssetEnv&, KeyedSettings& s) noexcept { CHECK(s.source.TryAssign(FixedId(9), "Body")); },
        [](AssetEnv&, KeyedSettings& s) noexcept { CHECK(s.numbers.TryPushBack(0)); },
        [](AssetEnv&, KeyedSettings& s) noexcept { CHECK(s.points.TryPushBack(KeyedPoint{})); },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.triple[2] = 1; },
        [](AssetEnv&, KeyedSettings& s) noexcept { CHECK(s.payload.TryPushBack(0)); },
        [](AssetEnv& e, KeyedSettings& s) noexcept { CHECK(s.tags.Add(Intern(e, "a"))); },
        [](AssetEnv& e, KeyedSettings& s) noexcept {
            String key(e.allocator);
            CHECK(key.TryAssign("k") && s.scores.Insert(Move(key), 0));
        },
        [](AssetEnv&, KeyedSettings& s) noexcept { CHECK(s.byIndex.Insert(0, KeyedPoint{})); },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.limit = 0; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.anchor = KeyedPoint{}; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.origin.x = 1; },
        [](AssetEnv&, KeyedSettings& s) noexcept { s.origin.y = 1; },
        [](AssetEnv& e, KeyedSettings& s) noexcept {
            CHECK(s.boxed.Emplace<KeyedPoint>(e.types, e.allocator));
        }};
    Array<Sha256Digest> seen(env.allocator);
    REQUIRE(seen.TryPushBack(baseKey.Value()));
    for (Mutation mutate : mutations) {
        KeyedSettings changed(env.allocator);
        mutate(env, changed);
        auto key = KeyOf(env, changed, env.allocator);
        REQUIRE(key);
        // 모든 변이의 키가 서로·기준과 다르다.
        for (const Sha256Digest& other : seen) {
            CHECK(!(key.Value() == other));
        }

        REQUIRE(seen.TryPushBack(key.Value()));
        changed.boxed.Reset();
    }

    // Transient는 키 밖이다.
    KeyedSettings transient(env.allocator);
    transient.cache = 99;
    auto transientKey = KeyOf(env, transient, env.allocator);
    REQUIRE(transientKey);
    CHECK(transientKey.Value() == baseKey.Value());
}

// Map·Set 저장 순서와 무관(정규 인코딩 byte 사전순), NaN payload 무관, -0과 +0은 다르다, Optional
// unset과 set(0)은 다르다, 필드 이름만 다르면 다르다, 같은 값의 struct 값 Map(값이 struct)도 순서
// 무관.
TEST(NuggetReflected_CanonicalOrderAndValues) {
    AssetEnv env;
    REQUIRE(env.Ready());
    auto keyed = [&](auto&& fill) noexcept {
        KeyedSettings settings(env.allocator);
        fill(settings);
        auto key = KeyOf(env, settings, env.allocator);
        CHECK(key);
        settings.boxed.Reset();
        return key ? key.Value() : Sha256Digest{};
    };
    auto scores = [&](KeyedSettings& s, StringView first, StringView second) noexcept {
        String a(env.allocator);
        String b(env.allocator);
        CHECK(a.TryAssign(first) && b.TryAssign(second));
        CHECK(s.scores.Insert(Move(a), 1) && s.scores.Insert(Move(b), 2));
    };
    CHECK(keyed([&](KeyedSettings& s) noexcept { scores(s, "a", "b"); }) ==
          keyed([&](KeyedSettings& s) noexcept {
              String b(env.allocator);
              String a(env.allocator);
              CHECK(b.TryAssign("b") && a.TryAssign("a"));
              CHECK(s.scores.Insert(Move(b), 2) && s.scores.Insert(Move(a), 1));
          }));
    // 값이 다르면 다르다(키만 같은 것이 아니다).
    CHECK(!(keyed([&](KeyedSettings& s) noexcept { scores(s, "a", "b"); }) ==
            keyed([&](KeyedSettings& s) noexcept { scores(s, "b", "a"); })));
    CHECK(keyed([&](KeyedSettings& s) noexcept {
              CHECK(s.tags.Add(Intern(env, "x")) && s.tags.Add(Intern(env, "y")));
          }) == keyed([&](KeyedSettings& s) noexcept {
              CHECK(s.tags.Add(Intern(env, "y")) && s.tags.Add(Intern(env, "x")));
          }));
    CHECK(keyed([](KeyedSettings& s) noexcept {
              KeyedPoint p{};
              p.x = 1;
              CHECK(s.byIndex.Insert(3, p) && s.byIndex.Insert(1, KeyedPoint{}));
          }) == keyed([](KeyedSettings& s) noexcept {
              KeyedPoint p{};
              p.x = 1;
              CHECK(s.byIndex.Insert(1, KeyedPoint{}) && s.byIndex.Insert(3, p));
          }));
    // 배열은 순서가 결과다.
    CHECK(!(keyed([](KeyedSettings& s) noexcept {
                CHECK(s.numbers.TryPushBack(1) && s.numbers.TryPushBack(2));
            }) == keyed([](KeyedSettings& s) noexcept {
                CHECK(s.numbers.TryPushBack(2) && s.numbers.TryPushBack(1));
            })));
    CHECK(
        keyed([](KeyedSettings& s) noexcept { s.ratio = platform::BitCast<float>(0x7fc00001u); }) ==
        keyed([](KeyedSettings& s) noexcept { s.ratio = platform::BitCast<float>(0xffc00000u); }));
    CHECK(keyed([](KeyedSettings& s) noexcept {
              s.precise = platform::BitCast<double>(0x7ff8000000000001ull);
          }) == keyed([](KeyedSettings& s) noexcept {
              s.precise = platform::BitCast<double>(0xfff8000000000000ull);
          }));
    CHECK(!(keyed([](KeyedSettings& s) noexcept { s.ratio = 0.0f; }) ==
            keyed([](KeyedSettings& s) noexcept { s.ratio = -0.0f; })));
    CHECK(!(keyed([](KeyedSettings&) noexcept {}) ==
            keyed([](KeyedSettings& s) noexcept { s.limit = 0; })));
    CHECK(!(keyed([](KeyedSettings& s) noexcept { s.anchor = KeyedPoint{}; }) ==
            keyed([&](KeyedSettings& s) noexcept {
                CHECK(s.boxed.Emplace<KeyedPoint>(env.types, env.allocator));
            })));

    KeyedAlpha alpha;
    KeyedBeta beta;
    alpha.alpha = 3;
    beta.beta = 3;
    auto alphaKey = KeyOf(env, alpha, env.allocator);
    auto betaKey = KeyOf(env, beta, env.allocator);
    REQUIRE(alphaKey && betaKey);
    CHECK(!(alphaKey.Value() == betaKey.Value()));
}

// Name은 ASCII 소문자로 접어 쓴다(ASCII 대소문자 무시 동등 — 표시 철자는 먼저 등록한 쪽).
TEST(NuggetReflected_NameIsCaseFolded) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyedSettings settings(env.allocator);
    settings.tag = Intern(env, "MiXed9_Name");
    LineTrace trace(env.allocator);
    REQUIRE(KeyOf(env, settings, env.allocator, &trace));
    CHECK(StringView(trace.text.View()).Contains("field\ntag\nstring\nmixed9_name\n"));
}

// 추적 golden: struct·Field·값 줄 순서(KeyedAlpha).
TEST(NuggetReflected_TraceGolden) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyedAlpha alpha;
    alpha.alpha = -4;
    LineTrace trace(env.allocator);
    REQUIRE(KeyOf(env, alpha, env.allocator, &trace));
    CHECK(trace.text.View() == "struct\nfield\nalpha\ni64 -4\nend\n");
}

// 거부: Text(지역화 원문이 바뀔 수 있다)는 Unsupported(기록기도 그 오류), ClassRef는 TypeId로 쓴다
// (null과 값이 다르다), 객체 참조(살아 있는 객체의 Hard·Weak 필드)는 Unsupported, 이미 실패한
// 기록기는 순회하지 않고 그 오류다.
TEST(NuggetReflected_Rejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    {
        KeyedUnsupported value;
        CHECK(KeyOf(env, value, env.allocator).Error() == ErrorCode::Unsupported);
    }

    // ClassRef만 따로 본다(Text를 빼고 — 키는 KeyedUnsupported의 일부까지 쓴 뒤 Text에서 실패하므로
    // ClassRef의 쓰기 자체는 실패하지 않는다).
    {
        auto lease =
            env.types.AcquireValueType(GeneratedType<KeyedUnsupported>::Id(), TypeKind::Struct);
        REQUIRE(lease);
        KeyedUnsupported value;
        value.kind = ClassRef<Asset>(GeneratedType<TestAsset>::Id());
        LineTrace trace(env.allocator);
        NuggetKeyWriter writer;
        writer.SetTrace(&trace);
        CHECK(WriteReflected(writer, AssignmentTarget::Value(*env.source, env.source->OwnerAccess(),
                                                             lease.Value(), &value, env.allocator))
                  .Error() == ErrorCode::Unsupported);
        char hex[48] = {};
        FixedStringBuilder guid{Span<char>(hex)};
        REQUIRE(guid.TryAppendFormat("guid {}", GeneratedType<TestAsset>::Id()));
        CHECK(StringView(trace.text.View()).Contains(guid.View()));
    }

    // 살아 있는 객체(TestAsset — Hard·Weak 참조 필드)는 Unsupported다.
    {
        auto package =
            NewPackage(*env.source, env.source->OwnerAccess(), FixedId(5), PackageKind::Editor);
        REQUIRE(package);
        const ObjectHandle asset =
            Make<TestAsset>(*env.source, TestAssetType(), package.Value().Handle(), "");
        REQUIRE(!asset.IsNull());
        NuggetKeyWriter writer;
        CHECK(WriteReflected(writer,
                             AssignmentTarget::Live(*env.source, env.source->OwnerAccess(), asset))
                  .Error() == ErrorCode::Unsupported);
        CHECK(writer.Finish().Error() == ErrorCode::Unsupported);
    }

    {
        auto lease = env.types.AcquireValueType(GeneratedType<KeyedAlpha>::Id(), TypeKind::Struct);
        REQUIRE(lease);
        KeyedAlpha alpha;
        LineTrace trace(env.allocator);
        NuggetKeyWriter writer;
        writer.Fail(ErrorCode::Busy);
        writer.SetTrace(&trace);
        CHECK(WriteReflected(writer, AssignmentTarget::Value(*env.source, env.source->OwnerAccess(),
                                                             lease.Value(), &alpha, env.allocator))
                  .Error() == ErrorCode::Busy);
        CHECK(trace.text.IsEmpty());
    }
}

// 할당 실패(순회의 경로·값·Map·Set 정렬 항목): 모두 OutOfMemory이고 실패 없는 키는 기준과 같다.
TEST(NuggetReflected_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyedSettings settings(env.allocator);
    CHECK(settings.label.TryAssign("label") && settings.numbers.TryPushBack(1) &&
          settings.points.TryPushBack(KeyedPoint{}) && settings.payload.TryPushBack(4) &&
          settings.tags.Add(Intern(env, "t")));
    String key(env.allocator);
    REQUIRE(key.TryAssign("k") && settings.scores.Insert(Move(key), 1));
    REQUIRE(settings.byIndex.Insert(2, KeyedPoint{}));
    settings.anchor = KeyedPoint{};
    REQUIRE(settings.boxed.Emplace<KeyedPoint>(env.types, env.allocator));
    auto expected = KeyOf(env, settings, env.allocator);
    REQUIRE(expected);
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto got = KeyOf(env, settings, failing);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(got);
            CHECK(got.Value() == expected.Value());
            break;
        }

        CHECK(got.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
    settings.boxed.Reset();
}

// Name 필드: Name pool 없는 Registry는 Unsupported, 다른 pool의 토큰은 Acquire 오류다.
TEST(NuggetReflected_NameNeedsThePool) {
    AssetEnv env;
    REQUIRE(env.Ready());
    auto lease = env.types.AcquireValueType(GeneratedType<KeyedSettings>::Id(), TypeKind::Struct);
    REQUIRE(lease);
    {
        ObjectRegistry bare(env.allocator, env.types, ContextId{0x4e554747ull, 2});
        KeyedSettings settings(env.allocator);
        NuggetKeyWriter writer;
        CHECK(
            WriteReflected(writer, AssignmentTarget::Value(bare, bare.OwnerAccess(), lease.Value(),
                                                           &settings, env.allocator))
                .Error() == ErrorCode::Unsupported);
        REQUIRE(bare.Shutdown(bare.OwnerAccess()));
    }

    NamePool other(env.allocator);
    REQUIRE(other.Initialize());
    auto foreign = other.TryIntern("only-in-other-pool-with-a-long-name");
    REQUIRE(foreign);
    KeyedSettings settings(env.allocator);
    settings.tag = foreign.Value();
    NuggetKeyWriter writer;
    CHECK(
        !WriteReflected(writer, AssignmentTarget::Value(*env.source, env.source->OwnerAccess(),
                                                        lease.Value(), &settings, env.allocator)));
    CHECK(!writer.Finish());
    CHECK(other.Shutdown());
}

} // namespace nugget_reflected_tests_file
