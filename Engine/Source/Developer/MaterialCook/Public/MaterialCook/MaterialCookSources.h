#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialCollectionLayout.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialCompiler/MaterialTextureFacts.h>
#include <MaterialCook/MaterialCookGatherContext.h>
#include <MaterialCook/MaterialCookReference.h>
#include <NodeGraph/GraphDocument.h>

// 번역 입력 모으기다(Docs/Design/MaterialSystem 12.8절 MS3c "번역 입력 모으기"). 루트 그래프의 참조
// 선언(19.4.3절)에서 시작해 함수·다른 머티리얼·레이어 그래프의 참조를 전이로 따라가며 패키지를
// 열고, 번역이 읽을 값을 복사해 소유한다(에셋 객체 Borrow는 모으는 동안만 쓴다): 그래프
// snapshot(함수 인스턴스는 기반 함수 그래프 — override는 MS6), 색 커브 네 채널과 조정, 커브 atlas
// 행 목록, 텍스처 사실, 파라미터 컬렉션 layout(base 사슬 — UE GetParameterIndex 배치). 그 자체가
// 번역 입력의 원본 공급(MaterialSourceProvider)이다.
//
// 사용 예:
//   MaterialCookSources sources(allocator);
//   if (auto gathered = sources.Gather(gather, material.graph); !gathered) { ... }
//   input.sources = &sources;
//   input.textures = sources.Textures();
//   input.collections = sources.Collections();
//
// 수명: 번역이 끝날 때까지 산다. 스레드: Registry 소유 스레드 전용이다.
namespace rocks {

class ROCKS_MATERIALCOOK_API MaterialCookSources final : public material::MaterialSourceProvider {
public:
    explicit MaterialCookSources(Allocator& allocator) noexcept;
    ~MaterialCookSources() override;

    MaterialCookSources(const MaterialCookSources&) = delete;
    MaterialCookSources& operator=(const MaterialCookSources&) = delete;

    // root 그래프에서 시작해 참조 닫힘을 모은다(앞서 모은 것을 비운다). 열지 못한 참조·역할
    // 클래스가 아닌 대상은 References에 object null로 남고 공급에서 빠진다(번역이 그 노드에서
    // 진단한다). 오류: context의 registry가 null이면 InvalidArgument, Registry 조회 오류(열기 실패
    // 밖), 할당 실패.
    Result<void> Gather(const MaterialCookGatherContext& context,
                        const nodegraph::GraphDocument& root) noexcept;

    const material::MaterialGraphSnapshot* FindFunction(Guid asset) const noexcept override;
    const material::MaterialGraphSnapshot* FindMaterial(Guid asset) const noexcept override;
    const material::MaterialColorCurveSnapshot* FindColorCurve(Guid asset) const noexcept override;
    const material::MaterialCurveAtlasSnapshot* FindCurveAtlas(Guid asset) const noexcept override;

    // 텍스처 사실이다(텍스처·atlas 참조마다 하나, AssetId 순).
    Span<const material::MaterialTextureFacts> Textures() const noexcept {
        return textures_.View();
    }

    // 파라미터 컬렉션 layout이다(AssetId 순).
    Span<const material::MaterialCollectionLayout> Collections() const noexcept {
        return collections_.View();
    }

    // 모은 참조다((역할, AssetId) 순 — 중복 없음).
    Span<const MaterialCookReference> References() const noexcept {
        return references_.View();
    }

    // 모은 그래프 snapshot 수와 index번째다(함수·머티리얼 — 모은 순서).
    usize GraphCount() const noexcept;
    const material::MaterialGraphSnapshot& Graph(usize index) const noexcept;

    // asset 텍스처의 사실이다(없으면 nullptr).
    const material::MaterialTextureFacts* FindTexture(Guid asset) const noexcept;

    // asset 컬렉션의 layout이다(없으면 nullptr).
    const material::MaterialCollectionLayout* FindCollection(Guid asset) const noexcept;

private:
    struct OwnedGraph;
    struct OwnedCurve;
    struct OwnedAtlas;
    struct OwnedCollection;

    Result<void> Visit(const MaterialCookGatherContext& context, MaterialCookReference& reference,
                       Array<MaterialCookReference>& pending) noexcept;
    Result<void> AddGraph(const MaterialCookGatherContext& context, Guid asset, ObjectHandle object,
                          bool material, Array<MaterialCookReference>& pending) noexcept;
    Result<void> AddCurve(const MaterialCookGatherContext& context, Guid asset,
                          ObjectHandle object) noexcept;
    Result<void> AddAtlas(const MaterialCookGatherContext& context, Guid asset,
                          ObjectHandle object) noexcept;
    Result<void> AddTexture(const MaterialCookGatherContext& context, Guid asset,
                            ObjectHandle object) noexcept;
    Result<void> AddCollection(const MaterialCookGatherContext& context, Guid asset,
                               ObjectHandle object) noexcept;
    void Clear() noexcept;

    Allocator* allocator_;
    Array<UniquePtr<OwnedGraph>> graphs_;
    Array<UniquePtr<OwnedCurve>> curves_;
    Array<UniquePtr<OwnedAtlas>> atlases_;
    Array<UniquePtr<OwnedCollection>> ownedCollections_;
    Array<material::MaterialTextureFacts> textures_;
    Array<material::MaterialCollectionLayout> collections_;
    Array<MaterialCookReference> references_;
};

} // namespace rocks
