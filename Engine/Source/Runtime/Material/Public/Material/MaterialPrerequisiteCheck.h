#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialPrerequisite.h>

// 에셋 TODO 필드 검사다(Docs/Design/MaterialSystem 4.6.1·19.4.1·19.9·19.11절, 결정 MD12). 선행이
// 없는 필드에 기본값이 아닌 값이 들어 있으면 그 선행 ID와 필드 이름을 모은다. 쿠킹(MS3)은 결과가
// 비어 있지 않으면 PrerequisiteMissing 진단으로 실패하고, 에디터는 경고로 보인다. 대체 값으로
// 바꾸지 않는다.
//
// 사용 예:
//   Array<MaterialPrerequisiteUse> uses(allocator);
//   if (auto checked = CollectUsedPrerequisites(material, uses); checked && !uses.IsEmpty()) {
//       Report(MaterialPrerequisiteId(uses[0].prerequisite), uses[0].field);
//   }
namespace rocks {

class Material;
class MaterialBase;
class MaterialFunctionInstance;
class MaterialInstance;

// 선행이 빠진 TODO 필드의 사용 하나다. field는 정적 영어 필드 경로다(예: "traits.displacement").
struct MaterialPrerequisiteUse {
    material::MaterialPrerequisite prerequisite = material::MaterialPrerequisite::None;
    StringView field;
};

// MaterialBase 필드(profile 목록·neural profile·조명 굽기 설정)를 본다. out 끝에 더한다. 오류: 할당
// 실패.
ROCKS_MATERIAL_API Result<void>
CollectUsedPrerequisites(const MaterialBase& base, Array<MaterialPrerequisiteUse>& out) noexcept;

// Material 필드(MaterialBase 포함)를 본다: 물리 재질(값·마스크·RenderTrace·표), 가상화 geometry
// 대체·displacement, 가상 텍스처 제한 완화, 블렌드 모드
// AlphaHoldout·TranslucentColoredTransmittance.
ROCKS_MATERIAL_API Result<void>
CollectUsedPrerequisites(const Material& material, Array<MaterialPrerequisiteUse>& out) noexcept;

// MaterialInstance 필드(MaterialBase 포함)를 본다: TODO 종류 파라미터 override, 물리 재질·profile·
// 가상화 geometry override, traitOverrides의 displacement·블렌드 모드, 조명 굽기 override.
ROCKS_MATERIAL_API Result<void>
CollectUsedPrerequisites(const MaterialInstance& instance,
                         Array<MaterialPrerequisiteUse>& out) noexcept;

// MaterialFunctionInstance의 TODO 종류 파라미터 override(텍스처 컬렉션·RVT·SVT·글꼴)를 본다.
ROCKS_MATERIAL_API Result<void>
CollectUsedPrerequisites(const MaterialFunctionInstance& instance,
                         Array<MaterialPrerequisiteUse>& out) noexcept;

} // namespace rocks
