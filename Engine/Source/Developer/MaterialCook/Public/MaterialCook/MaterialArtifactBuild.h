#pragma once

#include <Core/Result.h>
#include <Core/Text/NamePool.h>
#include <MaterialCook/MaterialArtifactLinks.h>
#include <MaterialCook/MaterialTranslationNugget.h>
#include <MaterialCook/MaterialTranslationValue.h>
#include <MaterialSchema/MaterialArtifact.h>

// 번역 값 → 런타임 artifact 변환이다(Docs/Design/MaterialSystem 12.8절 MS3d2). 상수 식 명령은 IR
// 연산을 MaterialConstantOp로 옮기고(Parameter·Texture 즉시값 [3]은 artifact 표 번호 그대로 —
// artifact.parameters가 Material::parameters로 잇는다), 텍스처 slot의 논리 크기 사용은 프로그램의
// TextureSize에서 유도한다.
//
// 사용 예:
//   MaterialArtifact artifact(allocator);
//   MakeMaterialArtifact(value, names, links, artifact);
namespace rocks {

// value를 out으로 옮긴다(out을 비우고 채운다 — 셰이더 집합 id·layout 서명·traits 정규 바이트
// 포함). links의 각 목록 길이는 값의 표 길이와 같고 번호는 모두 있어야 하며(NoMaterialReference
// 아님), textureParameters는 파라미터 텍스처 slot에만 번호가 있다. 오류: links 위반·프로그램에 상수
// 식 밖 연산이나 형·표 번호 범위 밖(상수 빌더 계약 위반 — 해석한 값은 의미 검사가 이미 거른다)은
// InvalidArgument, Name intern 실패, 할당 실패. 실패하면 out 내용은 정해지지 않는다.
ROCKS_MATERIALCOOK_API Result<void> MakeMaterialArtifact(const MaterialTranslationValue& value,
                                                         NamePool& names,
                                                         const MaterialArtifactLinks& links,
                                                         material::MaterialArtifact& out) noexcept;

} // namespace rocks
