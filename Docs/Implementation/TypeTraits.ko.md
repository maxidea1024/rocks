# 범용 타입 형질

공용 진입점은 `Base/TypeTraits.h`다. Base·PlatformBase·Core·ObjectRuntime과 C#이 생성하는 C++이 같은 API를 쓴다.
기존 `Core/Traits.h`는 include 호환성을 유지하는 전달 헤더다. traits 구현은 Types.h·Compiler.h·STL·OS 헤더에 의존하지 않으며 실행 시 저장소나 할당을 요구하지 않는다.

## 사용 규칙

```cpp
#include <Base/TypeTraits.h>

template <typename Base, typename Derived>
    requires rocks::IsBaseOfV<Base, Derived>
void Register();

template <typename T>
using Stored = rocks::RemoveCvRefT<T>;

enum class MyEnum : unsigned int { Value };
static_assert(rocks::IsNothrowConstructibleV<int, int>);
static_assert(rocks::IsSameV<rocks::UnderlyingTypeT<MyEnum>, unsigned int>);
```

값 형질은 `Trait<...>::Value`와 `TraitV<...>`, 타입 변환은 `Trait<...>::Type`과 `TraitT<...>`를 제공한다.
`IntegralConstant<T, Value>`는 `ValueType`·`Type` 및 값 변환·호출 연산자를 제공한다.
`IntegerSequence`의 길이는 기존 계약대로 `Size`다.

## 제공 범위

| 기능군 | API |
|---|---|
| 상수·조건·탐지 | `IntegralConstant`, `BoolConstant`, `TrueType`, `FalseType`, `Conditional/T`, `EnableIf/T`, `VoidT`, `TypeIdentity/T` |
| 논리 조합 | `Conjunction/V`, `Disjunction/V`, `Negation/V`. 앞에서 결정되면 나머지 형질을 인스턴스화하지 않고 선택한 피연산자의 멤버를 보존한다 |
| 타입 분류 | `IsVoid`, `IsNullPointer`, `IsIntegral`, `IsFloatingPoint`, `IsArray`, `IsBoundedArray`, `IsUnboundedArray`, `IsEnum`, `IsScopedEnum`, `IsUnion`, `IsClass`, `IsFunction`, `IsPointer`, `IsLvalueReference`, `IsRvalueReference`, `IsReference`, 멤버 포인터 3종 |
| 복합 분류·한정 | `IsArithmetic`, `IsFundamental`, `IsScalar`, `IsObject`, `IsCompound`, `IsConst`, `IsVolatile`, `IsSigned`, `IsUnsigned` |
| 관계·변환 가능성 | `IsSame<A, B>`, `IsBaseOf<Base, Derived>`, `IsConvertible<From, To>`, `IsNothrowConvertible<From, To>` |
| 생성·대입·소멸 | `IsConstructible<T, Args...>`, `IsAssignable<To, From>`, `IsDestructible<T>`와 각각의 `Nothrow`·`Trivially` 계열. Default/Copy/MoveConstructible 및 Copy/MoveAssignable도 같은 3종 제공 |
| 클래스·객체 표현 | `IsTrivial`, `IsTriviallyCopyable`, `IsStandardLayout`, `IsEmpty`, `IsPolymorphic`, `IsAbstract`, `IsFinal`, `IsAggregate`, `HasVirtualDestructor`, `HasUniqueObjectRepresentations` |
| 변환 | `RemoveConst`, `RemoveVolatile`, `RemoveCv`, `RemoveReference`, `RemoveCvRef`, `AddConst`, `AddVolatile`, `AddCv`, `AddLvalueReference`, `AddRvalueReference`, `AddPointer`, `RemovePointer`, `RemoveExtent`, `RemoveAllExtents`, `Decay`, `MakeSigned`, `MakeUnsigned`, `UnderlyingType` |
| 배열·팩 | `Rank/V`, `Extent/V`, `IntegerSequence`, `IndexSequence`, `MakeIntegerSequence`, `MakeIndexSequence`, `IndexSequenceFor`, `TypePackElement` |
| 호출 | `DeclVal`, `InvokeResult/T`, `IsInvocable/V`, `IsNothrowInvocable/V`, `IsInvocableR/V`, `IsNothrowInvocableR/V` |
| Clang 확장 | `RemoveRestrict/T`, `RemoveAtomic/T` |

판별 형질에는 위 표의 이름마다 `V` 변수가 있다. 예를 들어 `IsInvocableR`의 변수 이름은 `IsInvocableRV`다.
타입 변환에는 `T` 별칭이 있다. 기존 `TypePackElement<Index, Types...>`는 자체가 타입 별칭이다.

## 의미와 경계

- 기본 분류·변환의 의미는 대응하는 표준 type trait을 따른다. `IsBaseOf`는 접근 가능한 포인터 변환 여부와 다르므로 필요에 따라 `IsConvertible`도 검사한다.
- 완전한 타입을 요구하는 형질에는 완전한 타입을 넘긴다. 불완전 타입의 완전성에 따라 결과가 달라지는 공개 형질을 만들어 번역 단위 간에 공유하지 않는다.
- `AddLvalueReferenceT<void>`, `AddRvalueReferenceT<void>`, `DeclVal<void>()`도 유효하다. copy/move 형질 역시 void 입력에서 잘못된 참조를 만들지 않는다.
- `UnderlyingType`은 enum이 아니면 `Type`이 없다. `InvokeResult`도 호출 불가능한 조합에서는 `Type`이 없어 탐지 문맥에서 사용할 수 있다. `MakeSigned`·`MakeUnsigned`는 표준과 같이 지원하는 정수/enum 타입만 받는다.
- `RemoveCvRefT`는 cv·참조만 제거한다. `DecayT`는 배열과 함수를 포인터로도 바꾸므로 두 변환을 서로 대체하지 않는다. 기존 Tuple·Variant·Function의 저장 타입 정책은 자동으로 바꾸지 않는다.
- 호출 형질은 함수 객체·함수 포인터·멤버 함수/객체 포인터를 지원한다. 멤버 포인터의 대상은 객체 또는 역참조 가능한 포인터류다. STL `reference_wrapper` 특별 취급은 제공하지 않으므로 호출자가 `.get()`을 넘긴다. `IsInvocableR<void>`는 반환값을 버릴 수 있는지 검사하며, nothrow 계열은 호출과 반환값 변환을 함께 검사한다.
- `RemoveRestrictT`·`RemoveAtomicT`는 프로젝트가 지원하는 Clang 확장이다. `RemoveAtomic`은 atomic이 아닌 타입에는 `Type`이 없다. MetaCompiler의 탐침은 이 API와 포인터/참조 변환을 통해 주소 공간·unaligned·restrict 및 여러 겹의 타입을 처리한다.

## 내장 직접 사용 방지

`Base/TypeTraits.h`만 내장 형질을 직접 사용한다. 부족한 기능은 이 API와 테스트를 먼저 확장한 뒤 사용한다.
정책 검사 `compiler-type-trait`는 현재 사용 목록만 열거하지 않고 내장 형질의 이름 계열을 검사한다.
소유 C++ 소스·헤더·include 조각·템플릿과 C# 생성 문자열·시험 코드도 검사하며 외부 라이브러리 원본은 제외한다.

MetaCompiler는 실제 조립한 생성 헤더와 Clang에 전달할 임시 C++에도 검사를 적용한다.
이 경계에서는 문자열 데이터와 주석을 호출로 오인하지 않으며, 형질 외 메모리/원자 builtin도 목적별 공개 helper를 사용하도록 한다.
Binds 탐침의 include 추가는 실패한 원본 선언을 찾는 줄 번호 계산에 반영한다.

## 검증

`Base/Tests/TypeTraitsTests.cpp`는 표준 라이브러리를 독립 기준으로 분류·변환·생명주기·호출 형질과 void·배열·함수·불완전 타입 경계를 검사한다.
기존 컨테이너·스마트 포인터·리플렉션 테스트는 공개 동작을 검증한다.
정책 시험은 호출부·C# 문자열·확장자별 위반, 공용 헤더 허용, 합성 C++ 및 데이터 문자열 구분을 검증한다.

