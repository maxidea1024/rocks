#include "PackageEditing.h"

#include <ObjectSerialization/ClassDefaults.h>

// 클래스 기본값 지문 시험이다(Docs/Design/RuntimeClassMetadata.ko.md 'A5c 지문 연결', AssetSystem
// 5.2절 Summary·5.3절 타입 데이터 축): Summary ClassDefaults 행(레코드 있는 export 클래스마다 그
// 패키지 종류 구성의 지문, 행 ⊆ export 타입), 행이 export 밖이면 손상, 로드 판정(쿠킹은 지문 다름·
// 행 없음 UnsupportedVersion, 에디터는 changedDefaults에 담고 계속 연다), 낮은 formatVersion(v1)
// 거부. 섹션은 EditablePackage로 writer 규칙대로 다시 배치한다.
using namespace rocks;
namespace package_class_defaults_tests_file {

using namespace rocks::asset_test;
using namespace rocks::package_detail;

namespace {

// 대표 에셋(TestAsset) + 하위 객체 Part(TestPart)인 패키지다(export 클래스 둘).
struct Pair {
    explicit Pair(Allocator& allocator) noexcept : cooked(allocator), editor(allocator) {
    }

    Array<uint8> cooked;
    Array<uint8> editor;
};

bool MakePair(AssetEnv& env, Pair& pair) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto package = NewPackage(registry, access, FixedId(700), PackageKind::Editor);
    if (!package) {
        return false;
    }

    const ObjectHandle asset =
        Make<TestAsset>(registry, TestAssetType(), package.Value().Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), asset, "Part");
    if (asset.IsNull() || part.IsNull()) {
        return false;
    }

    pair.cooked = Write(env.allocator, registry, package.Value().Handle(), CookedOptions());
    pair.editor = Write(env.allocator, registry, package.Value().Handle(), EditorOptions());
    return !pair.cooked.IsEmpty() && !pair.editor.IsEmpty();
}

Result<PackageContents> ReadWith(AssetEnv& env, const Array<uint8>& bytes, PackageKind kind,
                                 bool withTypes, AssetDiagnostic* failure = nullptr) noexcept {
    PackageReadOptions options;
    options.expectedKind = kind;
    options.engineVersion = TestEngineVersion;
    options.types = withTypes ? &env.types : nullptr;
    return ReadPackage(env.allocator, bytes.View(), options, failure);
}

// Summary를 types·classDefaults로 다시 만든 패키지다.
Array<uint8> WithSummary(Allocator& allocator, const Array<uint8>& file, bool editor,
                         Span<const PackageTypeVersion> types,
                         Span<const PackageClassDefaults> classDefaults) noexcept {
    SummaryInput input{types, Span<const PackageCustomVersion>(), editor};
    input.classDefaults = classDefaults;
    Array<uint8> summary = SummaryBytes(allocator, input, SummaryFault::None);
    EditablePackage edit(allocator);
    if (summary.IsEmpty() || !edit.Parse(file.View()) ||
        !edit.Replace(PackageSectionKind::Summary, summary.View())) {
        return Array<uint8>(allocator);
    }

    return edit.Build();
}

} // namespace

// 행: 레코드 있는 export 클래스(TestAsset·TestPart)마다 한 행, TypeId 오름차순, 타입 표의 부분집합.
// 지문은 그 패키지 종류 구성의 지금 레코드 지문이다(에디터 Editor, 쿠킹 Cooked — 둘은 다르다).
TEST(AssetPackage_ClassDefaultsRows) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Pair pair(env.allocator);
    REQUIRE(MakePair(env, pair));
    const bool editorValues[] = {false, true};
    for (bool editor : editorValues) {
        const PackageKind kind = editor ? PackageKind::Editor : PackageKind::Cooked;
        auto read = ReadWith(env, editor ? pair.editor : pair.cooked, kind, false);
        REQUIRE(read);
        const PackageSummary& summary = read.Value().summary;
        REQUIRE(summary.classDefaults.Size() == 2);
        REQUIRE(summary.types.Size() == 2);
        CHECK(GuidLess(summary.classDefaults[0].type, summary.classDefaults[1].type));
        for (usize index = 0; index < summary.classDefaults.Size(); ++index) {
            const PackageClassDefaults& row = summary.classDefaults[index];
            CHECK(row.type == summary.types[index].type);
            auto lease = env.types.Acquire(row.type);
            REQUIRE(lease);
            auto defaults = AcquireClassDefaults(lease.Value(), env.allocator);
            REQUIRE(defaults);
            CHECK(row.fingerprint ==
                  defaults.Value()->Fingerprint(editor ? ClassDefaultsConfiguration::Editor
                                                       : ClassDefaultsConfiguration::Cooked));
        }
    }

    auto cooked = ReadWith(env, pair.cooked, PackageKind::Cooked, false);
    auto editorRead = ReadWith(env, pair.editor, PackageKind::Editor, false);
    REQUIRE(cooked && editorRead);
    CHECK(!(cooked.Value().summary.classDefaults[0].fingerprint ==
            editorRead.Value().summary.classDefaults[0].fingerprint));
}

// 형식: export 밖 TypeId 행은 Summary 손상이다(타입 축 판정 전 — types 없이 읽어도).
TEST(AssetPackage_ClassDefaultsOutsideExports) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Pair pair(env.allocator);
    REQUIRE(MakePair(env, pair));
    Allocator& allocator = env.allocator;
    auto read = ReadWith(env, pair.cooked, PackageKind::Cooked, false);
    REQUIRE(read);
    Array<PackageClassDefaults> rows = CopyEntries(allocator, read.Value().summary.classDefaults);
    REQUIRE(rows.TryPushBack(PackageClassDefaults{Guid{NumericLimits<uint64>::Max(), 1}, {}}));
    const Array<uint8> bytes =
        WithSummary(allocator, pair.cooked, false, read.Value().summary.types.View(), rows.View());
    REQUIRE(!bytes.IsEmpty());
    AssetDiagnostic failure;
    auto outside = ReadWith(env, bytes, PackageKind::Cooked, false, &failure);
    CHECK(!outside && failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.section == static_cast<uint32>(PackageSectionKind::Summary));
}

// 로드 판정: 저장 지문이 지금과 다르거나(바이트 하나) 행이 없으면(지금은 레코드 있음) 쿠킹은
// Summary UnsupportedVersion, 에디터는 읽기 전용이 아닌 채 changedDefaults에 그 타입을 담는다.
// 그대로면 둘 다 비어 있다. types 없이 읽으면 판정하지 않는다.
TEST(AssetPackage_ClassDefaultsJudgement) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Pair pair(env.allocator);
    REQUIRE(MakePair(env, pair));
    Allocator& allocator = env.allocator;
    const bool editorValues[] = {false, true};
    for (bool editor : editorValues) {
        const PackageKind kind = editor ? PackageKind::Editor : PackageKind::Cooked;
        const Array<uint8>& file = editor ? pair.editor : pair.cooked;
        auto same = ReadWith(env, file, kind, true);
        REQUIRE(same);
        CHECK(same.Value().changedDefaults.IsEmpty());
        CHECK(same.Value().unavailableDefaults.IsEmpty());
        const PackageSummary& summary = same.Value().summary;

        Array<PackageClassDefaults> flipped = CopyEntries(allocator, summary.classDefaults);
        flipped[1].fingerprint.bytes[0] ^= 1;
        Array<PackageClassDefaults> dropped(allocator);
        REQUIRE(dropped.TryPushBack(summary.classDefaults[0]));
        const Span<const PackageClassDefaults> variants[] = {flipped.View(), dropped.View()};
        for (Span<const PackageClassDefaults> rows : variants) {
            const Array<uint8> bytes =
                WithSummary(allocator, file, editor, summary.types.View(), rows);
            REQUIRE(!bytes.IsEmpty());
            AssetDiagnostic failure;
            auto judged = ReadWith(env, bytes, kind, true, &failure);
            if (!editor) {
                CHECK(!judged && failure.code == AssetDiagnosticCode::UnsupportedVersion &&
                      failure.error == ErrorCode::Unsupported &&
                      failure.section == static_cast<uint32>(PackageSectionKind::Summary));
            } else {
                REQUIRE(judged);
                CHECK(!judged.Value().readOnly);
                REQUIRE(judged.Value().changedDefaults.Size() == 1);
                CHECK(judged.Value().changedDefaults[0] == summary.types[1].type);
                CHECK(judged.Value().unavailableDefaults.IsEmpty());
            }

            // 타입 축을 보지 않는 도구 읽기는 판정하지 않는다.
            auto plain = ReadWith(env, bytes, kind, false);
            REQUIRE(plain);
            CHECK(plain.Value().changedDefaults.IsEmpty());
        }
    }
}

// 형식 버전: 낮은 formatVersion(v1 — 차이 형식·클래스 기본값 목록 이전)은 에디터·쿠킹 모두
// UnsupportedVersion이다(하위호환 없음).
TEST(AssetPackage_FormatVersionOneRejected) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Pair pair(env.allocator);
    REQUIRE(MakePair(env, pair));
    const bool editorValues[] = {false, true};
    for (bool editor : editorValues) {
        const PackageKind kind = editor ? PackageKind::Editor : PackageKind::Cooked;
        EditablePackage edit(env.allocator);
        REQUIRE(edit.Parse((editor ? pair.editor : pair.cooked).View()));
        StoreLittleEndian<uint16>(edit.header + 4, 1);
        const Array<uint8> old = edit.Build();
        AssetDiagnostic failure;
        auto read = ReadWith(env, old, kind, false, &failure);
        CHECK(!read && failure.code == AssetDiagnosticCode::UnsupportedVersion &&
              failure.error == ErrorCode::Unsupported);
    }
}

} // namespace package_class_defaults_tests_file
