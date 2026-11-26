#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderTypeRegistry.h>

// 셰이더 라이브러리 쿠킹 단계가 쿠킹할 셰이더 타입 등록부의 원천이다(Docs/Design/ShaderSystem
// 16.1절 SSD45 3번). 제품은 ModuleShaderCookRegistrySource(엔진 모듈 집합을 시작해 채운다 —
// Docs/Design/BuildTargets 3.10절 4항), 시험은 자기 등록부를 주는 원천을 쓴다.
//
// 사용 예(시험 원천):
//   class FixedRegistry final : public shader::ShaderCookRegistrySource {
//   public:
//       Result<const shader::ShaderTypeRegistry*> Acquire() noexcept override {
//           return Result<const shader::ShaderTypeRegistry*>::Success(&registry);
//       }
//       void Release() noexcept override {}
//       StringView Problem() const noexcept override { return {}; }
//       shader::ShaderTypeRegistry registry{allocator};   // Freeze됨
//   };
namespace rocks::shader {

class ShaderCookRegistrySource {
public:
    virtual ~ShaderCookRegistrySource() = default;

    // Freeze된 등록부를 준비해 돌려준다. 성공하면 Release까지 그 등록부가 유효하고 바뀌지 않는다.
    // 쿠킹 단계 Run 하나에서 Acquire·Release를 한 번씩 부른다(같은 스레드). 실패하면 그 오류이고
    // Problem이 영어 설명을 준다(Release를 부르지 않는다).
    virtual Result<const ShaderTypeRegistry*> Acquire() noexcept = 0;

    // Acquire로 준비한 것을 돌려준다.
    virtual void Release() noexcept = 0;

    // 마지막 Acquire 실패의 영어 설명이다(없으면 빈 값). 다음 Acquire까지 유효하다.
    virtual StringView Problem() const noexcept = 0;
};

} // namespace rocks::shader
