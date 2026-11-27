#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>

// Unreal 교환 A2f 임포터의 옵션이다(Docs/Design/UnrealExchange 10.6절 3번, 5.5절 11번). 종류별
// 임포터가 서비스 대입 텍스트(`objectPath=…`·`formerPaths=…`·`references=…`, 메시는
// `meshNormals=…`)로 넘기고 ImportRecord options에 남아
// 일반 asset-reimport가 그대로 쓴다. 필드 이름은 출력 타입의 노출 속성과 겹치지 않는다(A2f 등록
// 규칙).
//
// 사용 예(A2f 임포터):
//   const auto* options = static_cast<const UnrealExchangeImportOptions*>(context.Options());
//   if (options->objectPath.View() != asset.objectPath) { /* InvalidImportOption */ }
namespace rocks {

// --mesh-normals 값이다(5.3절 — 정적·스킨 메시가 UE 준비 원본을 쓰는지, 5.5절 2번). Unreal은 UE가
// 계산한 법선·tangent·lightmap UV를 담은 prepared 원본을, Rocks는 순수 원본과 UE 빌드 설정을 쓴다.
ROCKS_ENUM()
enum class ExchangeMeshNormals : uint8 {
    Unreal,
    Rocks
};

ROCKS_STRUCT()
struct UnrealExchangeImportOptions {
    ROCKS_GENERATED_BODY()

    explicit UnrealExchangeImportOptions(Allocator& allocator) noexcept
        : objectPath(allocator), formerPaths(allocator), references(allocator) {
    }

    // 이 에셋의 UE objectPath다(문서 objectPath와 같아야 한다).
    ROCKS_PROPERTY() String objectPath;
    // 묶음 redirects 가운데 to가 이 objectPath인 from이다(byte 순 — 태그 FormerPaths).
    ROCKS_PROPERTY() Array<String> formerPaths;
    // 참조로 걸 대상이다: 의존 objectPath(redirect를 풀기 전 원문) → AssetId. 못 찾은 대상은 없다.
    ROCKS_PROPERTY() OrderedMap<String, AssetId> references;
    // 메시 LOD 원본 선택이다(5.5절 2번 — 정적 메시만 쓴다, 텍스처는 기본값 그대로 기록된다).
    ROCKS_PROPERTY() ExchangeMeshNormals meshNormals = ExchangeMeshNormals::Unreal;
};

} // namespace rocks
