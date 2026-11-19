# 리플렉션 생성 소스 설계

기준일: 2026-11-19, 개정 2. 메타 컴파일러가 내는 리플렉션 코드를 둘로 나눈다.
- **생성 헤더**(`X.generated.h`): 선언, 컴파일 타임 형질, 작은 inline 함수만 둔다.
- **생성 소스**(`X.generated.cpp`): 스키마 표, 필드 연산 표, 등록 함수 같은 본문을 둔다. 모듈이 한 번만 컴파일한다.

지금은 본문 전체가 생성 헤더에 inline으로 들어 있어, 그 헤더를 include하는 번역 단위마다 같은 코드를 다시 인스턴스화하고 코드 생성한다. 시험 빌드 시간의 가장 큰 원인이다(1절).

다른 문서와의 관계:
- [빌드 타깃](BuildTargets.ko.md) 3.8절 BD28이 모듈식 링크를 위해 "생성 `.cpp`를 낸다"고 이미 정했다(BS8 단계, 미구현). 이 문서는 그 가운데 **생성 코드를 헤더에서 소스로 옮기는 부분**의 구현 스펙이고, 컴파일 시간 때문에 BS8보다 먼저 한다. 모듈 API 매크로 주입과 `NativeTypeKey` 교체는 BS8에 그대로 둔다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절(생성물 배치)·1.2절(검증 컴파일)과 등록 절의 집계 헤더 서술을 바꾼다(13절).
- [모듈 로더](ModuleLoader.ko.md) ML2(개정 3 작업 중)는 빌드가 만드는 TU `<M>.ModuleTypes.cpp`에 "모듈 자신의 타입만 등록하는 함수"를 두려 했다. 이 문서의 모듈 생성 소스가 그 자리를 대신한다. ML2는 GS1 병합 뒤에 그 함수와 진입점 연결을 모듈 생성 소스 구조 위에 더한다(모듈 로더 세션과 2026-11-19 조율).
- [유니티 빌드·PCH](UnityBuildPch.ko.md)는 "리플렉션 생성물은 헤더뿐"을 전제로 썼다. 생성 소스가 생기므로 그 전제를 고친다(13절).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 UnrealHeaderTool이다. 경로는 `Engine/Source/Programs/Shared/EpicGames.UHT/Exporters/CodeGen/` 기준이다. UE 이름은 대응표와 근거에만 적는다. 측정은 Rocks 툴체인(clang-cl 21.1.6, foundation-debug)으로 했다.

상태: **설계 확정**(결정 GSD1–GSD14, 2026-11-19 사용자 승인 — 권장안, GSD5·GSD13은 보류). 리뷰어 교차 검증을 반영했다(개정 2). 리뷰어가 기존 생성 헤더 35개를 5절 방식으로 나눠 전부 컴파일해 구조가 성립함을 확인했다. 구현 단계는 GS0–GS3(11절)이다. GS1(등록 함수 분리)은 2026-11-19 사용자 결정으로 착수했다. 다른 세션은 GS1 병합까지 전체 빌드·시험 반복을 보류한다. GS0(action 소요 시간 기록·기준 측정)과 GS1은 2026-11-20 구현해 engine-runtime에 병합했다(측정값은 9절). GS2(타입별 본문 분리)도 2026-11-20 구현했다(구현하며 정한 점은 개정 이력). 이후 단계의 기준·전후 측정용 빌드는 따로 돌리지 않는다(2026-11-20 사용자 결정) — 숫자는 일반 검증 빌드의 action 소요 시간(GSD12)으로 남긴다.

## 1. 현재 상태와 측정

### 1.1 구조

| 항목 | 현재 | 근거 |
|---|---|---|
| 타입별 생성 헤더 | `GeneratedType<T>` 특수화 하나에 형질, `Id()`, `Schema()`, `Trace()`, `Ops()`, `Register()` 등이 모두 inline 본문으로 들어 있다 | `Programs/Rocks.MetaCompiler/CppEmitter.cs:223`(클래스), `:625`(struct), `:952`(enum) |
| 필드 연산 | `Ops()`가 필드마다 읽기·쓰기·staged 쓰기·encode·decode·경로 thunk를 람다로 만들고, 각 람다가 `detail::ReadGeneratedField<T>` 같은 템플릿을 인스턴스화한다 | `CppEmitter.cs:305–396`, `:727–769` |
| 모듈 집계 헤더 | public 쪽 `<M>/<M>.module.generated.h`와 private 쪽 `<M>/<M>.private.module.generated.h`가 모듈의 생성 헤더와 의존 모듈의 집계 헤더를 include하고, inline `RegisterModuleTypes`·`RegisterPrivateModuleTypes`가 모든 타입의 `Schema()`·`Ops()`를 부른다 | `CppEmitter.cs:71–91`, `CompilerOptions.cs:36–37` |
| 생성 헤더의 include | 원본 헤더와 codec·경로·접근 헤더(`Object/GeneratedFieldCodec.h`·`GeneratedPathAccess.h`·`GeneratedFieldAccess.h` 등) | `CppEmitter.cs:149–201` |
| 검증 컴파일 | 메타 컴파일러가 게시 전에 생성 헤더마다 `-fsyntax-only` 컴파일을 **순차로** 한다(헤더당 timeout 2분). 모듈이 다시 생성될 때마다 모든 헤더를 다시 검증한다 | `Programs/Rocks.MetaCompiler/CompilerDriver.cs:75, 241–293` |
| 생성 소스 | 없다. 생성물은 헤더와 descriptor뿐이다 | `Programs/Rocks.MetaCompiler/CompilerOptions.cs:33` |
| 소스 없는 리플렉션 모듈 | ReflectionTestFixtures·CoreTypeBindings·ConfigTestFixtures·CurvesTestFixtures·AssetTestFixtures·WorldTestFixtures는 `.cpp`가 없어 header-only 모듈이다(archive 없음) | `Programs/Rocks.Build/README.ko.md` "현재 지원 범위" |
| 증분 key | compile key가 include 디렉터리의 모든 헤더 내용과 모듈 전체 소스 목록을 담는다 | `Programs/Rocks.Build/ActionGraph.cs:183–185, 239` |

### 1.2 측정(2026-11-19)

조건: foundation-debug의 compile 명령 그대로, clang `-ftime-trace`. 첫 측정은 다른 세션의 빌드가 돌던 중이었고, 리뷰어가 조용한 상태에서 다시 쟀다. 두 값이 다르면 조용한 상태의 값을 괄호에 적는다.

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
| 등록 함수를 **선언만 보고** 호출 | 0.6 s (0.4 s) | 0.3 s | 0.0 s | 0.0 s | 11 KB |
| ReflectionTestFixtures 집계 헤더 include만(호출 없음) | 16.8 s (17.6 s) | 4.3 s | 12.3 s | 0.1 s | 6 KB |
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

**헤더 파싱 시간의 구성**(조용한 상태, 리뷰어 측정):

| TU 내용 | 파싱 |
|---|---|
| 원본 fixture 헤더만 | 0.83 s |
| 위 + codec·접근·경로 헤더 | 1.16 s |
| 지금의 집계 헤더 | 3.65 s |

**나눈 결과의 시제품**(리뷰어가 기존 생성 헤더 35개를 5절 방식으로 기계적으로 나눈 것, include 목록은 그대로):

| 항목 | 지금 | 나눈 뒤 |
|---|---|---|
| 모듈의 모든 생성 헤더 include만 | 17.6 s | 1.6 s(원본 헤더만 include하면 1.1 s) |
| `TextAssignAppendTests.cpp` | 48 s | 2.4 s |
| `AtomicStructTests.cpp` | 19 s | 1.3 s |

해석:
- include만 해도 본문 안의 템플릿이 인스턴스화된다. inline 멤버 함수의 본문은 쓰이지 않아도 파싱되고, 본문이 참조한 함수 템플릿은 TU 끝에서 인스턴스화되기 때문이다. 코드 생성은 그 함수가 실제로 쓰일 때만 일어난다.
- 비용은 include하는 헤더 수가 아니라 **생성 본문** 자체에서 나온다. codec·접근·경로 헤더의 파싱은 0.3초 남짓이고, 나머지 파싱 2.5초와 인스턴스화 12초가 본문 몫이다.
- ReflectionTestFixtures 집계 헤더로 등록하는 TU 28개가 각각 약 46초, object 54 MB를 만든다. 합치면 약 21 CPU-분, object 1.5 GB이고 링커가 같은 COMDAT을 28번 받아 하나로 줄인다.
- 엔진 코드도 같은 비용을 낸다. `World.cpp`·`WorldLevels.cpp`·`Package.cpp`·`HostSettings.cpp`·`TraceController.cpp`·`CurvesModule.cpp`가 집계 헤더를 include한다.

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 생성 헤더 `X.generated.h` | `X.generated.h`(`UhtHeaderCodeGeneratorHFile.cs:228`) | 리플렉션 헤더 X의 타입 선언과 컴파일 타임 형질 |
| 생성 소스 `X.generated.cpp` | `X.gen.cpp`(`UhtHeaderCodeGeneratorCppFile.cs:546`) | 리플렉션 헤더 X의 타입 본문. 헤더마다 하나 |
| 모듈 생성 헤더 `<M>.module.generated.h`·`<M>.private.module.generated.h` | (패키지 생성 함수 선언) | 모듈 등록 함수의 선언 |
| 모듈 생성 소스 `<M>.module.generated.cpp` | `<Module>.init.gen.cpp`(`UhtPackageCodeGeneratorCppFile.cs:372`) | 모듈 등록 함수의 정의. 모듈마다 하나 |
| 등록 함수 `RegisterModuleTypes`·`RegisterPrivateModuleTypes` | `FRegisterCompiledInInfo` 정적 객체(`UhtHeaderCodeGeneratorCppFile.cs:509`) | 모듈의 타입을 등록기에 넣는다. Rocks는 정적 초기화가 아니라 명시 호출이다 |
| 컴파일 타임 형질 | (`TStructOpsTypeTraits` 등) | 다른 템플릿이 컴파일 중에 읽는 `constexpr` 값 |
| 검사 전용 멤버 | (없음) | `static_assert`만 담고 호출되지 않는 생성 멤버 함수(5.3절) |
| 리플렉션 생성 소스 목록 | (없음) | 빌드가 생성 소스를 손으로 쓴 소스와 따로 나르는 목록(6.3절) |

## 3. 목표와 범위

목표:
- 생성 헤더를 include하는 비용을 원본 헤더를 include하는 비용에 가깝게 만든다. 본문은 타입마다 한 번만 컴파일한다.
- 동작은 바꾸지 않는다. 스키마, 필드 연산, 등록 순서, descriptor가 그대로다.
- 링크 방식(Monolithic·Modular)에 따라 생성 코드가 달라지지 않는다(BD28). 주소를 식별자로 쓰는 정적 객체(스키마, 모듈 키)는 생성 소스에 한 벌만 둔다(BD29).
- GC 표시 경로의 성능을 바꾸지 않는다.
- 소비 코드를 고치는 범위를 줄인다. 타입별 본문 분리(GS2)는 소비 코드를 고치지 않고 끝낸다.

범위 밖:
- 모듈 API 매크로 주입, `NativeTypeKey` 저장소 교체: BS8(BuildTargets). 이 문서는 그 저장소가 놓일 자리(생성 소스)를 만든다.
- "모듈 자신의 타입만 등록하는 함수", 실제 `ModuleLease`, 소스 없던 리플렉션 모듈을 진입점 모듈로 바꾸는 일: ML2(모듈 로더).
- `EditorOnly` 조건 블록: BuildTargets 3.4절. 생성 헤더와 생성 소스에 같은 조건을 쓴다는 것만 맞춘다.
- 생성 소스에 유니티·PCH를 적용하는 일: UnityBuildPch UB1·UB4.
- 생성 헤더의 include를 줄이는 일(GSD5)과 생성 코드의 양 자체를 줄이는 일(GSD13): 보류(12절).

## 4. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 본문 위치 | 스키마 표·연산 표·등록·복사·검증 본문은 생성 소스에 둔다. 생성 헤더에는 선언, 컴파일 타임 형질, 작은 inline만 남긴다 | 1.2절: include만으로 17초, 나눈 시제품은 1.6초. UE도 본문을 `.gen.cpp`에 둔다 |
| 생성 소스 단위 | 리플렉션 헤더마다 하나, 그리고 모듈마다 등록용 하나 | 병렬로 컴파일된다. 모듈 하나로 합치면 ReflectionTestFixtures가 46초짜리 TU 하나가 된다. UE도 헤더마다 `.gen.cpp`, 모듈마다 `.init.gen.cpp`다. 헤더 하나를 고쳤을 때 생성 소스 하나만 다시 컴파일되는 증분 이득은 UnityBuildPch UB0(정밀 헤더 의존) 뒤에 생긴다 |
| inline으로 남기는 함수 | `Id()`, enum `IsDeclared()`, `Trace()`(struct·클래스) | `Id()`·`IsDeclared()`는 `constexpr`로 쓰인다. `Trace()`는 GC가 직접 부르는 경로라서 LTO 없는 구성에서도 inline이 되어야 한다(5.2절) |
| 정적 검사 | 멤버 타입 traits를 쓰는 `static_assert`와 `VerifyBinds`는 생성 소스의 검사 전용 멤버로 옮긴다 | 소비 TU마다 traits를 인스턴스화할 이유가 없다. 모듈 빌드에서 한 번 확인하면 된다 |
| 생성 헤더의 include | 지금 목록을 그대로 둔다(codec·경로·접근 헤더 포함). 줄이는 일은 보류한다 | 줄여도 TU당 0.3–0.5초뿐이다(1.2절). 줄이면 `Object/ValueCopy.h`·Trace 보조 함수를 가벼운 헤더로 나눠야 하고, 생성 헤더가 끌어오던 include에 기대던 소비 TU가 깨진다(시제품에서 95개 중 17개) |
| 모듈 생성 헤더 | 등록 함수 선언만 둔다. 타입별 생성 헤더와 의존 모듈 헤더를 include하지 않는다 | 등록만 하는 TU가 모든 타입 헤더를 끌어오지 않게 한다. 소비자는 쓰는 타입의 생성 헤더를 직접 include한다 |
| 검증 컴파일 | 빌드가 부르는 메타 생성은 게시 전에 생성 헤더만 검증하고, 본문은 빌드의 compile action이 검증한다. 메타 컴파일러에는 생성 소스까지 검증하는 선택 옵션을 둔다 | 본문 검증이 병렬이 되고 내용 key로 캐시된다. 메타 컴파일러 단독 실행·`--check`·도구 시험은 옵션으로 지금과 같은 거부 보장을 유지한다(7절) |
| 배치 | `<out>/Generated/<M>/Source/` 아래. include 루트가 아니다 | 생성 소스를 헤더 검색에서 뺀다 |
| 빌드 연결 | 리플렉션 생성 소스는 손으로 쓴 `Sources`와 다른 목록으로 나른다. 소스 없던 리플렉션 모듈은 정적 라이브러리가 되지만 모듈 진입점 판정은 바뀌지 않는다 | 진입점 모듈 전환은 ML2의 결정이다(6.3절) |
| 등록 방식 | 지금처럼 명시 호출이다. 정적 초기화로 등록하지 않는다 | 정적 라이브러리에서 참조되지 않은 object가 빠지는 문제와 초기화 순서 문제가 없다. UE는 정적 객체로 등록한다 |
| 순서 | 등록 함수 분리(GS1)를 타입별 본문 분리(GS2)보다 먼저 한다 | 등록 함수만 빼도 집계 헤더로 등록하던 TU가 46초에서 6.5초로 준다. 바꾸는 범위가 집계 생성부와 빌드 연결뿐이다 |

## 5. 생성 코드의 배치

### 5.1 멤버별 위치

`GeneratedType<T>` 특수화의 클래스 정의는 생성 헤더에 있고, 아래 표의 "생성 소스" 멤버는 헤더에 선언만 둔다.

| 멤버 | 대상 | 위치 | 이유 |
|---|---|---|---|
| `ContainsStrongReference`·`RequiresWorldOnlyOwner`·`ContainsReferenceArray`·`ContainsInstancedStruct`·`ContainsRequired`·`Depth` | struct | 헤더(`constexpr`) | 다른 타입의 생성 코드와 런타임 템플릿이 컴파일 중에 읽는다 |
| `ValueCopyable`·`HasValidateTree`·`HasValidateSelf` | struct | 헤더(`constexpr`) | `detail::ValueCopy<S>`·`ValueValidate<S>`가 컴파일 중에 읽는다 |
| `Id()` | 전부 | 헤더(`constexpr` inline) | `TypeIdOf<T>()`가 `constexpr`다 |
| `IsDeclared(int64)` | enum | 헤더(`constexpr` inline) | 쓰기·decode 템플릿이 값마다 부른다 |
| `Trace()` | struct·클래스 | 헤더(inline) | GC가 직접 부른다(5.2절) |
| `Schema()`·`Settings()` | 전부 | 생성 소스 | 정적 표다. 주소가 하나여야 한다(BD29) |
| `Ops()` | 클래스·struct | 생성 소스 | 필드마다 codec·경로·접근 템플릿을 인스턴스화한다. 비용의 대부분이다 |
| `Register()` | 전부 | 생성 소스 | `Schema()`·`Ops()`를 부르는 전달 함수다 |
| `ReferenceLayout()` | 클래스 | 생성 소스 | 등록할 때 한 번 쓴다 |
| `CopyValue()`·`ValidateSelf()`·`ValidateTree()` | struct | 생성 소스 | 리플렉션 쓰기 경로에서 부른다. 할당을 동반하는 느린 경로다 |
| bitfield `Get_x()`·`Set_x()` | 클래스·struct | 생성 소스(헤더에 선언) | `Ops()` 안에서만 쓴다 |
| `VerifyBinds()`, `static_assert` 묶음 | 전부 | 생성 소스(검사 전용 멤버) | 5.3절 |
| `detail::ValueCopy<S>`·`detail::ValueValidate<S>` 특수화 | struct | 헤더 | 형질 값과, `CopyValue()`·`ValidateTree()`로 넘기는 한 줄 전달 함수다 |
| `RegisterModuleTypes()`·`RegisterPrivateModuleTypes()` | 모듈 | 모듈 생성 소스(모듈 생성 헤더에 선언) | 모든 타입의 `Schema()`·`Ops()`를 부른다. 모듈 키(`moduleKey`)도 여기 있다 |

다른 세션이 더하는 생성 멤버도 같은 기준을 따른다. 컴파일 중에 쓰이거나 호출마다 inline이 되어야 하는 것(예: 로깅 LG16의 `constexpr` 필드 방문 `VisitFields`)은 헤더에, 표와 thunk는 생성 소스에 둔다.

### 5.2 `Trace()`를 inline으로 두는 이유

- struct의 `Trace()`는 소유 클래스의 `Trace()`와 배열 보조 함수(`StructRefArray<S>::Trace`, `detail::TraceArrayField`·`TraceMapField`)가 원소마다 직접 부른다.
- 클래스의 `Trace()`는 `RuntimeTypeOps::trace` 함수 포인터로 불리는 경로 말고도, native 파생 클래스의 GC 경로(`TraceNativeChain`, `Object/NativeClass.h:198–207`)가 직접 부른다.
- 생성 소스로 옮기면 이 호출들이 TU 경계를 넘는다. Release는 ThinLTO가 다시 inline하지만 Development(`-O2`, LTO 없음)는 그렇지 않다. GC 규모 작업(100만 객체)의 측정 구성을 건드리지 않으려고 inline으로 둔다.
- `Trace()` 본문은 작다(참조 필드마다 방문 호출 하나). 참조가 없는 타입은 빈 함수다. 생성 헤더의 include를 그대로 두므로(GSD5 보류) Trace 보조 함수를 옮길 필요가 없다.

### 5.3 정적 검사

- 지금 `GeneratedType<T>` 클래스 정의 안의 `static_assert`는 멤버 타입 traits(`IsNativeStrongReference<decltype(T::m)>`, `detail::RegistryContainer<...>` 등)를 인스턴스화한다. 생성 메타데이터가 현재 헤더와 맞는지 확인하는 검사이므로 모듈을 빌드할 때 한 번이면 된다.
- 생성 소스로 옮기되 private 멤버에 닿아야 하므로 `GeneratedType<T>`의 멤버 함수 안에 둔다. 헤더에 `static void VerifyGenerated() noexcept;`를 선언하고 생성 소스에서 정의한다. 이 함수는 호출하지 않는다. 정의가 컴파일될 때 `static_assert`가 평가된다(리뷰어가 반례로 확인).
- `VerifyBinds()`(미러 선언 검증)도 같은 방식으로 생성 소스에 둔다.
- 메시지는 지금 문구(영어, RMC 번호 포함)를 그대로 쓴다.

### 5.4 헤더 밖 정의의 규칙

- 정의 형태는 `RuntimeTypeOps GeneratedType<::ns::T>::Ops(ModuleLease module) noexcept { ... }`다. 명시 특수화의 멤버이므로 `template <>`을 붙이지 않는다. 외부 링크이고 프로그램에 정의가 하나다.
- private 멤버 접근은 지금과 같다. `ROCKS_GENERATED_BODY`가 `GeneratedType` 특수화를 friend로 두므로 헤더 밖 멤버 정의와 그 안의 람다도 닿는다.
- 기본 인자(`ModuleLease module = {}`)는 헤더의 선언에 둔다.
- 헤더에 선언한 멤버의 반환 타입과 인자 타입은 그 헤더에서 **완전한 타입**이어야 한다. 전방 선언만 두면 `requires { GeneratedType<T>::Ops(); }` 같은 탐지식(`Config/ConfigContext.h:104–107`)이 조용히 거짓이 된다. 지금 include 목록을 유지하므로 이 조건은 이미 성립한다.
- 본문이 타입 성질에 따라 갈리는 함수(`CopyValue()`의 `if constexpr`)는 지금 기본 인자를 둔 멤버 템플릿이다. 헤더에는 템플릿이 아닌 `CopyValue()`와 **다른 이름**의 멤버 템플릿 선언(`CopyValueImpl`)을 두고, 생성 소스에서 그 템플릿을 정의하고 한 번 인스턴스화한다. 같은 이름으로 두면 과부하 해석이 정의 없는 템플릿을 고를 수 있다.
- 생성 소스는 자기 생성 헤더를 첫 include로 둔다.
- 생성 소스는 파일 지역 이름(익명 namespace, `static` 함수)과 매크로를 정의하지 않는다. 모든 정의가 한정된 이름의 멤버다. 그래서 유니티 묶음에 넣어도 충돌하지 않는다(GSD9).
- 생성 소스에는 타입과 필드 블록 앞에 원본 선언 위치 주석(`// <철자>:<줄> <타입>::<멤버>`, 저장소 상대 경로)을 남긴다. 컴파일 오류가 난 줄에서 원본 선언을 바로 찾기 위해서다.

### 5.5 모듈 생성 헤더와 소스

- 모듈 생성 헤더는 지금처럼 둘이다. public 쪽(`<M>/<M>.module.generated.h`)은 `RegisterModuleTypes(ModuleRegistration&, const ModuleLease& = {})`, private 쪽(`<M>/<M>.private.module.generated.h`, private 리플렉션 헤더가 있는 모듈만)은 `RegisterPrivateModuleTypes`의 선언만 담는다. include는 `Object/ModuleRegistration.h`뿐이다(이 헤더만 include한 TU는 0.4–0.6초다 — 1.2절).
- 모듈 생성 소스는 모듈마다 하나다. 이 모듈의 타입별 생성 헤더(public·private)와 의존 모듈의 public 모듈 생성 헤더를 include하고 두 등록 함수를 정의한다. 등록 순서(의존 모듈 먼저, enum → struct → 클래스, 클래스는 base 먼저), private 함수가 public 함수를 먼저 부르는 것, 실패 시 되돌리기는 지금과 같다.
- 모듈 키는 등록 함수 정의 안의 정적 객체다. 정의가 하나이므로 주소도 하나다.
- ML2가 더할 "자기 타입만 등록하는 함수"도 이 헤더에 선언하고 이 소스에 정의한다.
- BS8은 헤더의 선언에 모듈 API 매크로를 붙인다. 생성기는 헤더 밖 멤버의 선언과 등록 함수 선언을 한 함수로 내도록 만들어, BS8이 그 자리에 접두만 더하면 되게 한다.

## 6. 파일 배치와 빌드 연결

### 6.1 배치

```
<out>/Generated/<M>/Public/<철자>.generated.h                    (지금과 같다, M의 public include 루트)
<out>/Generated/<M>/Private/<철자>.generated.h                   (지금과 같다, M의 private include 루트)
<out>/Generated/<M>/Public/<M>/<M>.module.generated.h            (지금과 같다, 내용만 선언으로)
<out>/Generated/<M>/Private/<M>/<M>.private.module.generated.h   (지금과 같다, 내용만 선언으로)
<out>/Generated/<M>/Source/Public/<철자>.generated.cpp           (GS2)
<out>/Generated/<M>/Source/Private/<철자>.generated.cpp          (GS2)
<out>/Generated/<M>/Source/<M>.module.generated.cpp              (GS1)
```

- `Source/`는 include 루트가 아니다. public 헤더와 private 헤더의 철자가 같을 수 있으므로 `Public/`·`Private/`로 나눈다.
- 리플렉션 헤더로 등록했는데 타입이 하나도 없는 헤더는 지금처럼 메타 컴파일러 오류(RMC2000)다. 그래서 생성 헤더와 생성 소스는 늘 짝으로 있다.

### 6.2 메타 컴파일러

- 생성 헤더와 생성 소스를 함께 낸다. 서식(clang-format), 내용이 같으면 다시 쓰지 않는 게시, stage 뒤 게시하는 트랜잭션이 `Source/`도 다룬다. `--check`와 잉여 파일 검사는 출력 루트를 재귀로 훑으므로 `Source/`를 이미 덮는다(`CompilerDriver.cs:316–328`).
- descriptor는 바뀌지 않는다. 이 변경 전후의 descriptor가 바이트까지 같아야 한다(수용 기준). identity 명세는 생성이 읽기만 하므로(`CompilerDriver.cs:109–112, 300` — 쓰기는 `ids` 명령뿐) 영향이 없다.

### 6.3 Rocks.Build

- `MetadataLayout`이 모듈의 `Metadata` 목록에서 생성 소스 경로를 정해 그 모듈의 **리플렉션 생성 소스 목록**(`NativeModuleInput`의 새 필드)에 더한다. 손으로 쓴 `Sources`와 섞지 않는다. 경로는 생성 전에 정해지므로 계획 단계에서 더할 수 있다. 파일은 메타 생성이 끝난 뒤, 액션 그래프를 세우기 전에 존재한다.
- 목록을 따로 두는 이유는 셋이다. 모듈 진입점 판정을 바꾸지 않기 위해서다(아래). 컴파일 실패에 RMC3000 안내를 붙일 대상을 알아야 한다(7절). 유니티에서 생성 소스끼리만 묶을 수 있어야 한다(GSD9).
- 액션 그래프는 이 목록을 `Sources`와 같은 compile action으로 만든다. 모듈의 public·private include와 정의를 그대로 받는다. key는 내용이므로 생성 결과가 같으면 다시 컴파일하지 않는다.
- `.cpp`가 없던 리플렉션 모듈은 정적 라이브러리가 되어 archive와 `ModuleOutputs` 항목이 생긴다. 실행 파일 모듈이 `Metadata`를 가지면 그 실행 파일의 소스로 컴파일한다.
- 모듈 진입점: 모듈 로더는 코드가 있는 모듈에 진입점(`ROCKS_IMPLEMENT_MODULE`)을 요구한다(`Programs/Rocks.Build/ModuleTables.cs`의 `hasCode`). 리플렉션 생성 소스는 이 판정에 넣지 않는다. 그래서 소스 없던 리플렉션 모듈은 라이브러리가 되어도 지금처럼 진입점 면제다. 이 모듈들을 진입점 모듈로 바꾸는 일은 ML2가 정한다.
- 도구 빌드 ID: 생성 소스는 다른 소스와 같이 action 입력이다. 생성 내용에 호스트 경로를 넣지 않는 지금 규칙이 생성 소스에도 적용된다.
- `compile_commands.json`에 생성 소스 항목이 들어간다. 정책 감사는 생성 헤더와 같은 규칙으로 생성 소스를 건너뛴다.
- 설치 SDK는 생성 헤더만 싣는다. `Source/`는 public include 루트 밖이라 실리지 않고, 생성 소스의 object는 `ModuleOutputs`의 라이브러리로 실린다(`Programs/Rocks.Build.Cli/InstalledSdk.cs:102–116`). `ImportedMetadata`(descriptor·identity 명세)는 그대로다. 소스가 없던 리플렉션 모듈이 SDK에 들어가면 이제 라이브러리도 함께 실린다.

## 7. 검증 컴파일

| 대상 | 지금 | 이후 |
|---|---|---|
| 생성 헤더 | 게시 전, 헤더마다 `-fsyntax-only`, 순차 | 그대로. 헤더가 가벼워져 빨라진다 |
| 생성 소스(본문) | 생성 헤더 검증에 포함 | 빌드가 부를 때: 빌드의 compile action(병렬, 내용 key로 캐시). 옵션을 주면: 게시 전 메타 컴파일러가 검증 |

- 메타 컴파일러에 옵션 `--verify-sources`를 둔다. 켜면 게시 전에 생성 소스도 `-fsyntax-only`로 컴파일하고(병렬), 실패는 지금과 같이 RMC3000과 원본 위치 안내(`SourceNotes`)로 알리며 아무것도 게시하지 않는다.
  - 메타 컴파일러 단독 실행, `--check`, 도구 시험(`Rocks.ToolTests` — RMC3000 거부와 "위조 descriptor는 게시되지 않는다"에 기대는 시험이 있다)은 이 옵션을 쓴다. 이 경로의 보장은 지금과 같다.
  - Rocks.Build가 부르는 메타 생성은 이 옵션을 주지 않는다. 본문은 바로 뒤 compile action이 컴파일하기 때문이다.
- 빌드에서 생성 소스 compile action이 실패하면 Rocks.Build가 컴파일러 출력 뒤에 RMC3000 안내문을 붙인다. 원본 위치 안내는 스키마 모델이 있어야 만들 수 있어 빌드가 붙이지 못한다. 대신 생성 소스의 위치 주석(5.4절)으로 찾고, 안내문에 `--verify-sources`로 다시 돌리는 명령을 적는다.
- 달라지는 점: 빌드 경로에서는 본문이 컴파일되지 않는 출력도 게시된다. 생성물은 입력의 결정적 함수라서, 본문이 컴파일되지 않으면 빌드가 실패하고 입력을 고치면 다시 생성된다. 낡은 산출물을 성공으로 재사용하는 경로는 없다.

## 8. 소비자 변경

GS1(등록 함수 분리)에서만 소비 코드를 고친다.
- 모듈 생성 헤더가 타입별 생성 헤더를 include하지 않게 되므로, `GeneratedType<T>`의 멤버를 쓰는 코드는 그 타입의 생성 헤더를 직접 include한다. 2026-11-19 기준 손으로 쓴 파일 41개가 집계 헤더를 include하고(그중 4개는 시험 지원 헤더 `*TestSupport.h`), 56개가 이미 타입별 생성 헤더를 직접 include한다. 게임 템플릿(`Templates/Game/Foundation/Source/Main.cpp`)도 집계 소비자다.
- 등록만 하는 코드는 모듈 생성 헤더만 include한다.
- 손으로 쓴 헤더(공개·비공개 모두)는 모듈 생성 헤더를 include하지 않는다. 등록은 `.cpp`에서 한다. 정책 검사로 막는다.

GS2(타입별 본문 분리)는 소비 코드를 고치지 않는다.
- 생성 헤더의 include 목록을 그대로 두므로(GSD5 보류), 생성 헤더가 끌어오던 include에 기대는 코드가 깨지지 않는다.
- `GeneratedType<T>::Schema()`·`Ops()`·`Register()`·`CopyValue()` 등의 호출 문법은 그대로다. 링크할 때 소유 모듈의 라이브러리가 필요하다(의존 선언이 이미 있어야 하는 관계다).

## 9. 측정과 수용 기준

GS0에서 기준을 재고 단계마다 같은 장비·같은 방법으로 다시 잰다. 다른 빌드가 돌지 않는 상태에서 잰다.

| 항목 | 기준(1.2절) | 목표 | 단계 |
|---|---|---|---|
| 선언으로 등록 함수를 부르는 TU | 46 s(집계 헤더 include + 호출) | 1 s 이내(리뷰어 측정 0.4 s) | GS1 |
| 집계 헤더로 등록하던 시험 TU | 46–54 s(GS0 재측정 37–42 s) | 기록 — GS1 뒤 `TextAssignAppendTests.cpp` 2.4 s, `ArrayGraphTests.cpp` 5.5 s | GS1 |
| 모듈의 모든 생성 헤더 include만 하는 TU(ReflectionTestFixtures) | 17.6 s | 원본 헤더만 include한 TU 대비 +1.5 s 이내(시제품 +0.5 s) | GS2 |
| `TextAssignAppendTests.cpp` | 48 s | 5 s 이내(시제품 2.4 s) | GS2 |
| FoundationTests clean compile의 action 시간 합 | GS0 1,541 s(386 TU). GS1 뒤 495 s(389 TU), 따로 모듈 생성 소스 7개 54 s(ReflectionTestFixtures 35.8 s) | 절반 이하 — GS1에서 달성 | GS1·GS2 |
| FoundationTests object 디렉터리 크기, link 시간 | GS0 1,899 MB, 4.6 s. GS1 뒤 266 MB(따로 모듈 생성 소스 object 67 MB), 2.4 s | 줄어든다(기록) | GS1·GS2 |
| ReflectionTestFixtures 메타 생성 시간(검증 포함) | Foundation 타깃 메타 단계 전체(모듈 7개): GS0 107 s, GS1 뒤 86 s | 줄어든다(기록) | GS2 |

GS0·GS1 측정 조건(2026-11-20): 같은 장비에서 두 commit을 각각 내보낸 사본으로 clean Foundation Debug 빌드(`--jobs 8`)를 연달아 하고 `action-report.json`의 action별 소요 시간(GSD12)을 더했다. 다른 세션의 빌드가 거의 없는 상태였다(GS1 빌드가 끝날 무렵 다른 빌드가 시작됐다). 가장 느린 action은 GS0에서 집계 헤더로 등록하던 시험 TU들(38–42 s)이었고, GS1 뒤에는 ReflectionTestFixtures 모듈 생성 소스(35.8 s) 하나와 그다음 8.6 s 이하의 TU들이다. Foundation 타깃 전체 빌드(메타 단계 포함)는 389 s에서 255 s가 됐다. descriptor는 두 commit에서 바이트까지 같다.

동작 기준:
- 전체 시험(Debug)이 통과한다. descriptor가 변경 전후 바이트까지 같다.
- no-op 빌드는 0 action이다.
- 리플렉션 헤더 하나를 고쳤을 때 그 헤더의 생성 소스만 다시 컴파일되는 것은 지금 증분 key(1.1절)에서는 성립하지 않는다. UnityBuildPch UB0 뒤의 기준으로 둔다.
- GC 표시 벤치마크가 Development·Release에서 나빠지지 않는다. native 파생 클래스 사례를 포함한다.
- 생성 헤더 단독 컴파일 검사와 공개 헤더 검사가 통과한다.
- 메타 컴파일러 도구 시험의 거부 시험(RMC3000 등)이 `--verify-sources`로 지금과 같이 통과한다.

## 10. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| GSD1 | 생성 헤더에는 선언·컴파일 타임 형질·작은 inline만 두고 본문은 생성 소스에 둔다 | 확정 |
| GSD2 | 생성 소스는 리플렉션 헤더마다 하나, 모듈마다 등록용 하나다 | 확정 |
| GSD3 | inline으로 남기는 함수는 `Id()`, enum `IsDeclared()`, `Trace()`(struct·클래스)다. 나머지는 생성 소스다(5.1절 표) | 확정 |
| GSD4 | `static_assert` 묶음과 `VerifyBinds()`는 생성 소스의 검사 전용 멤버 함수에 둔다 | 확정 |
| GSD5 | 생성 헤더의 include를 줄이는 일(codec·경로·접근 헤더 제거, `Object/ValueCopy.h`·Trace 보조 함수 분리)은 하지 않는다. 이득이 TU당 0.3–0.5초이고 소비 TU가 깨진다. GS3 측정 뒤 다시 본다 | 보류 |
| GSD6 | 모듈 생성 헤더는 등록 함수 선언만 담는다. 소비자는 쓰는 타입의 생성 헤더를 직접 include하고, 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다 | 확정 |
| GSD7 | 생성 소스는 `<out>/Generated/<M>/Source/` 아래에 둔다. 소스 없던 리플렉션 모듈은 정적 라이브러리가 된다 | 확정 |
| GSD8 | 빌드 경로의 게시 전 검증은 생성 헤더만 한다. 본문은 빌드의 compile action이 검증하고, 실패하면 RMC3000 안내를 붙인다. 메타 컴파일러 옵션 `--verify-sources`는 생성 소스까지 게시 전에 검증한다(단독 실행·`--check`·도구 시험) | 확정 |
| GSD9 | 생성 소스는 파일 지역 이름·매크로 없이 낸다. 유니티에서는 생성 소스끼리 묶는다(손으로 쓴 소스와 섞지 않는다, 기본 켬 — 2026-11-20 UnityBuildPch UB1에서 결정, 모듈 규칙 `ReflectionUnity = false`로 끈다) | 확정 |
| GSD10 | 링크 방식과 무관하게 같은 구조다(BD28). 모듈 API 매크로와 `NativeTypeKey` 교체는 BS8, 자기 타입만 등록하는 함수는 ML2가 모듈 생성 소스에 더한다 | 확정 |
| GSD11 | 설치 SDK는 생성 헤더만 싣는다 | 확정 |
| GSD12 | action마다 소요 시간을 `action-report.json`에 기록한다(측정 기반) | 확정 |
| GSD13 | 생성 코드의 양을 줄이는 일(필드마다 thunk를 만드는 대신 값 종류와 offset으로 도는 공용 codec)은 별도 과제로 둔다(12절) | 보류 |
| GSD14 | 등록 함수 분리(GS1)를 타입별 본문 분리(GS2)보다 먼저 한다. 리플렉션 생성 소스는 `Sources`와 다른 목록으로 나르고 모듈 진입점 판정에 넣지 않는다(진입점 모듈로 바꾸는 일은 ML2) | 확정 |

## 11. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험으로 검증한다.

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| GS0 | `action-report.json`에 action별 소요 시간 기록(GSD12). 9절 기준 측정 | 기준 수치 기록 |
| GS1 | **등록 함수 분리.** 모듈 생성 헤더를 선언만 남기고 모듈 생성 소스를 낸다(5.5절). `Source/` 루트를 게시 트랜잭션에 더한다. `MetadataLayout`이 리플렉션 생성 소스 목록에 더하고 액션 그래프가 컴파일한다(6.3절, 진입점 판정 제외). 모듈 생성 소스는 빌드가 컴파일한다(도구 시험이 필요하면 `--verify-sources`). 소비자 include를 고치고(8절), 손으로 쓴 헤더의 모듈 생성 헤더 include를 막는 정책 검사를 넣는다. 타입별 생성 헤더는 그대로 둔다 | 전체 시험(Debug)·설치 SDK 게임 빌드 통과, descriptor 바이트 동일, 메타 컴파일러 시험(생성물 대조) 갱신, 소스 없던 리플렉션 모듈의 archive 생성, 정책 검사 위반·통과 fixture, 9절 GS1 행 측정 |
| GS2 | **타입별 본문 분리.** 메타 컴파일러가 타입별 생성 헤더·생성 소스를 나눠 낸다(5.1–5.4절, include 목록은 그대로). `--verify-sources`를 넣고 빌드 경로의 게시 전 검증을 생성 헤더로 줄이며 RMC3000 안내를 빌드에 붙인다(7절). 소비 코드는 고치지 않는다 | 전체 시험 통과, descriptor 바이트 동일, 메타 컴파일러 시험 갱신(거부 시험은 `--verify-sources`로 유지), 본문 계약 위반 fixture가 RMC3000 안내와 함께 빌드 실패, GC 벤치마크 |
| GS3 | 9절 재측정, 문서 개정(13절), GSD5·GSD13 재검토 | 9절 목표와 동작 기준 |

## 12. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 등록 | `.gen.cpp`의 정적 객체가 프로그램 시작 때 등록한다 | 등록 함수를 명시 호출한다(모듈 로더가 부른다) | 정적 라이브러리에서 object가 빠지는 문제와 초기화 순서 의존이 없다 |
| 생성 헤더의 형태 | `GENERATED_BODY()`가 펼치는 매크로 묶음. 원본 헤더가 생성 헤더를 include한다 | `GeneratedType<T>` 특수화. 생성 헤더가 원본 헤더를 include한다 | 기존 구조를 유지한다 |
| 필드 접근 | 필드 기술 데이터(offset·종류)를 공용 코드가 해석한다. 필드마다 코드를 만들지 않는다(`UhtProperty.cs:1682, 1936`) | 필드마다 타입 있는 thunk를 만든다 | 기존 구조다. 생성 코드가 큰 원인이며(fixture 모듈 object 54 MB), 양을 줄이는 것은 GSD13으로 남긴다 |
| 타입 정보 export | 클래스를 export하지 않으면 `MinimalAPI`가 필요하다 | 항상 export한다(BD28) | BuildTargets 결정 |

보류 항목 메모:
- GSD13: 본문을 한 번만 컴파일하게 하면 중복은 없어지지만 생성 코드의 양은 그대로다. fixture 모듈 하나가 인스턴스화 11초, 코드 생성 21초, object 54 MB다. 엔진 타입이 늘면 이 비용이 모듈마다 생긴다. 값 종류와 offset으로 도는 공용 codec으로 필드별 thunk를 줄이는 방법을 GS3 측정 뒤에 따로 검토한다.
- GSD5: 생성 헤더의 include를 줄이려면 `Object/ValueCopy.h`(지금 `GeneratedFieldAccess.h`·`RuntimeTypeOps.h`를 include한다)와 Trace 보조 함수(`ArrayFieldCodec.h`의 `TraceArrayField`, `MapFieldCodec.h`의 `TraceMapField`, 그리고 이들이 쓰는 `GeneratedFieldAccess.h`의 concept과 `RuntimeTypeOps.h`의 `TraceVisitor`)를 가벼운 헤더로 나눠야 한다. 소비 코드의 include도 고쳐야 한다.

## 13. 다른 문서 개정

이번 개정에서 고친 것:
- [빌드 타깃](BuildTargets.ko.md) BD28(3.8절 본문과 결정 표): 생성 코드 분리의 구현 스펙이 이 문서이고, 생성 소스 단위가 "헤더마다 하나 + 모듈마다 하나"임을 적었다.
- [유니티 빌드·PCH](UnityBuildPch.ko.md) 1절 "생성 TU" 행: 리플렉션 생성 소스가 생긴다는 것과 이 문서 링크를 적었다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절: 이 문서 링크를 적었다.

구현할 때 고칠 것:
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절(배치에 `Source/` 추가), 1.2절(검증 컴파일 — 7절), 집계 헤더 서술(모듈 생성 헤더는 선언만): GS1. struct·enum 절의 "생성 헤더가 … 낸다" 서술: GS2.
- [유니티 빌드·PCH](UnityBuildPch.ko.md) UD5·16절 "생성 코드 묶기": 리플렉션 생성 소스를 자기들끼리 묶을지 UB1에서 정해 반영한다.
- [모듈 로더](ModuleLoader.ko.md) 3.10절(ML2): 생성 함수의 선언·정의 위치를 모듈 생성 헤더·소스로, 빌드 생성 TU `<M>.ModuleTypes.cpp`와 inline OwnTypes 안을 대체(모듈 로더 세션이 고친다).
- `Programs/Rocks.Build/README.ko.md`: 리플렉션 생성 소스 목록, 소스 없던 리플렉션 모듈의 archive, action 소요 시간 기록.
- `Programs/Rocks.MetaCompiler` 문서·도움말: 출력 목록, `--verify-sources`, 검증 범위.

## 14. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-19 | 생성 코드 분리 설계. 측정(1.2절), 멤버별 위치(5.1절), 배치와 빌드 연결(6절), 검증 컴파일 변경(7절), 결정 GSD1–GSD14(GSD13 보류). 등록 함수 분리(GS1)를 먼저 하기로 하고 착수했다(사용자 결정). 모듈 로더 ML2와 조율해 리플렉션 생성 소스를 별도 목록으로 나르고 진입점 판정에서 뺐다 |
| 2 | 2026-11-19 | 리뷰어 교차 검증 반영. 시제품(기존 생성 헤더 35개를 나눠 컴파일)과 조용한 상태 측정을 1.2절에 더했다. 생성 헤더의 include 줄이기를 보류로 돌리고(GSD5) 런타임 준비 단계를 없앴다 — 이득이 TU당 0.3–0.5초이고 소비 TU 17개가 깨진다. 그래서 타입별 본문 분리(GS2)는 소비 코드를 고치지 않는다. 클래스 `Trace()`도 inline으로 남긴다(native 파생 GC 경로, GSD3). 메타 컴파일러 옵션 `--verify-sources`를 더했다(GSD8, 도구 시험의 거부 보장 유지). 증분 재컴파일 기준을 UB0 뒤로 조건을 달았다. `CopyValueImpl` 이름, 완전한 반환 타입 규칙, private 모듈 생성 헤더, 인용·수치 정정(identity 명세는 생성이 쓰지 않는다, BuildTargets 3.4절, 소스 없는 모듈 6개, 집계 소비 파일 41개). 단계 GS0–GS3 |
| 2(구현 기록) | 2026-11-20 | GS0·GS1 구현과 측정(9절), engine-runtime 병합. 스펙에 없던 것 둘을 구현하며 정했다. 첫째, 설치 SDK가 실어 가는 "리플렉션 생성 소스만으로 된 라이브러리"에 표지 `ReflectionOnlyLibrary`를 붙였다(SDK manifest 7) — 게임 빌드의 모듈 진입점 판정은 미리 빌드한 라이브러리를 코드로 세므로, 표지가 없으면 CoreTypeBindings 같은 모듈이 SDK를 거치며 진입점 모듈이 된다(6.3절의 면제를 SDK에서도 유지). 둘째, MetaCompiler 옵션 `--verify-sources`(빌드 없이 쓰는 단독 실행·도구 시험이 모듈 생성 소스도 게시 전에 구문 검사한다 — 빌드는 넘기지 않는다) |
| 2(구현 기록 2) | 2026-11-20 | GS2 구현. 타입별 생성 헤더는 선언·형질·`Id()`·enum `IsDeclared()`·inline `Trace()`만 남기고 본문을 `Source/Public|Private/<철자>.generated.cpp`로 옮겼다(5.1–5.4절 그대로, include 목록 불변, 소비 코드 수정 없음). 구현하며 정한 점 넷. 첫째, 안내문의 "다시 돌리는 명령"(7절)은 MetaCompiler 인자 전체가 아니라 빌드 명령 옵션 `--verify-metadata-sources on`(build·test·sdk·game)으로 했다 — MetaCompiler 인자는 stage 경로를 담아 빌드 뒤에는 그대로 다시 돌릴 수 없고, 옵션은 같은 명령에 덧붙이면 메타 단계가 `--verify-sources`로 게시 전에 원본 위치와 함께 거부한다. 안내문은 `NativeModuleInput.ReflectionSourceFailureNote`(표시 전용, action key 밖)로 싣고 리플렉션 생성 소스의 compile action 실패에만 붙는다. 둘째, 게시 전 검증 컴파일을 생성 헤더까지 포함해 병렬로 돌린다(MetaCompiler `--jobs`, 빌드가 자기 `--jobs`를 넘긴다, 실패 보고는 생성물 순서에서 가장 앞선 것) — 7절은 `--verify-sources`의 생성 소스만 병렬이라고 적었으나 헤더 검증도 생성물마다 독립이다. 셋째, 설치 SDK manifest를 8로 올렸다(칸은 같다) — 이전 SDK의 생성 헤더는 본문을 inline으로 정의하고 동봉 MetaCompiler가 타입별 생성 소스를 내지 않아 새 빌드 도구와 맞지 않는다. 넷째, 도구 시험은 생성 코드의 문구를 헤더+생성 소스에서 읽고(본문을 고쳐 컴파일 실패를 보는 시험은 생성 소스를 고친다), 생성 static_assert probe는 생성 소스까지 한 번역 단위로 컴파일한다. 전체 빌드·시험과 GC 벤치마크는 단계별로 돌리지 않고 UB 단계까지 끝낸 뒤 한 번 검증한다(2026-11-20 사용자 결정) |
