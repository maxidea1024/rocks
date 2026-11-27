# 리플렉션 생성 소스 설계

기준일: 2026-11-19, 개정 2. 메타 컴파일러가 내는 리플렉션 코드를 둘로 나눈다.
- **생성 헤더**(`X.generated.h`): 선언, 컴파일 타임 형질, 작은 inline 함수만 둔다.
- **생성 소스**(`X.generated.cpp`): 스키마 표, 필드 연산 표, 등록 함수 같은 본문을 둔다. 모듈이 한 번만 컴파일한다.

지금은 본문 전체가 생성 헤더에 inline으로 들어 있어, 그 헤더를 include하는 번역 단위마다 같은 코드를 다시 인스턴스화하고 코드 생성한다. 시험 빌드 시간의 가장 큰 원인이다(1절).

다른 문서와의 관계:
- [빌드 타깃](BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 BD28이 모듈식 링크를 위해 "생성 `.cpp`를 낸다"고 이미 정했다(BS8 단계, 미구현). 이 문서는 그 가운데 **생성 코드를 헤더에서 소스로 옮기는 부분**의 구현 스펙이고, 컴파일 시간 때문에 BS8보다 먼저 한다. 모듈 API 매크로 주입과 `NativeTypeKey` 교체는 BS8에 그대로 둔다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절(생성물 배치)·1.2절(검증 컴파일)과 등록 절의 집계 헤더 서술을 바꾼다(13절).
- [모듈 로더](ModuleLoader/README.ko.md) ML2(개정 3 작업 중)는 빌드가 만드는 TU `<M>.ModuleTypes.cpp`에 "모듈 자신의 타입만 등록하는 함수"를 두려 했다. 이 문서의 모듈 생성 소스가 그 자리를 대신한다. ML2는 GS1 병합 뒤에 그 함수와 진입점 연결을 모듈 생성 소스 구조 위에 더한다(모듈 로더 세션과 2026-11-19 조율).
- [유니티 빌드·PCH](UnityBuildPch.ko.md)는 "리플렉션 생성물은 헤더뿐"을 전제로 썼다. 생성 소스가 생기므로 그 전제를 고친다(13절).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 UnrealHeaderTool이다. 경로는 `Engine/Source/Programs/Shared/EpicGames.UHT/Exporters/CodeGen/` 기준이다. UE 이름은 대응표와 근거에만 적는다. 측정은 Rocks 툴체인(clang-cl 21.1.6, foundation-debug)으로 했다.

상태: **설계 확정**(결정 GSD1–GSD14, 2026-11-19 사용자 승인 — 권장안. GSD5·GSD13은 2026-11-21 재검토에서 하지 않기로 했다 — 12절). 리뷰어 교차 검증을 반영했다(개정 2). 리뷰어가 기존 생성 헤더 35개를 5절 방식으로 나눠 전부 컴파일해 구조가 성립함을 확인했다. 구현 단계는 GS0–GS3(11절)이다. GS1(등록 함수 분리)은 2026-11-19 사용자 결정으로 착수했다. 다른 세션은 GS1 병합까지 전체 빌드·시험 반복을 보류한다. GS0(action 소요 시간 기록·기준 측정)과 GS1은 2026-11-20 구현해 engine-runtime에 병합했다(측정값은 9절). GS2(타입별 본문 분리)도 2026-11-20 구현했다(구현하며 정한 점은 개정 이력). 이후 단계의 기준·전후 측정용 빌드는 따로 돌리지 않는다(2026-11-20 사용자 결정) — 숫자는 일반 검증 빌드의 action 소요 시간(GSD12)으로 남긴다.

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
- 생성 헤더의 include를 줄이는 일(GSD5)과 생성 코드의 양 자체를 줄이는 일(GSD13): 하지 않는다(2026-11-21 재검토 — 12절).

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
| bitfield `Get_x()`·`Set_x()` | 클래스·struct | 생성 소스(헤더에 선언) | `Ops()` 안에서 쓴다. 로깅 `VisitFields`는 부르지 않는다(자기·상속 bitfield 모두 헤더 안에서 직접 읽는다 — 다른 모듈이 인스턴스화할 때 export되지 않은 `Get_`을 부르지 않게) |
| `VerifyBinds()`, `static_assert` 묶음 | 전부 | 생성 소스(검사 전용 멤버) | 5.3절 |
| `detail::ValueCopy<S>`·`detail::ValueValidate<S>` 특수화 | struct | 헤더 | 형질 값과, `CopyValue()`·`ValidateTree()`로 넘기는 한 줄 전달 함수다 |
| `RegisterModuleTypes()`·`RegisterPrivateModuleTypes()` | 모듈 | 모듈 생성 소스(모듈 생성 헤더에 선언) | 모든 타입의 `Schema()`·`Ops()`를 부른다. 모듈 키(`moduleKey`)도 여기 있다 |

다른 세션이 더하는 생성 멤버도 같은 기준을 따른다. 컴파일 중에 쓰이거나 호출마다 inline이 되어야 하는 것(예: 로깅 LG16의 `constexpr` 필드 방문 `VisitFields`)은 헤더에, 표와 thunk는 생성 소스에 둔다.

### 5.2 `Trace()`를 inline으로 두는 이유

- struct의 `Trace()`는 소유 클래스의 `Trace()`와 배열 보조 함수(`StructRefArray<S>::Trace`, `detail::TraceArrayField`·`TraceMapField`)가 원소마다 직접 부른다.
- 클래스의 `Trace()`는 `RuntimeTypeOps::trace` 함수 포인터로 불리는 경로 말고도, native 파생 클래스의 GC 경로(`TraceNativeChain`, `Object/NativeClass.h:198–207`)가 직접 부른다.
- 생성 소스로 옮기면 이 호출들이 TU 경계를 넘는다. Release는 ThinLTO가 다시 inline하지만 Development(`-O2`, LTO 없음)는 그렇지 않다. GC 규모 작업(100만 객체)의 측정 구성을 건드리지 않으려고 inline으로 둔다.
- `Trace()` 본문은 작다(참조 필드마다 방문 호출 하나). 참조가 없는 타입은 빈 함수다. 생성 헤더의 include를 그대로 두므로(GSD5 — 줄이지 않는다) Trace 보조 함수를 옮길 필요가 없다.

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
- 모듈 생성 소스는 모듈마다 하나다. 이 모듈의 타입별 생성 헤더(public·private)와 의존 모듈의 public 모듈 생성 헤더를 include하고 등록 함수 세 개를 정의한다(`RegisterModuleTypes`·`RegisterPrivateModuleTypes` 가운데 있는 것과, 모듈 로더가 부르는 자기 타입 함수 `RegisterModuleOwnTypes` — [모듈 로더](ModuleLoader/03-Design/03.10-ML2Wiring.ko.md) 3.10절). 등록 순서(의존 모듈 먼저, enum → struct → 클래스, 클래스는 base 먼저), private 함수가 public 함수를 먼저 부르는 것, 실패 시 되돌리기는 지금과 같다.
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
- 목록을 따로 두는 이유는 셋이다. 모듈 진입점 판정을 바꾸지 않기 위해서다(아래 — ML2 뒤로는 진입점 판정이 이 목록도 세고, 이 목록은 진입점의 자기 타입 표지 `ROCKS_MODULE_TYPES`를 정한다). 컴파일 실패에 RMC3000 안내를 붙일 대상을 알아야 한다(7절). 유니티에서 생성 소스끼리만 묶을 수 있어야 한다(GSD9).
- 액션 그래프는 이 목록을 `Sources`와 같은 compile action으로 만든다. 모듈의 public·private include와 정의를 그대로 받는다. key는 내용이므로 생성 결과가 같으면 다시 컴파일하지 않는다.
- `.cpp`가 없던 리플렉션 모듈은 정적 라이브러리가 되어 archive와 `ModuleOutputs` 항목이 생긴다. 실행 파일 모듈이 `Metadata`를 가지면 그 실행 파일의 소스로 컴파일한다.
- 모듈 진입점: 모듈 로더는 코드가 있는 모듈에 진입점(`ROCKS_IMPLEMENT_MODULE`)을 요구한다(`Programs/Rocks.Build/ModuleTables.cs`의 `hasCode`). 리플렉션 생성 소스는 이 판정에 넣지 않는다. 그래서 소스 없던 리플렉션 모듈은 라이브러리가 되어도 지금처럼 진입점 면제다. 이 모듈들을 진입점 모듈로 바꾸는 일은 ML2가 정한다. → ML2(2026-11-20)가 바꿨다: 진입점 판정이 리플렉션 생성 소스를 세고, 손으로 쓴 소스가 없는 메타데이터 모듈은 빌드 생성 진입점 TU를 받으며, 설치 SDK 표지 `ReflectionOnlyLibrary`는 없어졌다(SDK manifest 8 — [모듈 로더](ModuleLoader/03-Design/03.10-ML2Wiring.ko.md) 3.10절 "GS1 위 세부").
- 도구 빌드 ID: 생성 소스는 다른 소스와 같이 action 입력이다. 생성 내용에 호스트 경로를 넣지 않는 지금 규칙이 생성 소스에도 적용된다.
- `compile_commands.json`에 생성 소스 항목이 들어간다. 정책 감사는 생성 헤더와 같은 규칙으로 생성 소스를 건너뛴다.
- **MetaCompiler 인자는 응답 파일로 넘긴다**(개정 6, 2026-11-26). 헤더마다 `--input`·`--include` 쌍이 붙어, 헤더가 200개 가까운 모듈(MaterialSchema 노드 속성 — MaterialSystem 19.13절)은 Windows 명령줄 상한(32,767자)을 넘는다. `MetadataRunner`가 인자를 stage의 `<모듈>.metacompiler.rsp`(한 줄에 인자 하나, UTF-8 — 모듈 출력 루트 밖이라 `--check`의 남는 파일 검사와 게시에 섞이지 않는다)에 쓰고 `dotnet <MetaCompiler> @<경로>`로 부른다. MetaCompiler는 인자가 `@<경로>` 하나면 그 줄들을 인자로 쓴다. 경로·인자에 줄바꿈이 없으므로 따옴표 규칙이 필요 없다. 버린 대안은 모듈을 쪼개는 것(구조를 도구 한계에 맞추는 우회)과 길이가 넘을 때만 파일을 쓰는 것(경로가 둘이 되어 한쪽이 덜 시험된다)이다.
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
- 생성 헤더의 include 목록을 그대로 두므로(GSD5 — 줄이지 않는다), 생성 헤더가 끌어오던 include에 기대는 코드가 깨지지 않는다.
- `GeneratedType<T>::Schema()`·`Ops()`·`Register()`·`CopyValue()` 등의 호출 문법은 그대로다. 링크할 때 소유 모듈의 라이브러리가 필요하다(의존 선언이 이미 있어야 하는 관계다).

### 8.1 생성 헤더가 보이지 않는 번역 단위

**문제**
- 리플렉션 헤더(`Foo.h`)는 자기 생성 헤더를 include하지 않는다. 그래서 타입 `S`는 보이는데 `GeneratedType<S>`는 보이지 않는 번역 단위가 생긴다.
- `GeneratedType<S>`가 있는지를 탐지식(`requires { GeneratedType<S>::… }`)이나 생성 헤더가 내는 특수화로 고르는 템플릿은, 그런 번역 단위에서 오류 없이 "리플렉션 타입이 아닌 경우"의 길로 간다. 생성 헤더를 본 번역 단위와 판정이 갈리면 같은 템플릿이 두 가지로 인스턴스화되어 ODR 위반이기도 하다.

**전수 조사**(2026-11-21, 손으로 쓴 헤더의 탐지 지점)

| 지점 | 보이지 않을 때 | 판정 |
|---|---|---|
| `TypeIdOf`·`TypeOf`(`Meta/GeneratedType.h`), `ObjectTypeIdOf`·`ReflectedAncestorOf`(`Object/NativeClass.h`) | `static_assert`가 include를 안내한다 | 이미 컴파일 오류 |
| `ArrayFieldCodec.h`의 `GeneratedType<E>::ContainsStrongReference` 등 직접 사용 | 불완전 타입 오류 | 이미 컴파일 오류 |
| `ReflectedStruct`·`SettingsStruct`(`Config/ConfigContext.h`), `ReflectedEnum`(`Config/CVarEnum.h`) | 제약을 만족하지 못해 호출이 컴파일되지 않는다 | 이미 컴파일 오류 |
| `detail::ValueCopy<S>` 일반형(`Object/ValueCopy.h`) | 생성 특수화 대신 C++ 복사를 하거나 `Unsupported`를 돌려준다 | **조용히 갈림** |
| `detail::ValueValidate<S>` 일반형(같은 파일) | 검증 hook을 건너뛴다 | **조용히 갈림** |
| `ReflectedStructField<S>::Supported`(`Object/GeneratedFieldAccess.h`) | 거짓이 되어 필드 형질이 struct가 아닌 길로 간다 | **조용히 갈림** |
| `GeneratedFieldVisitor`·`GeneratedLogFields`(같은 파일) | 로그 구조 특수화가 빠진다 | **조용히 갈림** |
| `render::CheckRenderTransfer<T>`(`RenderBase/RenderTransfer.h`) | 스키마 검사를 건너뛴다 | **조용히 갈림** |
| `NativeTypeKey<T>`(`Object/Object.h`) | 리플렉션 클래스의 키는 생성 `.cpp`의 `GeneratedType<T>::NativeKey()`다([빌드 타깃](BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 BS8b) — `ReflectedDeclarationV`와 Object 파생으로 고르고 생성 헤더가 없으면 `static_assert`(struct·enum도 생성 키(BS8d4 — [빌드 타깃](BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절)) | 컴파일 오류(BS8b) |
| `ReflectedEnumField<E>::Supported`(`Object/GeneratedFieldAccess.h`) | 거짓이 된다 | 표지 없음(아래 한계) |

**규칙**
- 리플렉션 클래스·struct는 정의 안에 `ROCKS_GENERATED_BODY()`를 둔다(UE와 같다). 이 매크로가 타입 자신에게 선언만으로 판정되는 표지를 남긴다(`ReflectedDeclarationV<T>` — 생성 헤더 없이도 번역 단위마다 같은 값이다).
  - 표지는 자기 타입을 돌려주는 멤버 함수 선언이다. 상속으로 물려받은 표지는 반환 타입이 달라 세지 않는다.
  - 빌드의 메타 단계가 표지 없는 리플렉션 클래스·struct를 거부한다(`RMC2211`, MetaCompiler `--require-generated-body` — `MetadataRunner`가 늘 넘긴다). Binds 미러 선언은 대상이 아니다(실제 타입은 미러가 아니다).
- 탐지 지점은 한 관문(`detail::GeneratedHeaderVisible<T>()`)을 거친다. 표지가 있는 타입인데 `GeneratedType<T>`가 보이지 않으면 `static_assert`로 컴파일 오류를 내고 include할 헤더를 안내한다. 위 표의 "조용히 갈림" 다섯 지점에 넣는다.
- 탐지식을 새로 쓸 때는 이 관문을 쓴다(`requires { GeneratedType<T>::… }`만으로 고르지 않는다).
- 판정은 cv 한정자를 뗀 타입으로 한다. `GeneratedType`은 한정자 없는 타입에만 특수화되므로, 표지가 있는 타입을 `const`·`volatile`을 붙인 채 관문에 넘기면 컴파일 오류다(한정자를 떼고 쓰라고 안내한다). 그대로 두면 생성 헤더를 include해도 "리플렉션 타입이 아닌 경우"의 길로 간다.
- 관문은 완전한 타입만 받는다. 전방 선언만 보이는 클래스 타입을 넘기면 컴파일 오류다(표지를 읽을 수 없어 조용히 통과하는 것을 막는다).
- 메타 단계의 규칙(`RMC2211`)과 C++ 관문은 같은 것을 본다: 파서는 매크로가 남긴 접근 표지(`rocksGeneratedAccessMarker`)와 자기 타입 표지(`rocksReflectedSelf`)가 둘 다 있어야 `ROCKS_GENERATED_BODY()`가 있다고 본다.

**로그 인자**
- 로그 호출은 인자마다 로그 구조 특수화가 있는지 본다(`Core/Diagnostics/LogCapture.h`의 `WithArgumentInfo`가 모든 인자에 `HasLogStructure`를 평가한다). 그래서 표지가 있는 타입의 값을 로그에 넘기는 번역 단위는 그 타입에 `Formatter`가 따로 있어도 생성 헤더를 include해야 한다. 생성 헤더를 본 번역 단위와 판정이 갈리면 ODR 위반이므로 맞는 동작이다.

**한계**
- 리플렉션 enum에는 표지를 둘 자리가 없다(enum 정의에는 본문 매크로를 넣을 수 없다). enum의 탐지 지점은 생성 헤더가 없으면 컴파일 오류이거나(`ReflectedEnum`) 결과가 같다(값 복사).
- Binds 미러의 실제 타입(`Vector3` 등)에는 표지가 없다. 이 타입들은 평범한 값이라 일반형의 동작이 생성 특수화와 같고, 로그 구조는 처음부터 Binds 미러를 뺀다(`GeneratedLogFields`).
- 관문의 판정은 번역 단위 안에서 처음 평가한 값으로 고정된다. 유니티 묶음에서는 앞 구성원이 생성 헤더를 include하면, include를 빠뜨린 뒤 구성원도 통과한다(거짓 오류는 없다). 유니티를 끈 검증 변형([유니티 빌드·PCH](UnityBuildPch.ko.md) 11절 — CI의 Windows 단계)이 소스마다 따로 컴파일해 이것을 잡는다.
- 리플렉션 헤더 자신이 템플릿이 아닌 inline 코드에서 자기 타입으로 이 템플릿들을 쓰면(예: 멤버 함수 안의 `TryCopyValue(*this, …)`), 그 헤더만 보는 번역 단위 — MetaCompiler의 파싱과 공개 헤더 단독 컴파일 검사 — 에서 관문 오류가 난다. 생성 헤더는 리플렉션 헤더 뒤에야 올 수 있으므로 그런 코드는 `.cpp`나 템플릿으로 옮긴다.
- 매크로 이름 `ROCKS_GENERATED_BODY`가 문자열로 박힌 곳이 있다. 매크로를 개명할 때 함께 바꾼다: `Meta/GeneratedType.h`의 관문 문구, MetaCompiler의 `RMC2211` 메시지와 hint(`ClangAstParser.cs`), 옵션 설명(`CompilerOptions.cs`), 도구 시험의 기대 문구(`Rocks.ToolTests/MetaTests.Cli.cs`).
- MetaCompiler를 `--require-generated-body` 없이 직접 돌리면(도구 시험의 작은 입력) 표지 없는 타입도 받는다. 그런 타입은 관문이 지켜 주지 않는다.

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
| GSD5 | 생성 헤더의 include를 줄이는 일(codec·경로·접근 헤더 제거, `Object/ValueCopy.h`·Trace 보조 함수 분리)은 하지 않는다. 2026-11-21 재검토: 줄일 수 있는 몫이 Foundation clean 컴파일 CPU 시간의 2.8% 이하이고 소비 번역 단위를 고쳐야 한다(12절) | 하지 않음 |
| GSD6 | 모듈 생성 헤더는 등록 함수 선언만 담는다. 소비자는 쓰는 타입의 생성 헤더를 직접 include하고, 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다 | 확정 |
| GSD7 | 생성 소스는 `<out>/Generated/<M>/Source/` 아래에 둔다. 소스 없던 리플렉션 모듈은 정적 라이브러리가 된다 | 확정 |
| GSD8 | 빌드 경로의 게시 전 검증은 생성 헤더만 한다. 본문은 빌드의 compile action이 검증하고, 실패하면 RMC3000 안내를 붙인다. 메타 컴파일러 옵션 `--verify-sources`는 생성 소스까지 게시 전에 검증한다(단독 실행·`--check`·도구 시험) | 확정 |
| GSD9 | 생성 소스는 파일 지역 이름·매크로 없이 낸다. 유니티에서는 생성 소스끼리 묶는다(손으로 쓴 소스와 섞지 않는다, 기본 켬 — 2026-11-20 UnityBuildPch UB1에서 결정, 모듈 규칙 `ReflectionUnity = false`로 끈다) | 확정 |
| GSD10 | 링크 방식과 무관하게 같은 구조다(BD28). 모듈 API 매크로와 `NativeTypeKey` 교체는 BS8, 자기 타입만 등록하는 함수는 ML2가 모듈 생성 소스에 더한다 | 확정 |
| GSD11 | 설치 SDK는 생성 헤더만 싣는다 | 확정 |
| GSD12 | action마다 소요 시간을 `action-report.json`에 기록한다(측정 기반) | 확정 |
| GSD13 | 생성 코드의 양을 줄이는 일(필드마다 thunk를 만드는 대신 값 종류와 offset으로 도는 공용 codec)은 하지 않는다. 2026-11-21 재검토: 엔진 모듈의 생성 소스는 clean 컴파일 CPU 시간의 1.9%이고, 큰 몫은 시험 fixture 모듈이다. 다시 볼 조건은 12절에 있다 | 하지 않음 |
| GSD14 | 등록 함수 분리(GS1)를 타입별 본문 분리(GS2)보다 먼저 한다. 리플렉션 생성 소스는 `Sources`와 다른 목록으로 나르고 모듈 진입점 판정에 넣지 않는다(진입점 모듈로 바꾸는 일은 ML2). → 진입점 판정 면제는 ML2(2026-11-20)가 거뒀다(개정 이력 3) | 확정 |
| GSD15 | 리플렉션 클래스·struct는 `ROCKS_GENERATED_BODY()`를 반드시 둔다. 이 매크로가 선언만으로 판정되는 표지를 남기고, 빌드의 메타 단계가 표지 없는 타입을 거부한다(`RMC2211`). Binds 미러는 예외다(8.1절) | 확정(2026-11-21) |
| GSD16 | `GeneratedType<T>`의 유무로 길을 고르는 템플릿은 관문 `detail::GeneratedHeaderVisible<T>()`를 거친다. 표지가 있는데 생성 헤더가 보이지 않으면 컴파일 오류다(8.1절) | 확정(2026-11-21) |

## 11. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험으로 검증한다.

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| GS0 | `action-report.json`에 action별 소요 시간 기록(GSD12). 9절 기준 측정 | 기준 수치 기록 |
| GS1 | **등록 함수 분리.** 모듈 생성 헤더를 선언만 남기고 모듈 생성 소스를 낸다(5.5절). `Source/` 루트를 게시 트랜잭션에 더한다. `MetadataLayout`이 리플렉션 생성 소스 목록에 더하고 액션 그래프가 컴파일한다(6.3절, 진입점 판정 제외). 모듈 생성 소스는 빌드가 컴파일한다(도구 시험이 필요하면 `--verify-sources`). 소비자 include를 고치고(8절), 손으로 쓴 헤더의 모듈 생성 헤더 include를 막는 정책 검사를 넣는다. 타입별 생성 헤더는 그대로 둔다 | 전체 시험(Debug)·설치 SDK 게임 빌드 통과, descriptor 바이트 동일, 메타 컴파일러 시험(생성물 대조) 갱신, 소스 없던 리플렉션 모듈의 archive 생성, 정책 검사 위반·통과 fixture, 9절 GS1 행 측정 |
| GS2 | **타입별 본문 분리.** 메타 컴파일러가 타입별 생성 헤더·생성 소스를 나눠 낸다(5.1–5.4절, include 목록은 그대로). `--verify-sources`를 넣고 빌드 경로의 게시 전 검증을 생성 헤더로 줄이며 RMC3000 안내를 빌드에 붙인다(7절). 소비 코드는 고치지 않는다 | 전체 시험 통과, descriptor 바이트 동일, 메타 컴파일러 시험 갱신(거부 시험은 `--verify-sources`로 유지), 본문 계약 위반 fixture가 RMC3000 안내와 함께 빌드 실패, GC 벤치마크 |
| GS3 | 9절 재측정, 문서 개정(13절), GSD5·GSD13 재검토(2026-11-21에 재검토를 마쳤다 — 둘 다 하지 않는다, 12절) | 9절 목표와 동작 기준 |

## 12. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 등록 | `.gen.cpp`의 정적 객체가 프로그램 시작 때 등록한다 | 등록 함수를 명시 호출한다(모듈 로더가 부른다) | 정적 라이브러리에서 object가 빠지는 문제와 초기화 순서 의존이 없다 |
| 생성 헤더의 형태 | `GENERATED_BODY()`가 펼치는 매크로 묶음. 원본 헤더가 생성 헤더를 include한다 | `GeneratedType<T>` 특수화. 생성 헤더가 원본 헤더를 include한다 | 기존 구조를 유지한다 |
| 필드 접근 | 필드 기술 데이터(offset·종류)를 공용 코드가 해석한다. 필드마다 코드를 만들지 않는다(`UhtProperty.cs:1682, 1936`) | 필드마다 타입 있는 thunk를 만든다 | 기존 구조다. 생성 코드가 큰 원인이며(fixture 모듈 object 54 MB), 양을 줄이는 일은 지금 하지 않는다(GSD13 — 아래 재검토) |
| 타입 정보 export | 클래스를 export하지 않으면 `MinimalAPI`가 필요하다 | 항상 export한다(BD28) | BuildTargets 결정 |

보류 항목 재검토(2026-11-21): GSD5와 GSD13은 지금 하지 않는다.

근거 수치는 Foundation Debug 검증 빌드의 `--timing` 집계다(타입별 본문 분리와 유니티·공유 PCH가 들어간 뒤, 번역 단위 750개, 컴파일 CPU 시간 합 468.7 s — `750ee9a8`이 된 트리의 검증 빌드). 따로 측정 빌드를 돌리지 않았다. 한 번 잰 값이고 기기 부하의 영향을 받는다.

| 구분 | 번역 단위 | CPU 시간 | 비율 |
|---|---|---|---|
| 손으로 쓴 소스 묶음(구성원 655개) | 67 | 139.7 s | 29.8% |
| 외부 라이브러리 | 190 | 141.1 s | 30.1% |
| 공개 헤더 단독 컴파일 검사 | 417 | 88.7 s | 18.9% |
| **리플렉션 생성 소스 묶음(생성 소스 40개)** | 12 | 66.9 s | 14.3% |
| — 시험 fixture 모듈(`ReflectionTestFixtures` 51.5, `ConfigTestFixtures` 3.2, `CurvesTestFixtures` 3.0) | 8 | 57.7 s | 12.3% |
| — 엔진 모듈(`CoreTypeBindings` 3.5, `Curves` 2.5, `Config` 2.2, `TraceControl` 0.9) | 4 | 9.1 s | 1.9% |
| 단독 소스·생성 TU·PCH | 64 | 32.4 s | 6.9% |

**GSD13(생성 코드의 양 줄이기) — 하지 않는다**
- 생성 소스 하나는 평균 1.67 s로 손으로 쓴 소스(0.21 s)의 8배다. 필드마다 타입 있는 thunk를 만드는 구조의 비용이 맞다.
- 그러나 그 비용의 86%(57.7 s)가 시험 fixture 모듈이다. 엔진 모듈의 생성 소스는 9.1 s, clean 컴파일 CPU 시간의 1.9%다. 병렬 빌드의 벽시계로는 1–2 s다.
- 타입별 본문 분리(GS2) 뒤로 본문은 한 번만 컴파일된다. 생성 소스는 그 리플렉션 헤더나 codec 헤더가 바뀔 때만 다시 컴파일되므로 증분 빌드에는 거의 듣지 않는다.
- 공용 codec으로 바꾸는 일은 범위가 크다: 메타 컴파일러의 생성 코드, 필드 codec 헤더 전부(`Object/*FieldCodec.h`), 값 복사·검증·경로 접근, 직렬화 경로가 함께 바뀐다. 타입 있는 thunk의 직접 호출이 값 종류 분기로 바뀌어 실행 시간 비용도 다시 재야 한다. 리플렉션 기반(B9)이 아직 진행 중이라 그 위에서 구조를 바꾸면 양쪽 작업이 엇갈린다.
- 다시 볼 조건(하나라도 맞으면):
  - 엔진 모듈(fixture 제외)의 생성 소스 묶음이 clean 컴파일 CPU 시간의 10%를 넘는다(`--timing` 집계로 본다). EngineTests 타깃은 지금 생성 소스가 68개이고 그중 fixture가 아닌 것이 30개 안팎이다.
  - 생성 소스의 object 크기 때문에 링크 시간이나 Release 바이너리 크기가 문제가 된다.
  - 리플렉션 기반(B9)이 끝나 codec 구조를 바꿔도 다른 작업과 엇갈리지 않는다.
- 그 전에 fixture 비용이 거슬리면 구조를 바꾸지 않고 줄일 수 있다: fixture 헤더를 더 잘게 나누면(지금 헤더 25개에 타입 189개) 헤더 하나를 고칠 때 다시 컴파일되는 양이 준다. 이것은 fixture를 가진 작업의 판단에 맡긴다.

**GSD5(생성 헤더의 include 줄이기) — 하지 않는다**
- codec 헤더 사슬(`Object/GeneratedFieldCodec.h` 기준)은 이 빌드에서 78번 파싱되고 한 번에 0.20 s, 합 15.7 s다. 유니티와 공유 PCH가 들어가면서 소비 번역 단위가 묶음으로 줄어 횟수가 작아졌다(개정 2 때는 번역 단위마다 0.3–0.5 s였다).
- 78번 가운데 12번은 생성 소스 묶음이다. 본문이 codec을 쓰므로 줄일 수 없다. 37번(7.4 s)은 공개 헤더 단독 컴파일 검사, 29번(5.9 s)은 소비 묶음이다. 줄일 수 있는 몫은 많아야 13.3 s, clean 컴파일 CPU 시간의 2.8%다(병렬 빌드의 벽시계로 1–2 s).
- 그 대가는 그대로다: `Object/ValueCopy.h`(지금 `GeneratedFieldAccess.h`·`RuntimeTypeOps.h`를 include한다)와 Trace 보조 함수(`ArrayFieldCodec.h`의 `TraceArrayField`, `MapFieldCodec.h`의 `TraceMapField`, 이들이 쓰는 `GeneratedFieldAccess.h`의 concept과 `RuntimeTypeOps.h`의 `TraceVisitor`)를 가벼운 헤더로 나눠야 하고, 생성 헤더가 끌어오던 include에 기대는 소비 코드를 고쳐야 한다.
- 다시 볼 조건: codec 헤더 사슬의 파싱 합이 clean 컴파일 CPU 시간의 10%를 넘거나, 소비 번역 단위가 유니티·PCH 없이 컴파일되는 구성(검증 변형이 아닌 평소 빌드)이 생긴다.

## 13. 다른 문서 개정

이번 개정에서 고친 것:
- [빌드 타깃](BuildTargets/README.ko.md) BD28(3.8절 본문과 결정 표): 생성 코드 분리의 구현 스펙이 이 문서이고, 생성 소스 단위가 "헤더마다 하나 + 모듈마다 하나"임을 적었다.
- [유니티 빌드·PCH](UnityBuildPch.ko.md) 1절 "생성 TU" 행: 리플렉션 생성 소스가 생긴다는 것과 이 문서 링크를 적었다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절: 이 문서 링크를 적었다.

구현할 때 고칠 것:
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.1절(배치에 `Source/` 추가), 1.2절(검증 컴파일 — 7절), 집계 헤더 서술(모듈 생성 헤더는 선언만): GS1. struct·enum 절의 "생성 헤더가 … 낸다" 서술: GS2.
- [유니티 빌드·PCH](UnityBuildPch.ko.md) UD5·16절 "생성 코드 묶기": 리플렉션 생성 소스를 자기들끼리 묶을지 UB1에서 정해 반영한다.
- [모듈 로더](ModuleLoader/03-Design/03.10-ML2Wiring.ko.md) 3.10절(ML2): 생성 함수의 선언·정의 위치를 모듈 생성 헤더·소스로, 빌드 생성 TU `<M>.ModuleTypes.cpp`와 inline OwnTypes 안을 대체(모듈 로더 세션이 고친다).
- `Programs/Rocks.Build/README.ko.md`: 리플렉션 생성 소스 목록, 소스 없던 리플렉션 모듈의 archive, action 소요 시간 기록.
- `Programs/Rocks.MetaCompiler` 문서·도움말: 출력 목록, `--verify-sources`, 검증 범위.

## 14. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-19 | 생성 코드 분리 설계. 측정(1.2절), 멤버별 위치(5.1절), 배치와 빌드 연결(6절), 검증 컴파일 변경(7절), 결정 GSD1–GSD14(GSD13 보류). 등록 함수 분리(GS1)를 먼저 하기로 하고 착수했다(사용자 결정). 모듈 로더 ML2와 조율해 리플렉션 생성 소스를 별도 목록으로 나르고 진입점 판정에서 뺐다 |
| 2 | 2026-11-19 | 리뷰어 교차 검증 반영. 시제품(기존 생성 헤더 35개를 나눠 컴파일)과 조용한 상태 측정을 1.2절에 더했다. 생성 헤더의 include 줄이기를 보류로 돌리고(GSD5) 런타임 준비 단계를 없앴다 — 이득이 TU당 0.3–0.5초이고 소비 TU 17개가 깨진다. 그래서 타입별 본문 분리(GS2)는 소비 코드를 고치지 않는다. 클래스 `Trace()`도 inline으로 남긴다(native 파생 GC 경로, GSD3). 메타 컴파일러 옵션 `--verify-sources`를 더했다(GSD8, 도구 시험의 거부 보장 유지). 증분 재컴파일 기준을 UB0 뒤로 조건을 달았다. `CopyValueImpl` 이름, 완전한 반환 타입 규칙, private 모듈 생성 헤더, 인용·수치 정정(identity 명세는 생성이 쓰지 않는다, BuildTargets 3.4절, 소스 없는 모듈 6개, 집계 소비 파일 41개). 단계 GS0–GS3 |
| 2(구현 기록) | 2026-11-20 | GS0·GS1 구현과 측정(9절), engine-runtime 병합. 스펙에 없던 것 둘을 구현하며 정했다. 첫째, 설치 SDK가 실어 가는 "리플렉션 생성 소스만으로 된 라이브러리"에 표지 `ReflectionOnlyLibrary`를 붙였다(SDK manifest 7) — 게임 빌드의 모듈 진입점 판정은 미리 빌드한 라이브러리를 코드로 세므로, 표지가 없으면 CoreTypeBindings 같은 모듈이 SDK를 거치며 진입점 모듈이 된다(6.3절의 면제를 SDK에서도 유지). 둘째, MetaCompiler 옵션 `--verify-sources`(빌드 없이 쓰는 단독 실행·도구 시험이 모듈 생성 소스도 게시 전에 구문 검사한다 — 빌드는 넘기지 않는다) |
| 2(구현 기록 2) | 2026-11-20 | GS2 구현. 타입별 생성 헤더는 선언·형질·`Id()`·enum `IsDeclared()`·inline `Trace()`만 남기고 본문을 `Source/Public|Private/<철자>.generated.cpp`로 옮겼다(5.1–5.4절 그대로, include 목록 불변, 소비 코드 수정 없음). 구현하며 정한 점 넷. 첫째, 안내문의 "다시 돌리는 명령"(7절)은 MetaCompiler 인자 전체가 아니라 빌드 명령 옵션 `--verify-metadata-sources on`(build·test·sdk·game)으로 했다 — MetaCompiler 인자는 stage 경로를 담아 빌드 뒤에는 그대로 다시 돌릴 수 없고, 옵션은 같은 명령에 덧붙이면 메타 단계가 `--verify-sources`로 게시 전에 원본 위치와 함께 거부한다. 안내문은 `NativeModuleInput.ReflectionSourceFailureNote`(표시 전용, action key 밖)로 싣고 리플렉션 생성 소스의 compile action 실패에만 붙는다. 둘째, 게시 전 검증 컴파일을 생성 헤더까지 포함해 병렬로 돌린다(MetaCompiler `--jobs`, 빌드가 자기 `--jobs`를 넘긴다, 실패 보고는 생성물 순서에서 가장 앞선 것) — 7절은 `--verify-sources`의 생성 소스만 병렬이라고 적었으나 헤더 검증도 생성물마다 독립이다. 셋째, 설치 SDK manifest를 8로 올렸다(칸은 같다) — 이전 SDK의 생성 헤더는 본문을 inline으로 정의하고 동봉 MetaCompiler가 타입별 생성 소스를 내지 않아 새 빌드 도구와 맞지 않는다. 넷째, 도구 시험은 생성 코드의 문구를 헤더+생성 소스에서 읽고(본문을 고쳐 컴파일 실패를 보는 시험은 생성 소스를 고친다), 생성 static_assert probe는 생성 소스까지 한 번역 단위로 컴파일한다. 전체 빌드·시험과 GC 벤치마크는 단계별로 돌리지 않고 UB 단계까지 끝낸 뒤 한 번 검증한다(2026-11-20 사용자 결정) |
| 3(사실 정정) | 2026-11-20 | 모듈 로더 ML2 반영([모듈 로더](ModuleLoader/03-Design/03.10-ML2Wiring.ko.md) 3.10절 "GS1 위 세부"). 진입점 판정이 리플렉션 생성 소스를 세게 되어 GSD14의 진입점 면제를 거뒀고, 손으로 쓴 소스가 없는 메타데이터 모듈은 빌드가 만든 진입점 TU를 받는다. 개정 2(구현 기록)의 설치 SDK 표지 `ReflectionOnlyLibrary`는 없앴고 SDK manifest는 8이다. 모듈 생성 소스는 자기 타입 함수 `RegisterModuleOwnTypes`도 정의한다(5.5절 등록 함수 세 개) |
| 3(병합 기록) | 2026-11-20 | 모듈 로더 ML2(engine-runtime f5b448c8)와 GS2·유니티 PCH 작업을 병합했다. 설치 SDK manifest 8은 ML2의 `ReflectionOnlyLibrary` 제거가 가졌고, GS2(타입별 본문이 SDK 라이브러리에 있음)와 공유 PCH 헤더 칸은 함께 9다("2(구현 기록 2)"의 "manifest 8"은 병합 전 번호다). 모듈 정의가 정의 헤더로 가므로([유니티 빌드·PCH](UnityBuildPch.ko.md) 8.2절) ML2의 `ROCKS_MODULE_TYPES`도 `ROCKS_MODULE_NAME`과 함께 정의 헤더에 들어가고, AST 생성과 native compile이 같은 헤더를 받는다. ML2가 모듈 생성 소스 끝에 더한 `RegisterModuleOwnTypes`는 그대로다 |
| 3(검증 기록) | 2026-11-20 | GS2의 첫 전체 빌드와 시험을 유니티·PCH 작업 끝의 검증에서 함께 돌렸다([유니티 빌드·PCH](UnityBuildPch.ko.md) 개정 이력 "검증 기록"). Foundation·EngineTests Debug 빌드와 시험(2,549개·2,954개), 도구 시험 meta 246건, 통합 시험(설치 SDK 게임, Release ThinLTO SDK·게임 포함) 통과, 무변경 재빌드 0 action. 통합 시험 한 곳이 GS2에 맞지 않아 고쳤다 — 필드를 더해도 타입별 생성 헤더는 그대로이므로 게시 여부를 생성 소스와 descriptor로 확인한다. 수치(Foundation Debug clean build, 유니티·PCH를 끈 검증 변형이라 생성 소스 분리의 몫만 보인다): compile 시간 합 5,799.5 s → 752.8 s(1,278 → 1,337 action), 시험 실행 파일 3,970.6 s(386 TU) → 262.1 s(390 TU), 공개 헤더 단독 컴파일 검사 693.6 s → 151.0 s, 리플렉션 fixture 모듈의 생성 소스 26 TU 58.2 s. native 단계 벽시계는 776.5 s → 103.3 s다(장비 부하가 달라 거친 비교다) |
| 4 | 2026-11-21 | 8.1절을 더했다(GSD15·GSD16): 생성 헤더가 보이지 않는 번역 단위에서 템플릿이 오류 없이 다른 길로 가는 지점을 조사하고, `ROCKS_GENERATED_BODY()`의 표지와 탐지 관문으로 컴파일 오류가 되게 했다. 표지를 위해 저장소의 리플렉션 클래스·struct에 `ROCKS_GENERATED_BODY()`를 넣었다(동작은 그대로다) |
| 4(리뷰 반영) | 2026-11-21 | 8.1절 보강: 판정을 cv 한정자를 뗀 타입으로 하고 한정자 붙은 타입·불완전 타입은 관문이 거부, 파서가 두 표지를 함께 요구, 로그 인자에도 생성 헤더가 필요함, 유니티 묶음에서 놓치는 경우와 그것을 잡는 검증 변형, 리플렉션 헤더의 inline 코드 한계, 매크로 이름이 박힌 곳 목록. 관문마다 따로 실패를 확인하는 시험을 더했다 |
| 5 | 2026-11-21 | 보류하던 GSD5·GSD13을 재검토해 둘 다 하지 않기로 했다(12절). Foundation Debug 검증 빌드의 `--timing` 집계로, 리플렉션 생성 소스 묶음은 컴파일 CPU 시간의 14.3%지만 그 86%가 시험 fixture 모듈이고 엔진 모듈은 1.9%다. codec 헤더 사슬에서 줄일 수 있는 몫은 2.8% 이하다. 다시 볼 조건을 12절에 적었다 |
| 6 | 2026-11-26 | 6.3절: MetaCompiler 인자를 응답 파일(stage의 `<모듈>.metacompiler.rsp`, `@<경로>`)로 넘긴다. 헤더가 많은 모듈이 Windows 명령줄 상한을 넘었다(MaterialSchema MS1b) |
