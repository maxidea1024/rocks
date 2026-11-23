#include "AssetTestSupport.h"

#include <AssetTestFixtures/BundleAssets.generated.h>
#include <AssetTestFixtures/BundleAssets.h>

// 에셋 bundle·EditorOnly 클래스의 리플렉션 스키마 시험이다(Docs/Design/AssetSystem 10.2절
// "A4e 구현 세부" — A4e2a): 생성 스키마의 bundle 정규 텍스트(공백 뗌·struct 사슬·Map 키·
// InstancedStruct·지정 없음)와 EditorOnly 클래스 비트, Registry의 깊은 복사, 수동 등록 거부
// (EditorOnly 필드의 목록·정규 목록이 아닌 텍스트).
using namespace rocks;
namespace asset_bundle_schema_tests_file {

using namespace rocks::asset_test;
using asset_fixture::BundleAsset;
using asset_fixture::BundleSlot;
using asset_fixture::EditorNote;
using asset_fixture::EditorToolAsset;

namespace {

StringView Text(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

// schema에서 이름이 name인 필드의 bundle 텍스트다(없으면 "<missing>").
StringView Bundles(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (Text(field.name) == name) {
            return Text(field.assetBundles);
        }
    }

    return "<missing>";
}

} // namespace

TEST(AssetBundleSchema_GeneratedListsAndFlags) {
    const TypeSchema& asset = GeneratedType<BundleAsset>::Schema();
    CHECK(Bundles(asset, "portrait") == "UI,Game");
    CHECK(Bundles(asset, "levels") == "Game");
    CHECK(Bundles(asset, "slot") == "UI");
    CHECK(Bundles(asset, "plain").IsEmpty());
    CHECK(Bundles(asset, "weights") == "Audio");
    CHECK(Bundles(asset, "extra") == "Game");
    CHECK(Bundles(asset, "editorExtra").IsEmpty() && Bundles(asset, "editorSoft").IsEmpty());
    const TypeSchema& slot = GeneratedType<BundleSlot>::Schema();
    CHECK(Bundles(slot, "icon") == "UI,Game" && Bundles(slot, "any").IsEmpty());
    CHECK(HasFlag(GeneratedType<EditorToolAsset>::Schema().flags, TypeFlags::EditorOnly));
    CHECK(HasFlag(GeneratedType<EditorNote>::Schema().flags, TypeFlags::EditorOnly));
    CHECK(!HasFlag(asset.flags, TypeFlags::EditorOnly));

    // Registry는 목록을 깊게 복사한다(생성 저장소를 가리키지 않는다).
    AssetEnv env;
    REQUIRE(env.Ready());
    auto lease = env.types.Acquire(GeneratedType<BundleAsset>::Id());
    REQUIRE(lease);
    const TypeSchema& registered = lease.Value().Schema();
    CHECK(Bundles(registered, "portrait") == "UI,Game" &&
          Bundles(registered, "weights") == "Audio");
    for (usize index = 0; index < registered.fields.Size(); ++index) {
        if (registered.fields[index].assetBundles.Size() != 0) {
            CHECK(registered.fields[index].assetBundles.Data() !=
                  asset.fields[index].assetBundles.Data());
        }
    }

    auto tool = env.types.Acquire(GeneratedType<EditorToolAsset>::Id());
    REQUIRE(tool);
    CHECK(HasFlag(tool.Value().Schema().flags, TypeFlags::EditorOnly));
}

// 수동 등록도 같은 규칙이다: EditorOnly 필드의 목록, 정규 목록이 아닌 텍스트, 키가 소프트 참조가
// 아니고 값도 bundle 대상이 아닌 Map 필드의 목록은 InvalidArgument다(BundleAsset 스키마를 다른
// id·이름으로 복사하고 필드 하나만 바꾼다 — 함수표는 생성 그대로).
TEST(AssetBundleSchema_ManualRegistrationRejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    const TypeSchema& generated = GeneratedType<BundleAsset>::Schema();
    constexpr usize MaxFields = 32;
    REQUIRE(generated.fields.Size() <= MaxFields);
    const auto find = [&](StringView name) noexcept {
        usize index = 0;
        while (Text(generated.fields[index].name) != name) {
            ++index;
        }

        return index;
    };

    const usize portrait = find("portrait");
    const usize weights = find("weights");

    constexpr char CopyName[] = "BundleCopy";
    const auto registers = [&](uint32 extraFlags, const char* bundles,
                               usize target = NumericLimits<usize>::Max(),
                               bool plainKey = false) noexcept {
        FieldSchema fields[MaxFields];
        for (usize index = 0; index < generated.fields.Size(); ++index) {
            fields[index] = generated.fields[index];
        }

        const usize changed = target == NumericLimits<usize>::Max() ? portrait : target;
        fields[changed].flags |= extraFlags;
        fields[changed].assetBundles = Span<const char>(bundles, StringView(bundles).ByteCount());
        // 키를 정수로 바꾸면 Map 값(int32)과 키 모두 bundle 대상이 아니다.
        if (plainKey) {
            fields[changed].key = ValueType{ValueKind::Int32, TypeId{}};
        }
        TypeSchema schema = generated;
        schema.id = TypeId{0x42554e444c45ull, 1};
        schema.name = Span<const char>(CopyName, sizeof(CopyName) - 1);
        schema.toolName = schema.name;
        schema.fields = Span<const FieldSchema>(fields, generated.fields.Size());
        RuntimeTypeOps ops = GeneratedType<BundleAsset>::Ops();
        auto registered = env.types.Register(schema, Move(ops));
        if (registered) {
            (void)env.types.Unregister(registered.Value());
            return ErrorCode::None;
        }

        return registered.Error();
    };

    CHECK(registers(0, "UI,Game") == ErrorCode::None);
    CHECK(registers(static_cast<uint32>(FieldFlags::EditorOnly), "UI") ==
          ErrorCode::InvalidArgument);
    CHECK(registers(0, "UI,,Game") == ErrorCode::InvalidArgument);
    CHECK(registers(0, "UI,UI") == ErrorCode::InvalidArgument);
    CHECK(registers(0, " UI") == ErrorCode::InvalidArgument);
    CHECK(registers(0, "Audio", weights, true) == ErrorCode::InvalidArgument);
}

} // namespace asset_bundle_schema_tests_file
