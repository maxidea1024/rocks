#include "AssetTestSupport.h"

#include <AssetRuntime/PackageReader.h>
#include <AssetTestFixtures/BundleAssets.generated.h>
#include <AssetTestFixtures/BundleAssets.h>

// 쿠킹 패키지 쓰기의 EditorOnly 클래스 객체 제외 시험이다(Docs/Design/AssetSystem 10.2절 "A4e 구현
// 세부" — A4e2b, UE SavePackage가 쿠킹 때 편집기 전용 객체를 빼는 것과 같다): 에디터 쓰기는
// EditorOnly 하위 객체와 그 자식을 export로 담고, 쿠킹 쓰기는 그 부분 트리를 빼며 그것을 가리키는
// Hard 참조를 null로 쓴다(쓰기 실패가 아니다). 쿠킹 패키지는 로드된다.
using namespace rocks;
namespace package_editor_only_tests_file {

using namespace rocks::asset_test;
using asset_fixture::EditorNote;

namespace {

// 대표 TestAsset(hard → Note), EditorOnly 하위 객체 Note, 그 자식 TestPart Inner를 담은 패키지다.
struct EditorOnlyScene {
    explicit EditorOnlyScene(AssetEnv& env) noexcept {
        ObjectRegistry& registry = *env.source;
        auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(930), PackageKind::Editor);
        if (!made) {
            return;
        }

        package = Move(made.Value());
        asset = Make<TestAsset>(registry, TestAssetType(), package.Handle(), "");
        note = Make<EditorNote>(registry, GeneratedType<EditorNote>::Id(), asset, "Note");
        const ObjectHandle inner = Make<TestPart>(registry, TestPartType(), note, "Inner");
        TestAsset* value = Pointer<TestAsset>(registry, asset);
        ready = !asset.IsNull() && !note.IsNull() && !inner.IsNull() && value != nullptr &&
                static_cast<bool>(
                    registry.SetReference(registry.OwnerAccess(), asset, value->hard, note));
    }

    RootRef<Package> package;
    ObjectHandle asset{};
    ObjectHandle note{};
    bool ready = false;
};

} // namespace

TEST(AssetPackage_CookedWriteDropsEditorOnlyObjects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    EditorOnlyScene scene(env);
    REQUIRE(scene.ready);
    ObjectRegistry& registry = *env.source;

    // 에디터 쓰기: 대표·Note·Inner 셋을 담는다.
    Array<uint8> editor = Write(env.allocator, registry, scene.package.Handle(), EditorOptions());
    REQUIRE(!editor.IsEmpty());
    PackageReadOptions editorRead;
    editorRead.engineVersion = TestEngineVersion;
    auto editorContents = ReadPackage(env.allocator, editor.View(), editorRead, nullptr);
    REQUIRE(editorContents);
    CHECK(editorContents.Value().exports.Size() == 3);

    // 쿠킹 쓰기: Note 부분 트리(Note·Inner)를 빼고 대표만 담는다.
    Array<uint8> cooked = Write(env.allocator, registry, scene.package.Handle(), CookedOptions());
    REQUIRE(!cooked.IsEmpty());
    auto cookedContents = ReadPackage(env.allocator, cooked.View(), PackageReadOptions{}, nullptr);
    REQUIRE(cookedContents);
    CHECK(cookedContents.Value().exports.Size() == 1);

    // 쿠킹 패키지를 로드하면 대표의 hard는 null이다.
    Array<uint8> files[1] = {Move(cooked)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    auto loaded = env.target->FindChild(env.target->OwnerAccess(), handles[0], "");
    REQUIRE(loaded);
    TestAsset* value = Pointer<TestAsset>(*env.target, loaded.Value());
    REQUIRE(value != nullptr);
    CHECK(value->hard.Handle().IsNull());
}

} // namespace package_editor_only_tests_file
