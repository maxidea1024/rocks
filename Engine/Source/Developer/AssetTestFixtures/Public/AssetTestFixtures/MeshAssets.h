#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Meta/Annotations.h>
#include <Mesh/SimpleCollision.h>
#include <MeshSource/MeshSourceData.h>

// 원본 메시 시험 에셋이다(Docs/Design/MeshAnimationAssets.ko.md 12절). MeshSource 시험이
// MeshSource::Store로 채운 MeshSourceData를 패키지로 저장·로드해 Load가 실제 패키지 bulk를 읽게
// 한다. 정적 메시 LOD 원본처럼 EditorOnly 필드와 일반 필드, 원본 배열(LOD 여럿 — 한 bulk 원본을
// 공유)을 함께 둔다.
namespace rocks::asset_fixture {

ROCKS_CLASS()
struct TestMeshAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit TestMeshAsset(Allocator& allocator) noexcept
        : mesh(allocator), lods(allocator), editorMesh(allocator) {
    }

    ROCKS_PROPERTY() MeshSourceData mesh;
    ROCKS_PROPERTY() Array<MeshSourceData> lods;
    ROCKS_PROPERTY(EditorOnly) MeshSourceData editorMesh;
};

// 단순 충돌 시험 에셋이다(Docs/Design/MeshAnimationAssets.ko.md 4.8절·12.6절). 정적 메시의
// simpleCollision·complexity처럼 일반 필드와, LOD 배열처럼 SimpleCollision 배열·EditorOnly 사본을
// 두어 볼록 형상 bulk를 실제 패키지로 왕복한다.
ROCKS_CLASS()
struct TestCollisionAsset final : Asset {
    ROCKS_GENERATED_BODY()

    explicit TestCollisionAsset(Allocator& allocator) noexcept
        : collision(allocator), variants(allocator), editorCollision(allocator) {
    }

    ROCKS_PROPERTY() SimpleCollision collision;
    ROCKS_PROPERTY() CollisionComplexity complexity = CollisionComplexity::Default;
    ROCKS_PROPERTY() Array<SimpleCollision> variants;
    ROCKS_PROPERTY(EditorOnly) SimpleCollision editorCollision;
};

} // namespace rocks::asset_fixture
