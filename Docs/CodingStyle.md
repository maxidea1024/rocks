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

공개 헤더(`Public/`)에는 쓰기 가능한 정적 저장소(const가 아닌 `inline` 변수, inline·템플릿 함수 안의 const가 아닌 `static` 지역 변수, `static inline` 데이터 멤버)와 `thread_local`(선언 포함)을 두지 않는다. 공유 라이브러리(`Modular`)마다 따로 생겨 상태가 갈라진다. 저장소는 소유 모듈의 `.cpp`에 두고 접근 함수로 노출한다. `constexpr`·const 상수는 괜찮다. 공개 헤더 검사의 Clang 진단 `-Wunique-object-duplication`과 정책 `public-header-static-storage`가 막는다([빌드 타깃](Design/BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 BD29).

## 형식 한계와 "없음" 표지

형식의 한계를 `0xffffffffu`·`UINT32_MAX`·`SIZE_MAX`·`INT_MAX`·`FLT_MAX`·`static_cast<usize>(-1)`·`~0u`·`2147483647` 같은 날 숫자나 C 매크로로 쓰지 않는다. 값이 무엇을 뜻하는지(상한인지, 비어 있다는 표지인지)를 이름으로 드러낸다.

- **최대·최소 검사**(계수 포화, 합의 상한, 좁히기 전 범위, "제한 없음" 한도 인자): `Base/NumericLimits.h`의 `NumericLimits<T>::Max()`·`Min()`·`Lowest()`(UE5.8 `TNumericLimits` 대응, constexpr, STL·CRT 비의존)를 쓴다. usize 최대값은 `MaxSize`다. T는 비교·대입하는 값의 형식과 같게 쓴다. `uint32` 칸을 `MaxSize`와 비교하면 항상 거짓이고, 부호가 다른 형식끼리는 명시 변환 뒤 비교한다(`static_cast<uint64>(NumericLimits<int32>::Max())`). 부동소수 `Min()`은 가장 작은 양의 정규 값이며 가장 작은 유한값은 `Lowest()`다.
- **"없음" 표지**(찾지 못한 색인, 빈 슬롯, 아직 정하지 않은 값): 이름 있는 상수로 쓴다. 공용 표지는 usize 색인·위치용 `IndexNone`(UE5.8 `INDEX_NONE` 대응. 부호 없는 색인이라 -1이 아니라 최대값이다) 하나만 둔다. 공개 검색 API의 `NotFound`·`NoField` 같은 영역 이름은 `IndexNone`으로 정의한다. uint32 같은 다른 폭의 칸이나 색인이 아닌 값은 그 형식으로 선언한 영역 상수(`SlotArray::EndOfList`·`FieldCodecFrame::NoIndex`·`InfiniteTimeout`·`ProcessExit::UnknownCode` 등)를 그 형식의 최대값(`NumericLimits<T>::Max()`, usize는 `MaxSize`)으로 정의해 쓴다. 공용 상수를 폭별로 늘리지 않는 이유는 폭이 다른 공용 표지를 잘못 골라 비교가 항상 거짓이 되는 실수를 막고, 칸 선언 옆에서 형식과 뜻을 함께 보이기 위해서다. 최대값 이름(`MaxSize`·`NumericLimits<T>::Max()`)을 "없음"·실패 표지로 직접 반환·비교하지 않고 이름을 붙인다.
- **예외(값 자체가 의미인 상수)**: 해시·CRC·FNV 초기값, 비트 마스크(`x & 0xffffffffu`, 부호 bit 마스크), 파일·wire 형식이나 알고리즘이 정한 값(zstd 사전 ID 범위, Linux 단일 read 한도 등. 이름 있는 형식 상수가 있으면 그것을 쓴다), 외부 API가 문서로 정한 값(`(DWORD)-1`, `D3D12_DEFAULT_SAMPLE_MASK` 같은 SDK 이름을 우선), 시험의 경계 입력(비트 패턴·10진 철자 자체를 검사하는 데이터)은 그대로 둔다. 시험도 한도 인자·"없음" 표지는 위 규칙을 따른다.
- 정책 검사 `raw-limit-literal`(`PolicyChecks.RawLimitLiterals`)이 런타임·도구 C++ 소스(모듈 `Tests/`·테스트 runner 제외)의 C 한계 매크로, 부호 없는 형식의 -1 변환, 0의 비트 반전을 항상 거부하고, 32·64-bit 모든 bit·부호 있는 최대값과 같은 정수 리터럴은 `&`·`|`·`^`의 피연산자(비트 마스크)가 아니면 거부한다. 그 밖의 값 자체가 의미인 자리는 검사의 허용 목록에 파일과 철자로 등록하고 이유를 적는다.

## 의미 있는 수치는 이름 있는 상수로

뜻이 있는 수치는 코드에 날 숫자로 반복해 쓰지 않고 `constexpr` 상수로 이름을 붙인다(2026-11-19 사용자 지시, 전체 규칙). 형식 한계와 "없음" 표지(위 절)는 이 규칙의 특수한 경우다.

- **대상**: word·블록·page·청크 크기와 그 배수 관계(예: 비트 word 64), 용량·임계값·예산(분할 임계, 재시도 횟수, 표본 수), 공간·시간 단위와 크기(타일 한 변, 셀 크기, 시간 한도), 비율·계수(이력 비율 3/4, 오차 여유 계수), 형식·프로토콜의 고정 값, 단위 변환 계수.
- **관계는 유도한다**: 한 값에서 나오는 값(shift, mask, word 수)은 그 상수에서 계산해 정의하고, 다른 형식·함수와 크기가 맞아야 하면 `static_assert`(영어 메시지)로 묶는다. 예: `BitsPerWord = 64`와 `uint64`, `CountTrailingZeros64`.
- **범위와 이름**: 한 함수에서만 쓰면 함수 안, 한 파일이면 익명 namespace·`detail`, 공개 계약이면 헤더의 공개 상수로 둔다. 이름은 뜻을 드러낸다(`FlatVisibilityBitsPerWord`처럼 영역 접두사). 이미 있는 공용 상수(`BitArray::BitsPerWord`, `NumericLimits<T>` 등)가 같은 뜻이면 그것을 쓴다.
- **성능을 이유로 바꾸지 않는다**: 2의 거듭제곱 상수 곱·나눗셈을 손으로 시프트로 바꾸지 않는다. 부호 없는 상수 곱은 컴파일러가 시프트로 바꾸며, 손으로 바꾸면 뜻만 흐려진다. 측정으로 차이가 확인된 경우만 예외로 두고 근거를 주석에 적는다.
- **예외**: 산술 항등원과 자명한 식의 0·1·2(빈 값, 다음 색인, 절반·두 배), 벡터 성분 색인(0·1·2), 식 바로 옆에서 이름 있는 변수에 담기는 일회성 값, 시험의 입력 데이터(경계 값 자체를 검사하는 철자)는 그대로 둔다. 위 절의 예외(해시 초기값, 외부 API 값 등)도 같다.
- 자동 정책 검사는 한계값 리터럴(`raw-limit-literal`)만 있다. 나머지는 리뷰에서 본다.

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

## 결과물·빌드 설정·실행 중 메시지

실행 중에 보이는 문자열(로그·추적 채널 설명·진단·오류·도움말), 생성기가 내는 파일(머리 주석과 본문 주석 포함), 빌드 설정 파일은 영어로 쓴다. 손으로 쓴 C++·C# 소스의 주석은 한국어로 쓴다. 단 빌드 설정에 속하는 파일(`*.Build.cs`·`*.Target.cs` 등)은 주석까지 영어다. 문체는 기존 영어 static_assert·오류 메시지처럼 간결하고 사실적으로 쓰며, 경로·명령·식별자는 그대로 둔다. 정책 검사 둘이 거부한다.

- `source-literal-english`(`PolicyChecks.EnglishSourceLiterals`): `Core/Source/Runtime`·`Core/Source/Developer`·`Engine/Source/Runtime`·`Engine/Source/Developer`·`Engine/Plugins`·`Programs`·`Build`의 C++(.h·.c·.cpp·.mm)·C#(.cs) 소스에서 주석 밖의 한글(문자열·문자 리터럴 등)을 거부한다. 주석(`//`·`/* */`·`///`)은 보지 않는다.
- `build-file-english`(`PolicyChecks.EnglishBuildFiles`): 다음 파일은 주석을 포함해 파일 전체에서 한글을 거부한다.
  - 저장소 루트의 `Rocks.proj`·`.editorconfig`·`.gitattributes`·`.gitignore`·`.clang-format`·`global.json`·`Directory.Build.rsp`·`.vsconfig`, Visual Studio 프로젝트 파일 생성 진입점 `GenerateProjectFiles.bat`·`GenerateProjectFiles.ps1`([설계](Design/ProjectFiles.ko.md) 12절)와 설치 마법사 진입 스크립트 `Setup.bat`·`Setup.sh`
  - `.github/workflows/` 아래 파일
  - 모든 `*.Build.cs`·`*.Target.cs`·`*.csproj`·`*.props`·`*.targets`·`*.slnx`·`*.proj`(시험 실행 파일·시험 fixture 모듈의 규칙 포함)
  - `Build/` 아래 모든 파일: 스크립트·설정(`*.ps1`·`*.sh`·`*.cmake`·`Build.version` 등)과 모듈·타깃 규칙 기반 클래스(`Build/Rules/*.cs`). 빌드 출력 디렉터리는 제외한다.
  - ThirdParty 라이브러리 폴더(`Core/Source/ThirdParty/<Lib>/`·`Engine/Source/ThirdParty/<Lib>/`)에서 Rocks가 쓴 파일: `ThirdParty.json`(notes·verification·modifications reason 등), `*.Build.cs`, 도구 빌드 스크립트·cmake(`Build/`), patch, unity 파일(예: `Lz4Unity.c`)
  - `Templates/`(새 게임 프로젝트로 복사되는 결과물), `Programs/Scaffold/Templates/`(scaffold 생성 원본), `Engine/Config/`·`Core/Config/`
  - 생성 파일(`*.generated.*`, 시험 디렉터리 안의 것 포함). 체크인된 생성 파일은 생성기를 고친 뒤 생성기로 다시 만든다(예: `Rocks.ToolTests unicode-tables --root . --write`).

예외는 검사 코드(`PolicyChecks.EnglishArtifacts.cs`)에 명시 목록과 이유로 둔다.

- 문서(`Docs/`·`*.md`·`*.svg`·`*.pdf`): 설계·사용 문서는 한국어로 쓴다.
- 외부 원본(ThirdParty.json의 `upstreamDirectory`): 업스트림 byte를 그대로 보존한다(예: `UnicodeData/ucd-*/NormalizationTest.txt`).
- 시험 소스(모듈 `Tests/`·`Programs/FoundationTests/`·C# 시험 프로젝트 `Programs/Rocks.*Tests/`)와 시험 fixture 모듈(`*TestFixtures`): 유니코드 처리를 시험하려고 한글 경로·문자열·DisplayName을 일부러 쓴다. 문자열 규칙(`source-literal-english`)만 면제하며, 이들의 빌드 규칙과 생성 파일은 `build-file-english` 대상이다. 모듈 `Private/Tests/`는 모듈에 컴파일되어 출하되므로 시험 소스가 아니다.
- 빌드 출력 디렉터리(`Build/Artifacts/`·`Build/Cache/`, .gitignore 대상)

## 중괄호 목록 추론

형식 추론이 `std::initializer_list`를 요구하는 문법(중괄호 목록 range-for, auto 중괄호 복사 초기화)은 `<Base/InitializerList.h>`를 직접 include한 파일에서만 쓴다. 엔진은 STL 헤더를 쓰지 않으므로 이 헤더 없이는 Android(NDK) 빌드에 `std::initializer_list`가 보이지 않아 컴파일되지 않는다(Windows에서는 다른 STL 헤더가 간접으로 들여와 우연히 통과할 수 있다).

- 허용 형태: `for (bool x : {true, false})`, `auto values = {1, 2};`. 지역 배열(`const bool cases[] = {true, false}; for (bool x : cases)`)도 여전히 쓸 수 있다.
- `<initializer_list>`는 `Base/InitializerList.h`만 include한다. 다른 파일은 래퍼를 include한다(직접 include 금지 — 표준 헤더 철자를 한 곳에 모아 STL 예외 범위를 좁힌다).
- `std::initializer_list` 이름을 코드에 직접 쓰는 일(컨테이너의 initializer_list 생성자 등)은 이 예외에 들지 않는다. Runtime의 `std::` 금지(`PolicyChecks.Runtime`)가 그대로 막는다. 필요하면 따로 결정한다.
- 전역으로 열지 않는 이유: MSVC `<initializer_list>`는 `<cstddef>`·`<xtr1common>`·`yvals_core.h`를 함께 열어 PCH 없는 번역 단위마다 약 25 ms(2026-11-22 측정, Clang 22.1.8 `-fsyntax-only` 81 ms 대 55 ms)가 든다. `Base/Types.h`에 넣으면 공개 헤더 검사처럼 PCH를 쓰지 않는 모든 번역 단위가 이 비용을 낸다. 그래서 `Base/PlacementNew.h`와 같은 opt-in 래퍼로 둔다.
- 정책 검사 `initializer-list-deduction`(`PolicyChecks.InitializerListDeductions`)이 엔진 소유 C++ 소스 전체(시험 포함)에서 래퍼를 직접 include하지 않은 파일의 두 형태를 거부하고, `initializer-list-header`가 래퍼 밖의 `<initializer_list>` 직접 include를 거부한다. 간접 include(다른 헤더가 래퍼를 include)는 인정하지 않는다. 쓰는 파일이 스스로 include해야 include 정리에 깨지지 않는다.

## 컴파일러 진단

**컴파일러 경고는 허용하지 않고 오류로 처리한다.** 로컬·CI와 모든 빌드 구성의 엔진 소유 코드에 같은 기준을 적용한다. `/W4 /WX` 또는 `-Wall -Wextra -Wpedantic -Werror`를 필수로 사용하며 경고 억제·강등으로 해결하지 않는다. 생성 코드·테스트도 예외가 아니다. 실제 컴파일 실패 하네스와 상세 규약은 [컴파일러 경고 정책](Design/CompilerDiagnostics.ko.md)을 따른다.

플랫폼 분기는 빌드 시스템의 private 구현 파일 선택으로 제한하고 공용 코드에 OS 전처리·헤더를 확산시키지 않는다.

## 자동 검사와 사람의 검토

**clang-format 21.1.6**을 사용한다. 버전을 고정하고 버전 변경은 독립 변경으로 검토한다. `dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll format --root .`(`Programs/Rocks.Tooling.Checks`의 FormatCheck)가 Core·Engine·Programs·Templates의 C++ 소스 서식을 검사한다. CI에도 같은 검사를 연결한다.

C#(빌드 도구·검사 도구·모듈·타깃 규칙·게임 템플릿 규칙)은 Microsoft C# 규칙을 따른다(Allman 중괄호, 본문이 한 줄이어도 중괄호, 블록 뒤 빈 줄, 한 줄에 문장 하나). 같은 `format` 명령이 `global.json`이 고정한 .NET SDK의 `dotnet format`으로 `Build/Format/Rocks.Format.slnx`(저장소의 C# 프로젝트 전부)를 한 번에 검사하며 규칙은 `.editorconfig`의 `[*.cs]` 절이다. 저장소의 `.cs`(외부 라이브러리 원본·bin·obj 제외)가 모두 그 솔루션의 프로젝트에 속하는지도 확인해, 어느 프로젝트에도 속하지 않아 검사에서 빠지는 파일을 알린다. 위반은 `경로:행: 규칙 ID: 내용`으로 출력하고 `--write`를 붙이면 두 포매터가 고친다. 멤버 사이 빈 줄은 dotnet format에 해당 규칙이 없어 같은 `format` 명령이 Roslyn 구문 트리로 따로 본다(규칙 `member-blank-line`, 2026-11-25).

C# 멤버 사이 빈 줄 규칙(`member-blank-line`, 2026-11-25 — 교차 검증 반영):
- 대상: 선언 목록 안에서 선언이 바로 앞 선언 뒤에 올 때, **앞 선언의 마지막 줄 바로 다음 줄이 빈 줄**이어야 한다(공백만 있는 줄도 빈 줄이다). MS C# 규칙의 "메서드·속성 정의 사이에 빈 줄"을 모든 멤버로 넓힌 것이다.
  - 선언 목록: 형식 본문(class·struct·record·interface)의 멤버, namespace 본문(file-scoped 포함)과 컴파일 단위의 형식·대리자·namespace 선언.
  - 멤버: 메서드·생성자·소멸자·속성·인덱서·이벤트·연산자·변환 연산자·중첩 형식·대리자·필드. 인터페이스의 본문 없는 선언과 한 줄 자동 속성도 대상이다.
  - 대상 아님: 레코드 위치 매개변수·primary constructor 매개변수(매개변수 목록), 객체 초기화자·익명 형식 멤버(editorconfig new_line 규칙 몫), 지역 함수(문장 — IDE2003 몫).
- 예외:
  - 필드(`const` 포함)와 이벤트 필드가 연속하면 빈 줄 없이 묶어도 된다(.NET 런타임 관례 — 필드 무리).
  - 열거형 멤버는 대상이 아니다.
  - 목록의 첫 선언은 앞 선언이 없으므로 대상이 아니다. 여는 `{` 바로 뒤의 빈 줄을 막는 것(StyleCop SA1505)은 이 규칙의 범위가 아니다.
  - 두 선언 사이에 전처리 지시문(`#if`·`#region` 등)이 있으면 보지 않는다(지시문의 위치가 뜻을 가진다). `#if`의 꺼진 쪽은 Roslyn이 disabled text로 두므로 그 안의 선언은 보이지 않는다.
- 주석·특성: 뒤 선언의 주석(`//`·`///`)과 특성은 뒤 선언에 속한다. 그래서 빈 줄은 그 위(앞 선언의 마지막 줄 바로 다음)에 와야 한다 — `}` / `// 설명` / 빈 줄 / `void X()`는 위반이다. 줄 끝 주석(`}  // 끝`)은 Roslyn이 앞 토큰에 붙이므로 앞 선언에 남는다.
- 구현: `Rocks.Tooling.Checks`의 `CSharpMemberSpacing`이 `Microsoft.CodeAnalysis.CSharp` 5.9.0(정확한 버전 하나로 고정)의 구문 트리만 쓴다. 파싱 언어 버전은 `LanguageVersion.CSharp14`(Programs/Directory.Build.props의 LangVersion 14.0과 같다)이고, 컴파일·의미 분석은 하지 않는다.
  - 대상 파일은 `Build/Format/Rocks.Format.slnx` 프로젝트들의 Compile 항목이다(서식 검사의 소속 확인과 같은 목록).
  - 위반은 `경로:행: member-blank-line: 내용`으로 알린다. 행은 뒤 선언의 첫 줄(주석·특성 포함)이다. 구문 오류가 있는 파일은 건너뛰지 않고 `경로:행: member-blank-line: parse error`로 알린다.
  - `--write`는 뒤 선언 leading trivia의 맨 앞(들여쓰기 앞)에 줄 끝 하나를 넣는다. 줄 끝 문자(CRLF·LF)는 그 파일의 기존 줄 끝을 따르고, 인코딩·BOM은 그대로 둔다. 빈 줄 하나만 넣으므로 IDE2000(연속 빈 줄)을 만들지 않는다.
  - 순서(`--write`): 빈 줄 수정 → dotnet format 수정 → 둘 다 다시 검증. 검사만 할 때는 dotnet format 검증과 빈 줄 검사를 함께 알린다.
- 버린 대안:
  - 빌드 분석기(Roslyn analyzer 프로젝트를 모든 C# 프로젝트에 참조): `TreatWarningsAsErrors`라 위반이 곧 빌드 실패가 된다. 서식은 빌드가 아니라 format 검사의 몫이라는 지금 구조와도 어긋난다.
  - 텍스트(정규식) 검사: 문자열·주석 안의 중괄호, 한 줄 멤버, 특성·주석의 소속을 바르게 가르지 못한다.
- 도입 절차(공유 트리):
  1. 검사기·시험 커밋과 기계적 `--write` 수정 커밋(동작 불변)으로 나눈다.
  2. 수정 커밋 전에 `git status`로 다른 세션이 고치고 있는 `.cs`를 확인한다. 겹치면 worktree에서 커밋해 update-ref로 반영하거나, 그 파일은 그 세션에 알리고 빼 둔다.
  3. 두 커밋은 함께 push한다(CI format 검사가 중간에 실패하지 않게).
- 시험(Rocks.ToolTests, 결정적 입력 문자열): 예외마다 양성·음성 쌍, 주석·특성 위치(위의 모호한 사례 포함), namespace·컴파일 단위 목록, 구문 오류 보고, 지시문 사이, CRLF·LF 보존, `--write` 뒤 재검사 0과 IDE2000 없음.

빌드 시 공개 헤더를 각각 단독 include한 번역 단위로 컴파일한다. `dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll policy --root .`(PolicyChecks.Runtime)는 런타임 소스(Core·Engine·플러그인·프로그램, 모듈 `Tests/` 제외)의 금지 include와 `std::` 사용, 날 형식 한계 리터럴(`raw-limit-literal`)을 검사한다. 이 검사는 외부 SDK 내부나 최종 바이너리의 모든 의존성을 증명하는 검사는 아니다.

포맷터는 책임 분리, 명명 품질, 주석의 정확성, 수명과 동시성 규약의 이해 가능성을 판정하지 않는다. 이 항목들은 코드 리뷰와 하네스 검증을 함께 요구한다.
