#pragma once

#include <Core/Result.h>
#include <InputActions/InputContextBundle.h>
#include <InputActions/InputDataDiagnostic.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectHandle.h>

namespace rocks {
class ObjectRegistry;
} // namespace rocks

// 컨텍스트 객체 → 평가기 사본 모으기와 데이터 검증이다(Docs/Design/InputActions 2.7절).
namespace rocks::input {

// context(InputContext 객체)와 그 actions를 Borrow해 bundle을 만든다. registry·access는 그 객체를
// 읽을 수 있는 문맥이다(에셋이면 AssetManager::With 범위). 같은 액션 객체가 actions에 여러 번
// 있으면 한 번만 복사한다. 검증은 하지 않는다(평가기 AddContext·UpdateContext가 한다).
//
// 사용 예:
//   InputContextBundle bundle(allocator);
//   if (auto collected = CollectInputContext(registry, access, handle, bundle); !collected) {
//       return collected;
//   }
//
// 오류: 객체 접근 실패(파괴 포함)는 그 오류, null actions 원소는 InvalidArgument, 할당 실패는
// OutOfMemory다. 실패하면 bundle은 그대로다. 사본의 allocator는 bundle의 것이다.
ROCKS_INPUTACTIONS_API Result<void> CollectInputContext(ObjectRegistry& registry,
                                                        const ObjectAccessContext& access,
                                                        ObjectHandle context,
                                                        InputContextBundle& bundle) noexcept;

// bundle이 2.7절 규칙을 지키는지 본다. 거부하면 InvalidArgument이고 diagnostic(주면)에 첫 위치를
// 쓴다(성공이면 issue None). chord 대상이 액션 목록에 없는 것은 허용한다(다른 컨텍스트의 액션).
// 키 이름은 보지 않는다(빈 키·모르는 이름은 평가기가 다시 만들 때 그 바인딩만 건너뛴다 — UE와
// 같다).
ROCKS_INPUTACTIONS_API Result<void>
ValidateInputContextBundle(const InputContextBundle& bundle,
                           InputDataDiagnostic* diagnostic = nullptr) noexcept;

// 트리거·모디파이어 목록을 같은 규칙으로 본다(주입 — 4.3절 3). 위치 칸은 element·elementKind만
// 쓴다.
ROCKS_INPUTACTIONS_API Result<void>
ValidateInputElements(Span<const InputTrigger> triggers, Span<const InputModifier> modifiers,
                      InputDataDiagnostic* diagnostic = nullptr) noexcept;

// 액션 설정 하나를 같은 규칙으로 본다(주입 액션 — 4.3절 3). 위치 칸은 action을 빼고 같다.
ROCKS_INPUTACTIONS_API Result<void>
ValidateInputActionSettings(const InputActionSettings& settings,
                            InputDataDiagnostic* diagnostic = nullptr) noexcept;

} // namespace rocks::input
