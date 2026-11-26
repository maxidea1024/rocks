#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialIrModule.h>

// 머티리얼 상수 블록 layout과 상수 식 프로그램이다(Docs/Design/MaterialSystem 6.10절 MS2c2, 9.4절).
// IR 모듈에서 유도한다: 빈도 Uniform인 프로그램 연산 값 가운데 프로그램 밖에서 쓰이는 값이
// 출력이고, 출력은 상수 블록 float 칸에 놓인다. 프로그램은 출력까지의 값을 모듈 번호 순으로
// 계산하는 명령열이다 (명령의 피연산자는 앞 명령 번호). 생성 원문과 쿠킹(MS3)이 같은 결과를 쓴다.
//
// 사용 예:
//   MaterialConstants constants(allocator);
//   if (auto built = BuildMaterialConstants(module, constants); !built) { ... }
//   for (const MaterialConstantMember& member : constants.members) { ... }
namespace rocks::material {

// 상수 블록 칸의 내용이다(Double 출력은 tile·offset 두 칸 — 5.3절 LWC 표현).
enum class MaterialConstantSlotKind : uint8 {
    Value = 0,
    DoubleTile = 1,
    DoubleOffset = 2
};

// 상수 블록 멤버 하나(출력 하나의 칸)다.
struct MaterialConstantMember {
    // 모듈 값 번호다.
    IrValueId value = NoIrValue;
    // 그 값을 계산하는 프로그램 명령 번호다.
    uint32 step = 0;
    MaterialConstantSlotKind kind = MaterialConstantSlotKind::Value;
    // float4 칸 번호와 그 안의 첫 성분·성분 수다(float4 경계를 넘지 않는다 — 7.4절 5항).
    uint32 vector = 0;
    uint8 firstComponent = 0;
    uint8 components = 0;
};

struct MaterialConstants {
    explicit MaterialConstants(Allocator& allocator) noexcept
        : steps(allocator), members(allocator) {
    }

    // 프로그램 명령이다. IrValue 형식 그대로이고 operands는 앞 명령 번호, Parameter는 모듈 파라미터
    // 표 번호(즉시값 [3])를, Texture는 모듈 텍스처 표 번호(즉시값 [3])를 읽고 TextureSize는 그
    // slot의 논리 크기다. 형식 버전은 MaterialConstantProgramVersion이다.
    Array<IrValue> steps;
    // 멤버는 출력 값 번호 순, Double은 tile 다음 offset이다.
    Array<MaterialConstantMember> members;
    // 상수 블록의 float4 수다(0이면 상수 블록이 없다).
    uint32 floatVectors = 0;
};

// 프로그램 명령 형식 버전이다(바꾸면 올린다 — 번역 너겟 키).
inline constexpr uint32 MaterialConstantProgramVersion = 1;

// 상수 식 프로그램 연산인가(UE preshader 접기 목록 — 6.10절).
ROCKS_MATERIALCOMPILER_API bool IsMaterialConstantProgramOp(IrOp op) noexcept;

// module에서 out을 만든다(이전 내용은 지운다). 오류: 할당 실패, 모듈 손상(InvalidArgument).
ROCKS_MATERIALCOMPILER_API Result<void> BuildMaterialConstants(const IrModule& module,
                                                               MaterialConstants& out) noexcept;

} // namespace rocks::material
