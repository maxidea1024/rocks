#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <TestSupport/Test.h>

// 머티리얼 에셋 클래스 골격 시험이다(Docs/Design/MaterialSystem.ko.md 4.2절, 단계 MS3a): 등록과
// 상속 관계(Material은 MaterialBase·Asset), MaterialBase는 추상이라 만들 수 없음, 에디터 패키지
// 저장·로드·재저장 왕복과 결정성.
using namespace rocks;
namespace material_tests_file {

namespace {

TypeId MaterialType() noexcept {
    return GeneratedType<Material>::Id();
}

TypeId MaterialBaseType() noexcept {
    return GeneratedType<MaterialBase>::Id();
}

bool IsA(TypeRegistry& types, TypeId actual, TypeId expected) noexcept {
    auto result = types.IsA(actual, expected);
    return result && result.Value();
}

} // namespace

// 등록: Material은 MaterialBase와 Asset이고 그 반대는 아니다. 추상 MaterialBase는 만들 수 없고
// Material은 패키지의 대표 에셋으로 만들 수 있다.
TEST(Material_RegistersClasses) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    CHECK(IsA(env.types, MaterialType(), MaterialBaseType()));
    CHECK(IsA(env.types, MaterialType(), GeneratedType<Asset>::Id()));
    CHECK(IsA(env.types, MaterialBaseType(), GeneratedType<Asset>::Id()));
    CHECK(!IsA(env.types, MaterialBaseType(), MaterialType()));

    ObjectRegistry& source = *env.source;
    auto package =
        NewPackage(source, source.OwnerAccess(), asset_test::FixedId(0x600), PackageKind::Editor);
    REQUIRE(package);
    CHECK(asset_test::Make<MaterialBase>(source, MaterialBaseType(), package.Value().Handle(), "")
              .IsNull());
    CHECK(
        !asset_test::Make<Material>(source, MaterialType(), package.Value().Handle(), "").IsNull());
}

// 저장 → 로드 → 재저장: Material 대표 에셋 패키지를 다른 Registry에 로드하면 Material이고, 읽은
// 설정으로 다시 쓴 byte가 처음과 같다. 두 번 쓴 byte도 같다(결정성).
TEST(Material_PackageRoundTripResaveIdentical) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    auto package =
        NewPackage(source, source.OwnerAccess(), asset_test::FixedId(0x601), PackageKind::Editor);
    REQUIRE(package);
    REQUIRE(
        !asset_test::Make<Material>(source, MaterialType(), package.Value().Handle(), "").IsNull());
    Array<uint8> first = asset_test::Write(env.allocator, source, package.Value().Handle(),
                                           asset_test::EditorOptions());
    REQUIRE(!first.IsEmpty());
    const Array<uint8> again = asset_test::Write(env.allocator, source, package.Value().Handle(),
                                                 asset_test::EditorOptions());
    CHECK(asset_test::SameBytes(first.View(), again.View()));

    Array<uint8> files[1] = {asset_test::Copy(env.allocator, first.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(
        asset_test::LoadAll(env, *env.target, files, PackageKind::Editor, nullptr, roots, handles));
    auto loaded = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    REQUIRE(loaded);
    CHECK(asset_test::Pointer<Material>(*env.target, loaded.Value()) != nullptr);

    auto contents =
        ReadPackage(env.allocator, first.View(), asset_test::LoadOptions(PackageKind::Editor).read);
    REQUIRE(contents);
    const Array<uint8> resaved =
        asset_test::ResaveAsRead(env.allocator, *env.target, handles[0], contents.Value());
    CHECK(asset_test::SameBytes(first.View(), resaved.View()));
}

} // namespace material_tests_file
