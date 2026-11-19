# 리플렉션 생성 소스 설계

기준일: 2026-11-19, 개정 1. 메타 컴파일러가 내는 리플렉션 코드를 둘로 나눈다.
- **생성 헤더**(`X.generated.h`): 선언, 컴파일 타임 형질, 작은 inline 함수만 둔다.
- **생성 소스**(`X.generated.cpp`): 스키마 표, 필드 연산 표, 등록 함수 같은 본문을 둔다. 모듈이 한 번만 컴파일한다.

지금은 본문 전체가 생성 헤더에 inline으로 들어 있어, 그 헤더를 include하는 번역 단위마다 같은 코드를 다시 인스턴스화하고 코드 생성한다. 시험 빌드 시간의 가장 큰 원인이다(1절).

다른 문서와의 관계:
- [빌드 타깃](BuildTargets.ko.md) 3.8절 BD28이 모듈식 링크를 위해 "생성 `.cpp`를 낸다"고 이미 정했다(BS8 단계, 미구현). 이 문서는 그 가운데 **생성 코드를 헤더에서 소스로 옮기는 부분**의 구현 스펙이고, 컴파일 시간 때문에 BS8보다 먼저 한다. 모듈 API 매크로 주입과 `NativeTypeKey` 교체는 BS8에 그대로 둔다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절(생성물 배치)·1.2절(검증 컴파일)과 등록 절의 집계 헤더 서술을 바꾼다(13절).
- [모듈 로더](ModuleLoader.ko.md) ML2(개정 3 작업 중)는 빌드가 만드는 TU `<M>.ModuleTypes.cpp`에 "모듈 자신의 타입만 등록하는 함수"를 두려 했다. 이 문서의 모듈 생성 소스가 그 자리를 대신한다. ML2는 GS1 병합 뒤에 그 함수와 진입점 연결을 모듈 생성 소스 구조 위에 더한다(모듈 로더 세션과 2026-11-19 조율).
- [유니티 빌드·PCH](UnityBuildPch.ko.md)는 "리플렉션 생성물은 헤더뿐"을 전제로 썼다. 생성 소스가 생기므로 그 전제를 고친다(13절).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 UnrealHeaderTool이다. 경로는 `Engine/Source/Programs/Shared/EpicGames.UHT/Exporters/CodeGen/` 기준이다. UE 이름은 대응표와 근거에만 적는다. 측정은 Rocks 툴체인(clang-cl 21.1.6, foundation-debug)으로 했다.

상태: **설계 확정**(결정 GSD1–GSD14, 2026-11-19 사용자 승인 — 권장안, GSD13은 보류). 구현 단계는 GS0–GS5(11절)다. GS1(등록 함수 분리)은 2026-11-19 사용자 결정으로 착수했다. 다른 세션은 GS1 병합까지 전체 빌드·시험 반복을 보류한다. GS0(action 소요 시간 기록·기준 측정)과 GS1은 2026-11-20 구현했다(브랜치 `feature/generated-sources`, 측정값은 9절).

## 1. 현재 상태와 측정

### 1.1 구조

| 항목 | 현재 | 근거 |
|---|---|---|
| 타입별 생성 헤더 | `GeneratedType<T>` 특수화 하나에 형질, `Id()`, `Schema()`, `Trace()`, `Ops()`, `Register()` 등이 모두 inline 본문으로 들어 있다 | `Programs/Rocks.MetaCompiler/CppEmitter.cs:223`(클래스), `:625`(struct), `:952`(enum) |
| 필드 연산 | `Ops()`가 필드마다 읽기·쓰기·staged 쓰기·encode·decode·경로 thunk를 람다로 만들고, 각 람다가 `detail::ReadGeneratedField<T>` 같은 템플릿을 인스턴스화한다 | `CppEmitter.cs:305–396`, `:727–769` |
| 모듈 집계 헤더 | `<M>.module.generated.h`가 모듈의 모든 생성 헤더와 의존 모듈의 집계 헤더를 include하고, inline `RegisterModuleTypes`가 모든 타입의 `Schema()`·`Ops()`를 부른다 | `CppEmitter.cs:75–91` |
| 생성 헤더의 include | 원본 헤더와 codec·경로·접근 헤더(`Object/GeneratedFieldCodec.h`·`GeneratedPathAccess.h`·`GeneratedFieldAccess.h` 등, 합계 수천 줄) | `CppEmitter.cs:149–201` |
| 검증 컴파일 | 메타 컴파일러가 게시 전에 생성 헤더마다 `-fsyntax-only` 컴파일을 **순차로** 한다(헤더당 timeout 2분). 모듈이 다시 생성될 때마다 모든 헤더를 다시 검증한다 | `Programs/Rocks.MetaCompiler/CompilerDriver.cs:75, 241–293` |
| 생성 소스 | 없다. 생성물은 헤더와 descriptor뿐이다 | `Programs/Rocks.MetaCompiler/CompilerOptions.cs:33` |
| 소스 없는 리플렉션 모듈 | ReflectionTestFixtures·CoreTypeBindings·ConfigTestFixtures·CurvesTestFixtures는 `.cpp`가 없어 header-only 모듈이다(archive 없음) | `Programs/Rocks.Build/README.ko.md` "현재 지원 범위" |

### 1.2 측정(2026-11-19)

조건: foundation-debug의 compile 명령 그대로, clang `-ftime-trace`. 다른 세션의 빌드가 돌던 중이라 절대 시간에는 잡음이 있다. 구성 비율과 자릿수는 유효하다.

**생성 헤더를 끌어오는 번역 단위**(FoundationTests 385개, 전처리로 include 목록만 확인):

| 구분 | TU 수 |
|---|---|
| 생성 헤더를 하나라도 include | 96 |
| 모듈 집계 헤더를 include | 51 |
| — CoreTypeBindings 집계 | 51 |
| — ReflectionTestFixtures 집계 | 28 |
| — Config·ConfigTestFixtures 집계 | 18 |
| — Curves·CurvesTestFixtures 집계 | 4 |

**시험용 TU 하나의 비용**(같은 compile 인자, 소스만 바꿈):

| TU 내용 | 전체 | 헤더 파싱 | 인스턴스화 | 코드 생성 | object |
|---|---|---|---|---|---|
| 등록 함수를 **선언만 보고** 호출 | 0.6 s | 0.3 s | 0.0 s | 0.0 s | 11 KB |
| ReflectionTestFixtures 집계 헤더 include만(호출 없음) | 16.8 s | 4.3 s | 12.3 s | 0.1 s | 6 KB |
| 같은 헤더 include + `RegisterModuleTypes` 호출 | 46.2 s | 4.0 s | 11.1 s | 21.5 s | 54 MB |
| CoreTypeBindings 집계 헤더 include + 호출 | 4.3 s | 1.3 s | 0.9 s | 1.3 s | 3.3 MB |
| 등록은 선언으로 부르고 타입별 생성 헤더 2개만 include(GS1 뒤의 전형적인 시험 TU) | 6.5 s | 2.6 s | 2.9 s | 0.0 s | 11 KB |

**실제 시험 TU**:

| TU | 전체 | 헤더 파싱 | 인스턴스화 | 코드 생성 |
|---|---|---|---|---|
| `ReflectionText/Tests/TextAssignAppendTests.cpp`(351줄) | 48 s | 4.2 s | 12.7 s | 21.3 s |
| `ObjectSerialization/Tests/ArrayGraphTests.cpp` | 54 s | 5.1 s | 13.9 s | 24.2 s |
| `ObjectRuntime/Tests/Reflection/AtomicStructTests.cpp` | 19 s | 5.8 s | 3.7 s | 5.8 s |
| 생성 헤더를 쓰지 않는 시험 TU 6개 | 0.4–4 s | 절반 안팎 | 거의 0 | 작음 |

해석:
- include만 해도 본문 안의 템플릿이 인스턴스화된다. inline 멤버 함수의 본문은 쓰이지 않아도 파싱되고, 본문이 참조한 함수 템플릿은 TU 끝에서 인스턴스화되기 때문이다. 코드 생성은 그 함수가 실제로 쓰일 때만 일어난다.
- ReflectionTestFixtures 집계 헤더로 등록하는 TU 28개가 각각 약 46초, object 54 MB를 만든다. 합치면 약 21 CPU-분, object 1.5 GB이고 링커가 같은 COMDAT을 28번 받아 하나로 줄인다.
- 선언만 보고 부르면 0.6초다. 본문을 한 번만 컴파일하면 나머지 TU는 이 비용에 가까워진다.
- 엔진 코드도 같은 비용을 낸다. `World.cpp`·`WorldLevels.cpp`·`Package.cpp`·`HostSettings.cpp`·`TraceController.cpp`·`CurvesModule.cpp`가 집계 헤더를 include한다.

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 생성 헤더 `X.generated.h` | `X.generated.h`(`UhtHeaderCodeGeneratorHFile.cs:228`) | 리플렉션 헤더 X의 타입 선언과 컴파일 타임 형질 |
| 생성 소스 `X.generated.cpp` | `X.gen.cpp`(`UhtHeaderCodeGeneratorCppFile.cs:546`) | 리플렉션 헤더 X의 타입 본문. 헤더마다 하나 |
| 모듈 생성 헤더 `<M>.module.generated.h` | (패키지 생성 함수 선언) | 모듈 등록 함수의 선언 |
| 모듈 생성 소스 `<M>.module.generated.cpp` | `<Module>.init.gen.cpp`(`UhtPackageCodeGeneratorCppFile.cs:372`) | 모듈 등록 함수의 정의. 모듈마다 하나 |
| 등록 함수 `RegisterModuleTypes` | `FRegisterCompiledInInfo` 정적 객체(`UhtHeaderCodeGeneratorCppFile.cs:509`) | 모듈의 타입을 등록기에 넣는다. Rocks는 정적 초기화가 아니라 명시 호출이다 |
| 컴파일 타임 형질 | (`TStructOpsTypeTraits` 등) | 다른 템플릿이 컴파일 중에 읽는 `constexpr` 값 |
| 검사 전용 멤버 | (없음) | `static_assert`만 담고 호출되지 않는 생성 멤버 함수(5.3절) |

## 3. 목표와 범위

목표:
- 생성 헤더를 include하는 비용을 원본 헤더를 include하는 비용에 가깝게 만든다. 본문은 타입마다 한 번만 컴파일한다.
- 동작은 바꾸지 않는다. 스키마, 필드 연산, 등록 순서, descriptor, identity 명세가 그대로다.
- 링크 방식(Monolithic·Modular)에 따라 생성 코드가 달라지지 않는다(BD28). 주소를 식별자로 쓰는 정적 객체(스키마, 모듈 키)는 생성 소스에 한 벌만 둔다(BD29).
- GC 표시 경로의 성능을 바꾸지 않는다.

범위 밖:
- 모듈 API 매크로 주입, `NativeTypeKey` 저장소 교체: BS8(BuildTargets). 이 문서는 그 저장소가 놓일 자리(생성 소스)를 만든다.
- "모듈 자신의 타입만 등록하는 함수"와 실제 `ModuleLease`: ML2(모듈 로더).
- `EditorOnly` 조건 블록: BuildTargets 3.3절. 생성 헤더와 생성 소스에 같은 조건을 쓴다는 것만 맞춘다.
- 생성 소스에 유니티·PCH를 적용하는 일: UnityBuildPch UB1·UB4.
- 생성 코드의 양 자체를 줄이는 일(GSD13, 12절).

## 4. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 본문 위치 | 스키마 표·연산 표·등록·복사·검증 본문은 생성 소스에 둔다. 생성 헤더에는 선언, 컴파일 타임 형질, 작은 inline만 남긴다 | 1.2절: include만으로 16.8초, 선언만 보면 0.6초. UE도 본문을 `.gen.cpp`에 둔다 |
| 생성 소스 단위 | 리플렉션 헤더마다 하나, 그리고 모듈마다 등록용 하나 | 병렬 컴파일과 증분(헤더 하나를 고치면 생성 소스 하나만 다시 컴파일). 모듈 하나로 합치면 ReflectionTestFixtures가 46초짜리 TU 하나가 된다. UE도 헤더마다 `.gen.cpp`, 모듈마다 `.init.gen.cpp`다 |
| inline으로 남기는 함수 | `Id()`, enum `IsDeclared()`, struct `Trace()` | `Id()`·`IsDeclared()`는 `constexpr`로 쓰인다. struct `Trace()`는 GC가 원소마다 직접 부르는 경로라서 LTO 없는 구성에서도 inline이 되어야 한다 |
| 정적 검사 | 멤버 타입 traits를 쓰는 `static_assert`와 `VerifyBinds`는 생성 소스의 검사 전용 멤버로 옮긴다 | 소비 TU마다 traits를 인스턴스화할 이유가 없다. 모듈 빌드에서 한 번 확인하면 된다 |
| 생성 헤더의 include | 원본 헤더, 서명에 필요한 가벼운 헤더, 의존 타입의 생성 헤더만. codec·경로·접근 헤더는 생성 소스만 include한다 | 헤더 파싱 4초 가운데 대부분이 이 헤더들이다 |
| 모듈 생성 헤더 | 등록 함수 선언만 둔다. 타입별 생성 헤더와 의존 모듈 헤더를 include하지 않는다 | 등록만 하는 TU가 모든 타입 헤더를 끌어오지 않게 한다. 소비자는 쓰는 타입의 생성 헤더를 직접 include한다 |
| 검증 컴파일 | 게시 전에는 생성 헤더만 검증한다. 본문은 빌드의 compile action이 검증한다 | 본문 검증이 병렬이 되고 내용 key로 캐시된다. 지금은 모듈이 다시 생성될 때마다 모든 헤더를 순차로 다시 검증한다 |
| 배치 | `<out>/Generated/<M>/Source/` 아래. include 루트가 아니다 | 생성 소스를 헤더 검색에서 뺀다 |
| 소스 없는 리플렉션 모듈 | 생성 소스가 생기므로 정적 라이브러리가 된다 | 본문의 정의가 한 곳에 있어야 한다 |
| 등록 방식 | 지금처럼 명시 호출이다. 정적 초기화로 등록하지 않는다 | 정적 라이브러리에서 참조되지 않은 object가 빠지는 문제와 초기화 순서 문제가 없다. UE는 정적 객체로 등록한다 |

## 5. 생성 코드의 배치

### 5.1 멤버별 위치

`GeneratedType<T>` 특수화의 클래스 정의는 생성 헤더에 있고, 아래 표의 "생성 소스" 멤버는 헤더에 선언만 둔다.

| 멤버 | 대상 | 위치 | 이유 |
|---|---|---|---|
| `ContainsStrongReference`·`RequiresWorldOnlyOwner`·`ContainsReferenceArray`·`ContainsInstancedStruct`·`ContainsRequired`·`Depth` | struct | 헤더(`constexpr`) | 다른 타입의 생성 코드와 런타임 템플릿이 컴파일 중에 읽는다 |
| `ValueCopyable`·`HasValidateTree`·`HasValidateSelf` | struct | 헤더(`constexpr`) | `detail::ValueCopy<S>`·`ValueValidate<S>`가 컴파일 중에 읽는다 |
| `Id()` | 전부 | 헤더(`constexpr` inline) | `TypeIdOf<T>()`가 `constexpr`다 |
| `IsDeclared(int64)` | enum | 헤더(`constexpr` inline) | 쓰기·decode 템플릿이 값마다 부른다 |
| `Trace()` | struct | 헤더(inline) | GC가 배열 원소·중첩 struct마다 직접 부른다(5.2절) |
| `Trace()` | 클래스 | 생성 소스 | GC는 `Ops().trace` 함수 포인터로만 부른다. 그 포인터를 만드는 `Ops()`와 같은 TU에 있으면 된다 |
| `Schema()`·`Settings()` | 전부 | 생성 소스 | 정적 표다. 주소가 하나여야 한다(BD29) |
| `Ops()` | 클래스·struct | 생성 소스 | 필드마다 codec·경로·접근 템플릿을 인스턴스화한다. 비용의 대부분이다 |
| `Register()` | 전부 | 생성 소스 | `Schema()`·`Ops()`를 부르는 전달 함수다 |
| `ReferenceLayout()` | 클래스 | 생성 소스 | 등록할 때 한 번 쓴다 |
| `CopyValue()`·`ValidateSelf()`·`ValidateTree()` | struct | 생성 소스 | 리플렉션 쓰기 경로에서 부른다. 할당을 동반하는 느린 경로다 |
| bitfield `Get_x()`·`Set_x()` | 클래스·struct | 생성 소스(헤더에 선언) | `Ops()` 안에서만 쓴다 |
| `VerifyBinds()`, `static_assert` 묶음 | 전부 | 생성 소스(검사 전용 멤버) | 5.3절 |
| `detail::ValueCopy<S>`·`detail::ValueValidate<S>` 특수화 | struct | 헤더 | 형질 값과, `CopyValue()`·`ValidateTree()`로 넘기는 한 줄 전달 함수다 |
| `RegisterModuleTypes()` | 모듈 | 모듈 생성 소스(모듈 생성 헤더에 선언) | 모든 타입의 `Schema()`·`Ops()`를 부른다. 모듈 키(`moduleKey`)도 여기 있다 |

### 5.2 struct `Trace()`를 inline으로 두는 이유와 조건

- 클래스의 `Trace()`는 GC가 `RuntimeTypeOps::trace` 함수 포인터로 부른다. 그 안에서 중첩 struct와 struct 배열 원소의 `GeneratedType<S>::Trace()`를 직접 부른다(`StructRefArray<S>::Trace`, `detail::TraceArrayField`).
- struct `Trace()`를 생성 소스로 옮기면 struct가 소유 클래스와 다른 헤더에 있을 때 원소마다 TU 경계를 넘는 호출이 된다. Release는 ThinLTO가 다시 inline하지만 Development(`-O2`, LTO 없음)는 그렇지 않다. GC 규모 작업(100만 객체)의 측정 구성을 건드리지 않으려고 inline으로 둔다.
- 참조가 없는 struct의 `Trace()`는 빈 함수라 비용이 없다.
- 조건: 생성 헤더가 codec 헤더를 include하지 않아야 하므로(GSD5), Trace 보조 함수를 가벼운 헤더로 옮긴다. `detail::TraceArrayField`(지금 `Object/ArrayFieldCodec.h:806–822`)와 Map·Optional·InstancedStruct의 대응 함수가 대상이다. 새 헤더는 `Object/GeneratedTrace.h`이고 codec 헤더가 이것을 include한다(GS1, 동작 불변 이동).

### 5.3 정적 검사

- 지금 `GeneratedType<T>` 클래스 정의 안의 `static_assert`는 멤버 타입 traits(`IsNativeStrongReference<decltype(T::m)>`, `detail::RegistryContainer<...>` 등)를 인스턴스화한다. 생성 메타데이터가 현재 헤더와 맞는지 확인하는 검사이므로 모듈을 빌드할 때 한 번이면 된다.
- 생성 소스로 옮기되 private 멤버에 닿아야 하므로 `GeneratedType<T>`의 멤버 함수 안에 둔다. 헤더에 `static void VerifyGenerated() noexcept;`를 선언하고 생성 소스에서 정의한다. 이 함수는 호출하지 않는다. 정의가 컴파일될 때 `static_assert`가 평가된다.
- `VerifyBinds()`(미러 선언 검증)도 같은 방식으로 생성 소스에 둔다.
- 메시지는 지금 문구(영어, RMC 번호 포함)를 그대로 쓴다.

### 5.4 헤더 밖 정의의 규칙

- 정의 형태는 `RuntimeTypeOps GeneratedType<::ns::T>::Ops(ModuleLease module) noexcept { ... }`다. 명시 특수화의 멤버이므로 `template <>`을 붙이지 않는다. 외부 링크이고 프로그램에 정의가 하나다.
- private 멤버 접근은 지금과 같다. `ROCKS_GENERATED_BODY`가 `GeneratedType` 특수화를 friend로 두므로 헤더 밖 멤버 정의와 그 안의 람다도 닿는다.
- 기본 인자(`ModuleLease module = {}`)는 헤더의 선언에 둔다.
- 서명에 쓰는 타입은 선언에서 완전하지 않아도 되는 곳(반환 타입, 참조 인자)은 전방 선언으로 받는다. 기본 인자 때문에 완전한 타입이 필요한 `ModuleLease`는 가벼운 헤더(`Core/Lifetime/ModuleLease.h`)를 include한다.
- 본문이 타입 성질에 따라 갈리는 함수(`CopyValue()`의 `if constexpr`)는 지금 기본 인자를 둔 멤버 템플릿이다. 헤더에는 템플릿이 아닌 `CopyValue()`와 멤버 템플릿 선언만 두고, 생성 소스에서 그 템플릿을 정의하고 한 번 인스턴스화한다.
- 생성 소스는 자기 생성 헤더를 첫 include로 둔다. 그다음에 codec·경로·접근 헤더와, 필드·기반이 쓰는 다른 타입의 생성 헤더를 include한다.
- 생성 소스는 파일 지역 이름(익명 namespace, `static` 함수)과 매크로를 정의하지 않는다. 모든 정의가 한정된 이름의 멤버다. 그래서 유니티 묶음에 넣어도 충돌하지 않는다(GSD9).
- 생성 소스에는 타입과 필드 블록 앞에 원본 선언 위치 주석(`// <철자>:<줄> <타입>::<멤버>`, 저장소 상대 경로)을 남긴다. 컴파일 오류가 난 줄에서 원본 선언을 바로 찾기 위해서다.

### 5.5 모듈 생성 헤더와 소스

- 모듈 생성 헤더는 지금처럼 둘이다. public 쪽은 `RegisterModuleTypes(ModuleRegistration&, const ModuleLease& = {})`, private 쪽(private 리플렉션 헤더가 있는 모듈만)은 `RegisterPrivateModuleTypes`의 선언만 담는다. include는 `Object/ModuleRegistration.h`뿐이다(이 헤더만 include한 TU는 0.6초다 — 1.2절).
- 모듈 생성 소스는 모듈마다 하나다. 이 모듈의 타입별 생성 헤더(public·private)와 의존 모듈의 public 모듈 생성 헤더를 include하고 두 등록 함수를 정의한다. 등록 순서(의존 모듈 먼저, enum → struct → 클래스, 클래스는 base 먼저), private 함수가 public 함수를 먼저 부르는 것, 실패 시 되돌리기는 지금과 같다.
- 모듈 키는 등록 함수 정의 안의 정적 객체다. 정의가 하나이므로 주소도 하나다.
- ML2가 더할 "자기 타입만 등록하는 함수"도 이 헤더에 선언하고 이 소스에 정의한다.
- BS8은 헤더의 선언에 모듈 API 매크로를 붙인다. 생성기는 헤더 밖 멤버의 선언과 등록 함수 선언을 한 함수로 내도록 만들어, BS8이 그 자리에 접두만 더하면 되게 한다.

## 6. 파일 배치와 빌드 연결

### 6.1 배치

```
<out>/Generated/<M>/Public/<철자>.generated.h          (지금과 같다, M의 public include 루트)
<out>/Generated/<M>/Private/<철자>.generated.h         (지금과 같다, M의 private include 루트)
<out>/Generated/<M>/Public/<M>/<M>.module.generated.h  (지금과 같다, 내용만 선언으로)
<out>/Generated/<M>/Source/Public/<철자>.generated.cpp
<out>/Generated/<M>/Source/Private/<철자>.generated.cpp
<out>/Generated/<M>/Source/<M>.module.generated.cpp
```

- `Source/`는 include 루트가 아니다. public 헤더와 private 헤더의 철자가 같을 수 있으므로 `Public/`·`Private/`로 나눈다.
- 타입이 하나도 없는 헤더는 지금처럼 생성물이 없다.

### 6.2 메타 컴파일러

- 생성 헤더와 생성 소스를 함께 낸다. 서식(clang-format), 내용이 같으면 다시 쓰지 않는 게시, stage 뒤 게시하는 트랜잭션, 남은 잉여 파일 검사(`--check`)가 `Source/`도 다룬다.
- descriptor와 identity 명세는 바뀌지 않는다. 이 변경 전후의 descriptor가 바이트까지 같아야 한다(수용 기준).

### 6.3 Rocks.Build

- `MetadataLayout`이 모듈의 `Metadata` 목록에서 생성 소스 경로를 정해 그 모듈의 **리플렉션 생성 소스 목록**(`NativeModuleInput`의 새 필드)에 더한다. 손으로 쓴 `Sources`와 섞지 않는다. 경로는 생성 전에 정해지므로 계획 단계에서 더할 수 있다. 파일은 메타 생성이 끝난 뒤, 액션 그래프를 세우기 전에 존재한다.
- 목록을 따로 두는 이유는 셋이다. 모듈 진입점 판정을 바꾸지 않기 위해서다(아래). 컴파일 실패에 RMC3000 안내를 붙일 대상을 알아야 한다(7절). 유니티에서 생성 소스끼리만 묶을 수 있어야 한다(GSD9).
- 액션 그래프는 이 목록을 `Sources`와 같은 compile action으로 만든다. 모듈의 public·private include와 정의를 그대로 받는다. key는 내용이므로 생성 결과가 같으면 다시 컴파일하지 않는다.
- `.cpp`가 없던 리플렉션 모듈은 정적 라이브러리가 되어 archive와 `ModuleOutputs` 항목이 생긴다. 실행 파일 모듈이 `Metadata`를 가지면 그 실행 파일의 소스로 컴파일한다.
- 모듈 진입점: 모듈 로더는 코드가 있는 모듈에 진입점(`ROCKS_IMPLEMENT_MODULE`)을 요구한다(`Programs/Rocks.Build/ModuleTables.cs`의 `hasCode`). 리플렉션 생성 소스는 이 판정에 넣지 않는다. 그래서 소스 없던 리플렉션 모듈은 라이브러리가 되어도 지금처럼 진입점 면제다. 이 모듈들을 진입점 모듈로 바꾸는 일은 ML2가 정한다.
- 도구 빌드 ID: 생성 소스는 다른 소스와 같이 action 입력이다. 생성 내용에 호스트 경로를 넣지 않는 지금 규칙이 생성 소스에도 적용된다.
- `compile_commands.json`에 생성 소스 항목이 들어간다. 정책 감사는 생성 헤더와 같은 규칙으로 생성 소스를 건너뛴다.
- 설치 SDK는 생성 헤더만 싣는다. 생성 소스의 object는 SDK 라이브러리에 들어 있다. `ImportedMetadata`(descriptor·identity 명세)는 그대로다. 소스가 없던 리플렉션 모듈이 SDK에 들어가면 이제 라이브러리도 함께 싣는다.

## 7. 검증 컴파일

| 대상 | 지금 | 이후 |
|---|---|---|
| 생성 헤더 | 게시 전, 헤더마다 `-fsyntax-only`, 순차 | 그대로. 헤더가 가벼워져 빨라진다 |
| 본문(생성 소스) | 생성 헤더 검증에 포함 | 빌드의 compile action. 병렬이고 내용 key로 캐시된다 |

- 게시 전 검증은 선언, 형질, inline `Trace()`의 접근·`noexcept` 계약을 확인한다.
- 본문의 계약 위반(생성자·소멸자 `noexcept`, 접근 권한, `ROCKS_GENERATED_BODY` 누락 등)은 생성 소스를 컴파일할 때 드러난다. Rocks.Build는 생성 소스 compile action이 실패하면 컴파일러 출력 뒤에 RMC3000 안내문을 붙인다(지금 문구 그대로).
- 달라지는 점: 지금은 "모든 생성 연산이 컴파일되어야 출력을 받아들인다". 이후에는 헤더까지만 게시 전에 확인하고, 본문이 컴파일되지 않는 출력도 게시된다. 받아들일 수 있는 이유는 다음과 같다.
  - 생성물은 입력의 결정적 함수다. 본문이 컴파일되지 않으면 빌드가 실패하고, 입력을 고치면 다시 생성된다. 낡은 산출물을 성공으로 재사용하는 경로가 없다.
  - identity 명세에 새 ID가 먼저 기록될 수 있다. ID는 이름에 묶인 것이라 타입을 고친 뒤에도 그대로 유효하다.

## 8. 소비자 변경

- 모듈 생성 헤더가 타입별 생성 헤더를 include하지 않게 되므로, `GeneratedType<T>`의 멤버를 쓰는 코드는 그 타입의 생성 헤더를 직접 include한다. 2026-11-19 기준 손으로 쓴 파일 43개가 집계 헤더를, 11개가 타입별 생성 헤더를 include한다.
- 등록만 하는 코드는 모듈 생성 헤더만 include한다.
- 손으로 쓴 헤더(공개·비공개 모두)는 모듈 생성 헤더를 include하지 않는다. 등록은 `.cpp`에서 한다. 정책 검사로 막는다(GS3).
- `GeneratedType<T>::Schema()`·`Ops()`·`Register()`·`CopyValue()` 등의 호출 문법은 그대로다. 링크할 때 소유 모듈의 라이브러리가 필요하다(의존 선언이 이미 있어야 하는 관계다).

## 9. 측정과 수용 기준

GS0에서 기준을 재고 GS4에서 같은 장비·같은 commit 기반으로 다시 잰다. 다른 빌드가 돌지 않는 상태에서 잰다.

| 항목 | 기준(1.2절) | 목표 |
|---|---|---|
| 모듈의 모든 생성 헤더 include만 하는 TU(ReflectionTestFixtures) | 16.8 s | 원본 헤더만 include한 TU 대비 +1.5 s 이내 |
| 선언으로 등록 함수를 부르는 TU | (지금은 46 s) | 1 s 이내 |
| `TextAssignAppendTests.cpp` | 48 s(GS0 재측정 37.2 s). GS1 뒤 2.4 s | 5 s 이내 |
| FoundationTests clean compile의 action 시간 합 | GS0 1,541 s(386 TU). GS1 뒤 495 s(389 TU), 따로 모듈 생성 소스 7개 54 s(ReflectionTestFixtures 35.8 s) | 절반 이하 |
| FoundationTests object 디렉터리 크기, link 시간 | GS0 1,899 MB, 4.6 s. GS1 뒤 266 MB(따로 모듈 생성 소스 object 67 MB), 2.4 s | 줄어든다(기록) |
| ReflectionTestFixtures 메타 생성 시간(검증 포함) | Foundation 타깃 메타 단계 전체(모듈 7개)로 쟀다: GS0 107 s. GS1 뒤 86 s | 줄어든다(기록) |

GS0·GS1 측정 조건(2026-11-20): 같은 장비에서 두 commit을 각각 내보낸 사본으로 clean Foundation Debug 빌드(`--jobs 8`)를 연달아 하고 `action-report.json`의 action별 소요 시간(GSD12)을 더했다. 다른 세션의 빌드가 거의 없는 상태였다(GS1 빌드가 끝날 무렵 다른 빌드가 시작됐다). 가장 느린 action은 GS0에서 집계 헤더로 등록하던 시험 TU들(38–42 s)이었고, GS1 뒤에는 ReflectionTestFixtures 모듈 생성 소스(35.8 s) 하나와 그다음 8.6 s 이하의 TU들이다. Foundation 타깃 전체 빌드(메타 단계 포함)는 389 s에서 255 s가 됐다. descriptor는 두 commit에서 바이트까지 같다.

동작 기준:
- 전체 시험(Debug)이 통과한다. descriptor와 identity 명세가 변경 전후 바이트까지 같다.
- no-op 빌드는 0 action이다. 리플렉션 헤더 하나를 고치면 생성 소스는 그 헤더 것만 다시 컴파일된다(내용 key).
- GC 표시 벤치마크가 Development·Release에서 나빠지지 않는다(struct `Trace()` inline 유지 확인).
- 생성 헤더 단독 컴파일 검사와 공개 헤더 검사가 통과한다.

## 10. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| GSD1 | 생성 헤더에는 선언·컴파일 타임 형질·작은 inline만 두고 본문은 생성 소스에 둔다 | 확정 |
| GSD2 | 생성 소스는 리플렉션 헤더마다 하나, 모듈마다 등록용 하나다 | 확정 |
| GSD3 | inline으로 남기는 함수는 `Id()`, enum `IsDeclared()`, struct `Trace()`다. 클래스 `Trace()`와 나머지는 생성 소스다(5.1절 표) | 확정 |
| GSD4 | `static_assert` 묶음과 `VerifyBinds()`는 생성 소스의 검사 전용 멤버 함수에 둔다 | 확정 |
| GSD5 | 생성 헤더는 codec·경로·접근 헤더를 include하지 않는다. Trace 보조 함수는 `Object/GeneratedTrace.h`로 옮긴다 | 확정 |
| GSD6 | 모듈 생성 헤더는 등록 함수 선언만 담는다. 소비자는 쓰는 타입의 생성 헤더를 직접 include하고, 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다 | 확정 |
| GSD7 | 생성 소스는 `<out>/Generated/<M>/Source/` 아래에 두고 `MetadataLayout`이 모듈 `Sources`에 더한다. 소스 없던 리플렉션 모듈은 정적 라이브러리가 된다 | 확정 |
| GSD8 | 게시 전 검증 컴파일은 생성 헤더만 한다. 본문은 빌드의 compile action이 검증하고, 실패하면 RMC3000 안내를 붙인다 | 확정 |
| GSD9 | 생성 소스는 파일 지역 이름·매크로 없이 낸다. 유니티로 묶을지는 UnityBuildPch UB1에서 정한다 | 확정 |
| GSD10 | 링크 방식과 무관하게 같은 구조다(BD28). 모듈 API 매크로와 `NativeTypeKey` 교체는 BS8, 자기 타입만 등록하는 함수는 ML2가 생성 소스에 더한다 | 확정 |
| GSD11 | 설치 SDK는 생성 헤더만 싣는다 | 확정 |
| GSD12 | action마다 소요 시간을 `action-report.json`에 기록한다(측정 기반) | 확정 |
| GSD13 | 생성 코드의 양을 줄이는 일(필드마다 thunk를 만드는 대신 값 종류와 offset으로 도는 공용 codec)은 별도 과제로 둔다(12절) | 보류 |
| GSD14 | 등록 함수 분리(GS1)를 타입별 본문 분리(GS3)보다 먼저 한다. 리플렉션 생성 소스는 `Sources`와 다른 목록으로 나르고 모듈 진입점 판정에 넣지 않는다(진입점 모듈로 바꾸는 일은 ML2) | 확정 |

## 11. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험으로 검증한다.

GS1을 먼저 하는 이유: 등록 함수만 생성 소스로 빼도, 집계 헤더로 등록하던 TU가 46초에서 6.5초로 줄어든다(1.2절). 바꾸는 범위는 집계 생성부와 빌드 연결뿐이라 작다.

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| GS0 | `action-report.json`에 action별 소요 시간 기록(GSD12). 9절 기준 측정 | 기준 수치 기록 |
| GS1 | **등록 함수 분리.** 모듈 생성 헤더를 선언만 남기고 모듈 생성 소스를 낸다(5.5절). `Source/` 루트를 게시 트랜잭션·`--check`·잉여 파일 검사에 더한다. `MetadataLayout`이 리플렉션 생성 소스 목록에 더하고 액션 그래프가 컴파일한다(6.3절, 진입점 판정 제외). 모듈 생성 소스는 게시 전 검증에서 빼고 빌드가 컴파일한다. 소비자 include를 고치고(8절), 손으로 쓴 헤더의 모듈 생성 헤더 include를 막는 정책 검사를 넣는다. 설치 SDK가 소스 없던 리플렉션 모듈의 라이브러리를 싣는다. 타입별 생성 헤더는 그대로 둔다 | 전체 시험(Debug)·설치 SDK 게임 빌드 통과, descriptor·identity 명세 바이트 동일, 메타 컴파일러 시험(생성물 대조) 갱신, 소스 없던 리플렉션 모듈의 archive 생성, 정책 검사 위반·통과 fixture, 집계 헤더로 등록하던 시험 TU의 시간 기록 |
| GS2 | 런타임 준비(동작 불변 이동): Trace 보조 함수를 `Object/GeneratedTrace.h`로 분리, 생성 헤더 서명에 필요한 전방 선언 정리 | 기존 생성 헤더 그대로 전체 시험 통과 |
| GS3 | **타입별 본문 분리.** 메타 컴파일러가 타입별 생성 헤더·생성 소스를 나눠 낸다(5.1–5.4절). 게시 전 검증 컴파일을 생성 헤더로 줄이고 RMC3000 안내를 빌드에 붙인다(7절) | 전체 시험 통과, descriptor·identity 명세 바이트 동일, 메타 컴파일러 시험 갱신, 본문 계약 위반 fixture가 RMC3000 안내와 함께 빌드 실패 |
| GS4 | 설치 SDK가 생성 헤더만 싣는지 확인(GSD11), 남은 소비자 정리 | 설치 SDK 게임 빌드 통과 |
| GS5 | 9절 재측정, 문서 개정(13절) | 9절 목표와 동작 기준 |

## 12. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 등록 | `.gen.cpp`의 정적 객체가 프로그램 시작 때 등록한다 | 등록 함수를 명시 호출한다(모듈 로더가 부른다) | 정적 라이브러리에서 object가 빠지는 문제와 초기화 순서 의존이 없다 |
| 생성 헤더의 형태 | `GENERATED_BODY()`가 펼치는 매크로 묶음. 원본 헤더가 생성 헤더를 include한다 | `GeneratedType<T>` 특수화. 생성 헤더가 원본 헤더를 include한다 | 기존 구조를 유지한다 |
| 필드 접근 | 필드 기술 데이터(offset·종류)를 공용 코드가 해석한다. 필드마다 코드를 만들지 않는다 | 필드마다 타입 있는 thunk를 만든다 | 기존 구조다. 생성 코드가 큰 원인이며(fixture 모듈 object 54 MB), 양을 줄이는 것은 GSD13으로 남긴다 |
| 타입 정보 export | 클래스를 export하지 않으면 `MinimalAPI`가 필요하다 | 항상 export한다(BD28) | BuildTargets 결정 |

GSD13 메모: 본문을 한 번만 컴파일하게 하면 중복은 없어지지만 생성 코드의 양은 그대로다. fixture 모듈 하나가 인스턴스화 11초, 코드 생성 21초, object 54 MB다. 엔진 타입이 늘면 이 비용이 모듈마다 생긴다. 값 종류와 offset으로 도는 공용 codec으로 필드별 thunk를 줄이는 방법을 GS4 측정 뒤에 따로 검토한다.

## 13. 다른 문서 개정

이번 개정에서 고친 것:
- [빌드 타깃](BuildTargets.ko.md) BD28: 생성 코드 분리의 구현 스펙이 이 문서이고, 생성 소스 단위가 "헤더마다 하나 + 모듈마다 하나"임을 적었다.
- [유니티 빌드·PCH](UnityBuildPch.ko.md) 1절 "생성 TU" 행: 리플렉션 생성 소스가 생긴다는 것과 이 문서 링크를 적었다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절: 이 문서 링크를 적었다.

구현할 때 고칠 것:
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절(배치에 `Source/` 추가), 1.2절(검증 컴파일 — 7절), 집계 헤더 서술(모듈 생성 헤더는 선언만), struct·enum 절의 "생성 헤더가 … 낸다" 서술: GS2·GS3.
- [유니티 빌드·PCH](UnityBuildPch.ko.md) UD5·16절 "생성 코드 묶기": 리플렉션 생성 소스를 자기들끼리 묶을지 UB1에서 정해 반영한다.
- [모듈 로더](ModuleLoader.ko.md) ML2: 생성 함수의 정의 위치가 모듈 생성 소스라는 것.
- `Programs/Rocks.Build/README.ko.md`: 생성 소스의 `Sources` 편입, 소스 없던 리플렉션 모듈의 archive, action 소요 시간 기록.
- `Programs/Rocks.MetaCompiler` 문서·도움말: 출력 목록과 검증 범위.

## 14. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-19 | 생성 코드 분리 설계. 측정(1.2절), 멤버별 위치(5.1절), 배치와 빌드 연결(6절), 검증 컴파일 변경(7절), 결정 GSD1–GSD14(GSD13 보류), 단계 GS0–GS5. 등록 함수 분리(GS1)를 먼저 하기로 하고 착수했다(사용자 결정). 모듈 로더 ML2와 조율해 리플렉션 생성 소스를 별도 목록으로 나르고 진입점 판정에서 뺐다 |
| 1(구현 기록) | 2026-11-20 | GS0·GS1 구현과 측정(9절). 스펙에 없던 것 둘을 구현하며 정했다. 첫째, 설치 SDK가 실어 가는 "리플렉션 생성 소스만으로 된 라이브러리"에 표지 `ReflectionOnlyLibrary`를 붙였다(SDK manifest 7) — 게임 빌드의 모듈 진입점 판정은 미리 빌드한 라이브러리를 코드로 세므로, 표지가 없으면 CoreTypeBindings 같은 모듈이 SDK를 거치며 진입점 모듈이 된다(6.3절의 면제를 SDK에서도 유지). 둘째, MetaCompiler 옵션 `--verify-sources`(빌드 없이 쓰는 단독 실행·도구 시험이 모듈 생성 소스도 게시 전에 구문 검사한다 — 빌드는 넘기지 않는다) |
