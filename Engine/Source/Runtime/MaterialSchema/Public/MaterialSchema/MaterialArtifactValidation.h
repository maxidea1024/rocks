#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <MaterialSchema/MaterialArtifact.h>

// 쿠킹 artifact의 구조 검증이다(Docs/Design/MaterialSystem 12.8절 MS3d2 — 리플렉션 해석은 필드
// 사이 번호를 대조하지 않으므로 로드가 쓰기 전에 한 번 부른다). 손상이면 CorruptData다(조용히
// 대체하지 않는다 — 머티리얼 로드가 실패로 보고한다).
//
// 사용 예:
//   const MaterialArtifactTables tables{material.parameters.Size(),
//                                       material.referencedTextures.Size(),
//                                       material.parameterCollections.Size()};
//   if (auto valid = ValidateMaterialArtifact(artifact, tables); !valid) { return valid; }
namespace rocks::material {

// artifact가 가리키는 Material 표의 크기다.
struct MaterialArtifactTables {
    usize parameters = 0;
    usize textures = 0;
    usize collections = 0;
};

// artifact를 검증한다: 명령의 피연산자 수·번호(앞 명령)·연산·형, Parameter·Texture 즉시값 번호,
// TextureSize의 피연산자(Texture 명령), 칸의 명령 번호·float4 번호·성분 범위·형(Double 명령은
// DoubleTile·DoubleOffset), 파라미터·텍스처·컬렉션 번호(표 크기 안 — 텍스처 slot의 parameter는
// 파라미터 slot일 때만). 오류: 위반 CorruptData.
ROCKS_MATERIALSCHEMA_API Result<void>
ValidateMaterialArtifact(const MaterialArtifact& artifact,
                         const MaterialArtifactTables& tables) noexcept;

} // namespace rocks::material
