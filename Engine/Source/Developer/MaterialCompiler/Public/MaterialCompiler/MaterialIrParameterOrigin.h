#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <MaterialSchema/ParameterKey.h>

// 번역의 파라미터 출처 하나다(Docs/Design/MaterialSystem 12.8절 MS3d1, 9.2절 ParameterKey). 수
// 파라미터와 파라미터 텍스처의 IR 정체 Guid(함수 안이면 호출 경로를 섞은 hash)마다 하나이고, 쿠킹
// 처리기가 ParameterKey·이름·기본값(노드 hook)을 다시 찾는 데 쓴다.
//
// 사용 예:
//   if (const IrParameterOrigin* origin = module.FindParameterOrigin(parameter.id)) {
//       key.parameterId = origin->parameterId;
//   }
namespace rocks::material {

struct IrParameterOrigin {
    explicit IrParameterOrigin(Allocator& allocator) noexcept
        : callPath(allocator), name(allocator) {
    }

    IrParameterOrigin(IrParameterOrigin&&) noexcept = default;
    IrParameterOrigin& operator=(IrParameterOrigin&&) noexcept = default;
    IrParameterOrigin(const IrParameterOrigin&) = delete;
    IrParameterOrigin& operator=(const IrParameterOrigin&) = delete;

    // IR 정체(IrParameter::id·파라미터 IrTexture::id)다.
    Guid identity;
    // 파라미터 노드를 담은 그래프의 에셋이다(함수 인스턴스로 부른 함수면 인스턴스, MaterialSample로
    // 펼친 머티리얼이면 그 머티리얼, 루트 그래프는 null — 처리기가 루트 Material id로 채운다).
    Guid owner;
    // 파라미터 노드의 ParameterId(정체 hash 전 원래 값)다.
    Guid parameterId;
    ParameterScopeKind scope = ParameterScopeKind::Global;
    // Layer·LayerBlend의 레이어 Guid다(그 밖은 null).
    Guid layer;
    // 루트부터의 원본 호출 노드 id(FunctionCall·MaterialSample·레이어 노드 — 바깥 → 안)다. 공유
    // 파라미터는 빈다.
    Array<Guid> callPath;
    // 파라미터 이름(노드 parameterName)이다.
    String name;
};

// source를 out에 복사한다. 오류: 할당 실패.
inline Result<void> CopyIrParameterOrigin(const IrParameterOrigin& source,
                                          IrParameterOrigin& out) noexcept {
    out.identity = source.identity;
    out.owner = source.owner;
    out.parameterId = source.parameterId;
    out.scope = source.scope;
    out.layer = source.layer;
    out.callPath.Clear();
    if (auto copied = out.callPath.TryAppend(source.callPath.View()); !copied) {
        return copied;
    }

    return out.name.TryAssign(source.name.View());
}

} // namespace rocks::material
