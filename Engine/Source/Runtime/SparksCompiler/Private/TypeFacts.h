#pragma once

#include <SparksCompiler/Compilation.h>

// 검사가 끝난 타입 표만 읽는 타입 판정이다(Docs/Design/SparksLanguage 3.3절 "비교", 3.8절 원소
// 조건). 타입 검사(TypeChecker)와 공개 질의(Compilation::MemberAvailable — 자동완성, Docs/Design/
// SparksTools.ko.md 11.6절)가 같은 판정을 쓰도록 둘이 함께 부른다. 타입을 새로 만들지 않으므로
// 결과 표만 있으면 된다.

namespace rocks::sparks::detail {

// 숫자 타입의 분류다.
enum class NumericKind : uint8 {
    None,
    Signed,
    Unsigned,
    Float
};

struct NumericInfo {
    NumericKind kind = NumericKind::None;
    uint32 bits = 0;
};

class TypeFacts final {
public:
    explicit TypeFacts(const Compilation& compilation) noexcept : compilation_(compilation) {
    }

    // 타입을 정한 심볼이다(NoType·범위 밖은 NoSymbol).
    uint32 SymbolOf(uint32 type) const noexcept;

    // index번째 타입 인자다(없으면 NoType).
    uint32 Argument(uint32 type, uint32 index) const noexcept;

    bool IsOptional(uint32 type) const noexcept;

    // 숫자 타입의 분류다(`?` 타입·숫자가 아닌 타입은 None).
    NumericInfo Numeric(uint32 type) const noexcept;

    // 이름이 name인 내장·prelude 타입인지다(`?` 여부는 보지 않는다).
    bool IsLanguage(uint32 type, StringView name) const noexcept;

    // 참조 타입(Sparks class·interface, 엔진 object·asset·interface)인지다.
    bool IsReference(uint32 type) const noexcept;

    // 엔진 asset 타입(또는 그것을 기반으로 둔 class)인지다.
    bool IsAssetType(uint32 type) const noexcept;

    // enum 타입(Sparks·엔진 enum, `Clock`)인지다(`?` 타입은 아니다).
    bool IsEnumType(uint32 type) const noexcept;

    // 객체 참조 타입(asset이 아닌 참조)인지다.
    bool IsObjectType(uint32 type) const noexcept;

    // `==`를 쓸 수 있는 타입인지다(3.3절).
    bool IsEquatable(uint32 type) const noexcept;

    // `<`를 쓸 수 있는 타입인지다(3.3절).
    bool IsOrdered(uint32 type) const noexcept;

    // 언어 멤버(부록 B)가 receiver 타입에서 있는지다(원소 조건 [==]·[<]·[값]·[객체]·void 아님).
    // 언어 멤버가 아니면 참이다.
    bool MemberAvailable(uint32 member, uint32 receiver) const noexcept;

private:
    const Compilation& compilation_;
};

} // namespace rocks::sparks::detail
