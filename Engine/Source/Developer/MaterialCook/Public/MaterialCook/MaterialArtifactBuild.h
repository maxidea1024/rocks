#pragma once

#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Core/Text/NamePool.h>
#include <MaterialCook/MaterialArtifactLinks.h>
#include <MaterialCook/MaterialTranslationNugget.h>
#include <MaterialCook/MaterialTranslationValue.h>
#include <MaterialSchema/MaterialArtifact.h>

// 번역 값 → 런타임 artifact 변환이다(Docs/Design/MaterialSystem 12.8절 MS3d2). 상수 식 명령은 IR
// 연산을 MaterialConstantOp로 옮기고(Parameter 즉시값 [3]은 Material::parameters 번호로 바꾼다),
// 텍스처 slot의 논리 크기 사용은 프로그램의 TextureSize에서 유도한다.
//
// 사용 예:
//   MaterialArtifact artifact(allocator);
//   MakeMaterialArtifact(value, names, links, artifact);
namespace rocks {

// value를 out으로 옮긴다(out을 비우고 채운다 — 셰이더 집합 id·layout 서명·traits 정규 바이트
// 포함). links의 각 목록 길이는 값의 표 길이와 같아야 한다. 오류: links 길이 불일치·프로그램에 상수
// 식 밖 연산이나 형(상수 빌더 계약 위반)은 InvalidArgument, Name intern 실패, 할당 실패.
ROCKS_MATERIALCOOK_API Result<void> MakeMaterialArtifact(const MaterialTranslationValue& value,
                                                         NamePool& names,
                                                         const MaterialArtifactLinks& links,
                                                         material::MaterialArtifact& out) noexcept;

} // namespace rocks
