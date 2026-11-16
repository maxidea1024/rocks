# Rocks 코드 스타일

**사람이 알아보기 좋은 코드 스타일 유지**를 필수 구현·리뷰 기준으로 적용한다.

## 서식과 가독성

- 들여쓰기는 스페이스 4칸이다. 실제 탭 문자는 사용하지 않는다.
- 함수·타입 정의와 의미가 다른 처리 단계 사이에는 적절한 빈 줄을 둔다.
- 이름만으로 역할과 단위가 드러나도록 작성한다. 긴 함수는 책임 경계에 따라 나눈다.
- 여러 처리 단계나 분기를 한 줄에 압축하지 않는다. 서식 검사를 통과하더라도 읽기 어려운 코드는 리뷰에서 수정한다.
- 줄 길이는 기본 100열이다. 자동 정렬 규칙은 저장소 루트의 `.clang-format`을 따른다.
- 파일은 UTF-8, LF, 마지막 개행을 사용한다. 에디터 설정은 `.editorconfig`를 따른다.

## 주석과 API 계약

**주석을 생략하는 규칙은 없다.** Core와 Object처럼 여러 모듈이 의존하는 기반 코드에는 호출자가 알아야 할 계약과 구현 선택의 이유를 한국어 주석으로 남긴다.

- 공개 타입에는 역할, 자원 소유자, 먼저 파괴해야 할 대상, 비소유 뷰의 유효 기간을 설명한다.
- 필요한 API에는 호출 가능한 스레드, 동시 사용 제한, 재진입 허용 범위, 실패 시 유지되는 상태를 설명한다. 계수에 잠금이 있다는 이유만으로 객체 전체가 스레드 안전하다고 쓰지 않는다.
- 포인터·뷰·반복자의 무효화 조건, 문자열 길이와 시간의 단위, null/empty 의미를 구분한다.
- 복잡한 내부 로직은 **이 순서를 지켜야 하는 이유**와 보호하는 불변식을 설명한다. GC의 부분 표식 실패, 콜백 잠금 해제, 생성 저널 선할당 등이 해당한다.
- 테스트에는 비자명한 반례와 기대하는 실패 이유를 남긴다. 버그 재현 테스트는 어떤 잘못된 동작을 막는지 알 수 있어야 한다.
- 이름을 그대로 읽어 주는 주석, 특수화마다 반복하는 동일 설명, 구현되지 않은 보장, 버전이 지나면 틀리는 줄 번호 나열은 피한다.
- 계약을 바꾸면 코드·주석·테스트를 같은 변경에서 갱신한다. 주석은 자동 테스트를 대신하지 않는다.

짧은 문맥 설명은 `//`를 사용한다. 공개 API 계약도 선언 가까이에 배치하여 헤더만 읽고 소유권과 실패 경계를 이해할 수 있도록 한다. 장문의 설계 배경은 설계 문서에 두되 안전한 사용에 필요한 전제는 헤더에도 남긴다.

## 이름과 경계

공개 C++ API는 `rocks` 네임스페이스와 자체 이름을 사용한다. U/F/A 등 타입 분류용 접두사를 사용하지 않는다. C ABI·외부 언어 연결에는 충돌 방지를 위한 `rocks_` 접두사를 검토하며, 외부 공급자의 네이밍은 adapter 내부로 제한한다.

엔진 소유 Runtime과 공개 API에는 STL 컨테이너·문자열·알고리즘을 사용하지 않는다. 문자열은 자체 `String`/`StringView`, 번역 가능한 표시 문구는 별도 `Text` 계층으로 구분한다. 각 타입의 실제 구현 여부는 구현 상태 문서를 따른다. 외부 SDK와 호스트 도구는 설계에 명시한 경계에서 격리한다.

## 형식 한계와 "없음" 표지

형식의 한계를 `0xffffffffu`·`UINT32_MAX`·`SIZE_MAX`·`INT_MAX`·`FLT_MAX`·`static_cast<usize>(-1)`·`~0u`·`2147483647` 같은 날 숫자나 C 매크로로 쓰지 않는다. 값이 무엇을 뜻하는지(상한인지, 비어 있다는 표지인지)를 이름으로 드러낸다.

- **최대·최소 검사**(계수 포화, 합의 상한, 좁히기 전 범위, "제한 없음" 한도 인자): `Base/NumericLimits.h`의 `NumericLimits<T>::Max()`·`Min()`·`Lowest()`(UE5.8 `TNumericLimits` 대응, constexpr, STL·CRT 비의존)를 쓴다. usize 최대값은 `MaxSize`다. T는 비교·대입하는 값의 형식과 같게 쓴다. `uint32` 칸을 `MaxSize`와 비교하면 항상 거짓이고, 부호가 다른 형식끼리는 명시 변환 뒤 비교한다(`static_cast<uint64>(NumericLimits<int32>::Max())`). 부동소수 `Min()`은 가장 작은 양의 정규 값이며 가장 작은 유한값은 `Lowest()`다.
- **"없음" 표지**(찾지 못한 색인, 빈 슬롯, 아직 정하지 않은 값): 이름 있는 상수로 쓴다. 공용 표지는 usize 색인·위치용 `IndexNone`(UE5.8 `INDEX_NONE` 대응. 부호 없는 색인이라 -1이 아니라 최대값이다) 하나만 둔다. 공개 검색 API의 `NotFound`·`NoField` 같은 영역 이름은 `IndexNone`으로 정의한다. uint32 같은 다른 폭의 칸이나 색인이 아닌 값은 그 형식으로 선언한 영역 상수(`SlotArray::EndOfList`·`FieldCodecFrame::NoIndex`·`InfiniteTimeout`·`ProcessExit::UnknownCode` 등)를 그 형식의 최대값(`NumericLimits<T>::Max()`, usize는 `MaxSize`)으로 정의해 쓴다. 공용 상수를 폭별로 늘리지 않는 이유는 폭이 다른 공용 표지를 잘못 골라 비교가 항상 거짓이 되는 실수를 막고, 칸 선언 옆에서 형식과 뜻을 함께 보이기 위해서다. 최대값 이름(`MaxSize`·`NumericLimits<T>::Max()`)을 "없음"·실패 표지로 직접 반환·비교하지 않고 이름을 붙인다.
- **예외(값 자체가 의미인 상수)**: 해시·CRC·FNV 초기값, 비트 마스크(`x & 0xffffffffu`, 부호 bit 마스크), 파일·wire 형식이나 알고리즘이 정한 값(zstd 사전 ID 범위, Linux 단일 read 한도 등. 이름 있는 형식 상수가 있으면 그것을 쓴다), 외부 API가 문서로 정한 값(`(DWORD)-1`, `D3D12_DEFAULT_SAMPLE_MASK` 같은 SDK 이름을 우선), 시험의 경계 입력(비트 패턴·10진 철자 자체를 검사하는 데이터)은 그대로 둔다. 시험도 한도 인자·"없음" 표지는 위 규칙을 따른다.
- 정책 검사 `raw-limit-literal`(`PolicyChecks.RawLimitLiterals`)이 런타임·도구 C++ 소스(모듈 `Tests/`·테스트 runner 제외)의 C 한계 매크로, 부호 없는 형식의 -1 변환, 0의 비트 반전을 항상 거부하고, 32·64-bit 모든 bit·부호 있는 최대값과 같은 정수 리터럴은 `&`·`|`·`^`의 피연산자(비트 마스크)가 아니면 거부한다. 그 밖의 값 자체가 의미인 자리는 검사의 허용 목록에 파일과 철자로 등록하고 이유를 적는다.

## 타입 형질

타입 검사·변환은 `<Base/TypeTraits.h>`의 공용 API를 쓴다. 값은 `IsBaseOfV<Base, Derived>`처럼 `V` 접미사,
타입 변환은 `RemoveCvRefT<T>`처럼 `T` 접미사를 사용한다. trait 자체를 전달할 때는 `IsBaseOf<Base, Derived>`
등의 형식과 `::Value`·`::Type`을 쓴다. 기존 `<Core/Traits.h>` 경로도 유지하지만 새 코드는 Base 헤더를 직접 포함한다.

컴파일러 내장 형질은 `Base/TypeTraits.h`에서만 사용한다. 필요한 기능이 없으면 해당 헤더에 의미·지원 범위와
경계 타입 테스트를 추가하고 호출부는 새 API를 사용한다. C++ 테스트, C# 생성 문자열, 코드 템플릿도 같은 규칙이다.
정책 검사 `compiler-type-trait`는 형질 계열 전체를 검사하며, MetaCompiler는 실제 생성 헤더와 Clang에 넘기는
합성 C++에도 검사를 적용한다. 기능 검사 매크로와 플랫폼 intrinsic은 기존 목적별 정책을 따른다.
지원 목록과 완전성·확장 타입 계약은 [타입 형질](Implementation/TypeTraits.ko.md)에 정리한다.

## 컴파일 시점 메시지

`static_assert`의 메시지와 `#error` 문구는 영어로 쓴다(MetaCompiler가 생성 코드에 내는 static_assert 메시지도 영어다). 주석은 한국어로 쓴다. 정책 검사 `compile-time-message-english`(`PolicyChecks.CompileTimeMessages`)가 엔진 소유 C++ 소스 전체(시험 포함)에서 두 메시지에 한글이 있으면 거부한다(주석은 보지 않는다).

## 중괄호 목록 추론

형식 추론이 `std::initializer_list`를 요구하는 문법을 쓰지 않는다. 엔진은 STL 헤더를 쓰지 않으므로 Android(NDK) 빌드에는 `<initializer_list>`가 없어 컴파일되지 않는다(Windows에서는 시스템 헤더가 간접으로 들여와 통과한다).

- range-for의 범위를 중괄호 목록으로 쓰지 않는다: `for (bool x : {true, false})` 대신 `const bool cases[] = {true, false}; for (bool x : cases)`.
- auto 변수를 중괄호 목록으로 복사 초기화하지 않는다: `auto values = {1, 2};` 대신 배열이나 `Array`.
- 정책 검사 `initializer-list-deduction`(`PolicyChecks.InitializerListDeductions`)이 엔진 소유 C++ 소스 전체(시험 포함)에서 두 형태를 거부한다.

## 컴파일러 진단

**컴파일러 경고는 허용하지 않고 오류로 처리한다.** 로컬·CI와 모든 빌드 구성의 엔진 소유 코드에 같은 기준을 적용한다. `/W4 /WX` 또는 `-Wall -Wextra -Wpedantic -Werror`를 필수로 사용하며 경고 억제·강등으로 해결하지 않는다. 생성 코드·테스트도 예외가 아니다. 실제 컴파일 실패 하네스와 상세 규약은 [컴파일러 경고 정책](Design/CompilerDiagnostics.ko.md)을 따른다.

플랫폼 분기는 빌드 시스템의 private 구현 파일 선택으로 제한하고 공용 코드에 OS 전처리·헤더를 확산시키지 않는다.

## 자동 검사와 사람의 검토

**clang-format 21.1.6**을 사용한다. 버전을 고정하고 버전 변경은 독립 변경으로 검토한다. `dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll format --root .`(`Programs/Rocks.Tooling.Checks`의 FormatCheck)가 Core·Engine·Programs·Templates의 C++ 소스 서식을 검사한다. CI에도 같은 검사를 연결한다.

C#(빌드 도구·검사 도구·모듈·타깃 규칙·게임 템플릿 규칙)은 Microsoft C# 규칙을 따른다(Allman 중괄호, 본문이 한 줄이어도 중괄호, 블록 뒤 빈 줄, 한 줄에 문장 하나). 같은 `format` 명령이 `global.json`이 고정한 .NET SDK의 `dotnet format`으로 `Build/Format/Rocks.Format.slnx`(저장소의 C# 프로젝트 전부)를 한 번에 검사하며 규칙은 `.editorconfig`의 `[*.cs]` 절이다. 저장소의 `.cs`(외부 라이브러리 원본·bin·obj 제외)가 모두 그 솔루션의 프로젝트에 속하는지도 확인해, 어느 프로젝트에도 속하지 않아 검사에서 빠지는 파일을 알린다. 위반은 `경로:행: 규칙 ID: 내용`으로 출력하고 `--write`를 붙이면 두 포매터가 고친다. 멤버 사이 빈 줄은 이 검사에 해당 규칙이 없어 리뷰에서 확인한다.

빌드 시 공개 헤더를 각각 단독 include한 번역 단위로 컴파일한다. `dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll policy --root .`(PolicyChecks.Runtime)는 런타임 소스(Core·Engine·플러그인·프로그램, 모듈 `Tests/` 제외)의 금지 include와 `std::` 사용, 날 형식 한계 리터럴(`raw-limit-literal`)을 검사한다. 이 검사는 외부 SDK 내부나 최종 바이너리의 모든 의존성을 증명하는 검사는 아니다.

포맷터는 책임 분리, 명명 품질, 주석의 정확성, 수명과 동시성 규약의 이해 가능성을 판정하지 않는다. 이 항목들은 코드 리뷰와 하네스 검증을 함께 요구한다.
