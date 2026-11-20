# Clang 버전 지원 설계

기준일: 2026-11-20, 개정 6. Clang 22가 JSON AST에서 `ElaboratedType`을 없애, MetaCompiler가 읽던 타입 철자가 바뀌었다. 이 문서는 두 가지를 정한다.
- **정규 형식 탐침**: MetaCompiler가 타입 철자를 첫 실행 JSON의 `qualType`·`desugaredQualType`에서 읽지 않고, Clang이 출력한 정규 형식(완전 한정 철자)으로 읽게 한다. 그러면 Clang 21과 22의 AST 모양 차이가 철자에 닿지 않는다.
- **도구 모음 버전 정책**: 검증한 LLVM 버전, `ROCKS_MSVC_TOOLSET`, `Build/InstallLlvm.ps1`, CI, 그리고 MSVC 14.44 고정을 언제 푸는지.

배경:
- GitHub `windows-latest` 이미지(windows-2025-vs2026, 20261116)가 MSVC 14.52로 올라갔다. 그 STL은 Clang 22 미만을 `STL1000`으로 거부하고, 이미지의 LLVM은 20.1.8이다.
- a0704772가 CI를 MSVC 14.44(`ROCKS_MSVC_TOOLSET` → VsDevCmd `-vcvars_ver`)와 LLVM 21.1.6(`Build/InstallLlvm.ps1`)으로 고정해 피했다. LLVM 22.1.8로 올린 CI 실행은 MetaCompiler에서 `DeviceProfileSettings.h:37: error RMC2000: rocks::config::DeviceCondition::trait: unsupported field type 'String'`으로 멈췄다.
- macOS는 Apple clang 17.0.0(Xcode 16, macOS 15 이미지), Android는 NDK r29 clang(`clang version 20.0.0`, r547379)을 쓴다. 이 둘은 Clang 22보다 오래된 AST 모양이다. 그래서 한동안 두 모양을 함께 받아야 한다.

다른 문서와의 관계:
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.3절(합성 번역 단위), 2.4절(NATIVE 재귀 검사), 4.6절(Binds 철자 해석)의 구현 방식을 바꾼다. 규칙(무엇을 거부하는지)은 바꾸지 않는다.
- [리플렉션 확장](ReflectionExtensions.ko.md) 8절(정적 배열 탐침)·9.2절(Within 해석)의 탐침을 이 문서의 탐침으로 합친다.
- [빌드 안내](../../Build/README.md)의 요구 사항 절과 `.github/workflows/foundation.yml`의 도구 모음 고정을 이 문서 6절이 정한다.
- 새 Clang의 경고는 [컴파일러 경고 정책](CompilerDiagnostics.ko.md)대로 끄지 않고 고친다(7절).

측정 도구 모음: LLVM 21.1.6·22.1.8 공식 Windows x64 배포(설치 파일을 풀어 씀), 로컬 MSVC 14.29(Visual Studio 2019 STL), clang-format 21.1.6. UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 UnrealBuildTool·UnrealHeaderTool이다. UE 이름은 12절 대응표에만 적는다.

상태: **구현 중**(결정 CLGD1–CLGD15, 권장안으로 확정). 구현 단계는 CLG0–CLG4(10절)다. CLG0–CLG2는 engine-runtime에 병합했고(53bce26c), 개정 4는 CLG1·CLG2 리뷰를 레코드 닫힘(4.4절)과 base 식별자(4.3절)에 반영했다(7ee2b0d5). CLG3을 구현하고(c4c0a535) 리뷰를 반영했다(개정 6). 구현하며 정한 점은 14절 개정 3·5·6이다. 시제품(커밋하지 않음)으로 실제 헤더에서 확인한 측정은 1.4절이다. 개정 2는 리뷰어 교차 검증(Clang 21.1.6·22.1.8 양쪽 탐침)을 적용했다.

## 1. 현재 상태와 측정

### 1.1 Clang 22의 JSON 변화

Clang 22는 `ElaboratedType`(한정·키워드를 감싸던 sugar 노드)을 없애고, 한정을 `TagType`·`TypedefType` 자체에 담는다. 그래서 한정 없이 쓴 비템플릿 레코드·enum은 desugar할 sugar가 없어 `desugaredQualType` 키가 빠진다. 같은 헤더를 두 버전으로 덤프한 결과다(`-Xclang -ast-dump=json`).

| 선언 | Clang 21.1.6 `type` | Clang 22.1.8 `type` |
|---|---|---|
| `String s;`(이름공간 `rocks` 안의 비템플릿 struct, 한정 없이) | `qualType: String`, `desugaredQualType: rocks::String` | `qualType: String` |
| `rocks::String q;` | `qualType: rocks::String` | 같다 |
| `::rocks::String g;` | `qualType: ::rocks::String`, `desugaredQualType: rocks::String` | `qualType: ::rocks::String` |
| `Mode m;`(enum), `Nested n;`(클래스 안 타입), `Holder h;` | 정규 이름으로 desugar | `qualType`만, 쓴 그대로 |
| `using Alias = String;` 뒤 `Alias a;` | `desugaredQualType: rocks::String` | `desugaredQualType: String` |
| `uint32 u;`(typedef → builtin) | `desugaredQualType: unsigned int` | 같다 |
| `Array<String> arr;`(템플릿 특수화) | `desugaredQualType: rocks::Array<rocks::String>` | 같다 |
| `uint32* up;`, `String* sp;`(포인터) | `uint32 *`, `String *`(desugar 없음) | 같다 |
| base `struct Thing : Object` | `qualType: Object`, `desugaredQualType: rocks::Object` | `qualType: Object` |
| base `struct D : ObjectAlias`(별칭) | `typeAliasDeclId` 있음, desugar `rocks::Object` | `typeAliasDeclId` 있음, desugar `Object` |
| 정규 형식(`-fdump-record-layouts-canonical`) | `struct rocks::String` | 같다 |

JSON 덤퍼가 내는 형식 키는 `qualType`·`desugaredQualType`·`typeAliasDeclId`뿐이고, 정규·완전 한정 철자를 내게 하는 cc1 옵션은 없다. `-ast-dump-decl-types`는 타입 선언(`TypeDecl`)의 형식만 덧붙이고 필드 형식은 덧붙이지 않는다(실측). 반면 템플릿 인자는 두 버전 모두 정규 형식으로 출력된다. 1.4절의 탐침은 이 점을 쓴다.

포인터 행이 보이듯, 포인터·참조·배열처럼 sugar가 아닌 겉 형식은 **Clang 21에서도** 안쪽 이름을 쓴 그대로 낸다. 그래서 아래 1.2절의 조용한 실패 일부(중첩 레코드의 `World*`)는 Clang 21에서도 이미 있었다.

### 1.2 MetaCompiler가 기대는 곳

원 위치는 origin/feature/engine-runtime(3794978d) 기준이다.

| 위치 | 읽는 것 | Clang 22에서 | 실패 모양 |
|---|---|---|---|
| `ClangAstParser.TypeSpelling`(`ClangAstParser.cs:545`) | `desugaredQualType`, 없으면 `qualType` | 비템플릿 레코드·enum이 쓴 그대로 나온다 | 아래 소비자마다 다르다 |
| PROPERTY 필드(`:953` → `SchemaValidator` `Scalars`·`ValueType`) | 정규 이름 표(`rocks::String` 등)·리플렉션 타입 정확 일치 | `String`·`Name`·리플렉션 struct·enum이 표에 없다 | **시끄러움**: RMC2000 unsupported field type |
| 클래스 base(`:799` → `SchemaValidator.ResolveType` `:168`) | base 철자, 둘러싼 이름공간 순 탐색 | `Object`가 `rocks::Object`로 풀리지 않는다(특별 취급이 정확 일치뿐) | **시끄러움**: RMC2003 |
| struct base(`:818` → `StructBase` `:523`, `EmptyBases` `:575`) | base 철자 | 리플렉션 base는 이름공간 탐색으로 찾지만, 비리플렉션 빈 base는 쓴 그대로 생성 코드 `IsEmptyV<::철자>`에 들어간다(`CppEmitter.cs:801`) | 생성 소스 컴파일 오류, 같은 이름의 전역 타입이 있으면 **다른 타입을 검사** |
| NATIVE 멤버(`:928` → `StrongReferencePath`·`RequiresWorldOnlyOwnerPath`, `ClangAstParser.Traits.cs:23·103`) | 레코드 색인(정규 이름 열쇠)에서 필드·base를 따라간다 | `ObjectHandle`·`TickSettings` 같은 철자가 색인에 없어 재귀가 멈춘다 | **조용함**: RMC2205(강한 참조)·RMC2206(WorldOnly) 누락 |
| NATIVE struct(`SchemaValidator.cs:696`) | 직접 형식이 리플렉션 struct인지 | `StructLeaf`를 못 찾아 `native.Struct`가 비고, 생성 코드의 2차 확인 `static_assert`가 빠진다 | **조용함** |
| 레코드 색인 필드·base(`:148`·`:163`) | 위 재귀가 쓰는 철자 | 중첩 필드 `String`·`SlotHandle` 등이 쓴 그대로 | **조용함**. 포인터(`Allocator *`)는 Clang 21에서도 같다 |
| `IsObjectDerived`(`Traits.cs:8`) | base 철자 사슬이 `rocks::Object`에 닿는지 | base가 쓴 그대로라 사슬이 끊긴다 | **조용함**: 중첩 `Object*` 누락 |
| Binds·Within 탐침(`CompilerDriver.cs:752`·`:687`) | `using rocksBinds_N = ::철자;`의 desugar | 비템플릿 타입을 가리키는 별칭을 거친 철자는 별칭 선언에 쓴 그대로 나온다(`ns::Alias`, `using Alias = Plain;` → `Plain`). 템플릿 특수화를 가리키는 별칭(`rocks::Vec3`)은 그대로다 | 미러 대조 실패(시끄러움). 실제 헤더에는 해당 철자가 없다(1.3절) |
| Binds 미러 필드(`CppEmitter.cs:743`) | 필드 철자를 생성 `IsSameV<…, 철자>`에 넣는다 | 쓴 그대로의 이름이 생성 문맥에서 안 보일 수 있다 | 생성 소스 컴파일 오류 |
| 배열 원소 진단(`CompilerDriver.cs` `ArrayElementFailure`, `SourceChecks.cs:569`) | 실패한 첫 실행의 철자·강한 참조 경로 | 철자·경로가 덜 나온다 | 진단 문구만 나빠진다 |
| enum 바탕 형식(`ClangAstParser.Enums.cs:59`) | `fixedUnderlyingType`의 desugar | typedef → builtin이라 그대로다 | 영향 없음 |
| Validate hook 철자(`:382`), `currentAliases`(`:871`) | 함수 형식 `qualType`·별칭 `qualType`, 한정 선택 정규식 | 두 버전 모두 쓴 그대로라 그대로다 | 영향 없음(생성 `static_assert`가 정확히 대조) |
| GENERATED_BODY 표지(`SourceChecks.cs:193`) | `void () noexcept` | 그대로 | 영향 없음 |
| StaticArrayProbe(`StaticArrayProbe.cs:108`), NativeMemberProbe(`NativeMemberProbe.cs:438`) | 탐침 변수 형식의 desugar(템플릿 인자) | 템플릿 인자는 정규 형식이라 그대로다 | 영향 없음 |

그 밖의 도구:
- Rocks.Build는 Clang JSON AST를 읽지 않는다. `--version` 첫 줄(`clang version 22.1.8 (…)`)은 `NativeBuildModels.cs:170`의 정규화로 그대로 다뤄진다. `-ftime-trace` 이벤트 이름(`Total ExecuteCompiler`·`Source`·`InstantiateClass`, `TimingReport.cs`)은 22.1.8에도 있다(실측).
- 컴파일러 버전을 확인하는 gate는 **없다**. 고정 버전 확인은 clang-format뿐이다(`FormatCheck.RequiredVersion = "21.1.6"`, MetaCompiler RMC3100 `CompilerDriver.cs:234`). LLVM 버전은 `InstallLlvm.ps1`이 설치할 때만 확인한다.

### 1.3 실측: 고치지 않은 MetaCompiler

EngineTests Debug 대상의 메타데이터 모듈 11개(AssetRuntime, AssetTestFixtures, Config, ConfigTestFixtures, CoreTypeBindings, Curves, CurvesTestFixtures, ReflectionTestFixtures, TraceControl, World, WorldTestFixtures)에서, MetaCompiler가 읽은 철자를 1.4절 탐침의 정규 철자와 대조했다. C 배열은 Clang 21에서도 정적 배열 탐침이 따로 정규화하므로 빼고 센다.

| 종류 | 개수 | Clang 21에서 다른 것 | Clang 22에서 다른 것 | 고치지 않으면 |
|---|---|---|---|---|
| PROPERTY 필드 | 1,134 | 0 | 261(`String` 36, `Name` 12, `BulkDataRef` 9, `Text`·`Guid`·`AssetId` 각 7 …) | RMC2000 |
| 클래스 base | 104 | 0 | 103(`Object` 56 …) | RMC2003 |
| struct base | 15 | 0 | 14 | 리플렉션 base는 이름공간 탐색으로 통과, 비리플렉션 빈 base(`InheritMarker`)는 생성 `IsEmptyV<::InheritMarker>`가 컴파일 오류(Clang 21은 `::rocks::reflection_fixture::InheritMarker`) |
| NATIVE 멤버 | 33 | 3(`World *` — 포인터) | 14 | RMC2205·2206 누락(조용함) |
| NATIVE 재귀 레코드 필드 | 26 | 1(`Allocator *`) | 6 | 누락(조용함) |
| Binds·Within | 28·1 | 0 | 0 | — |

Clang 22로 Foundation을 그대로 빌드하면 Config 모듈의 첫 RMC2000에서 멈춘다(로컬 재현, CI와 같은 메시지). 시끄러운 실패(RMC2000·2003)가 먼저 나므로 조용한 실패는 드러나지 않는다. PROPERTY만 고치는 부분 수정은 NATIVE 재귀 누락(예: `InstancedWorldBound::token`의 WorldOnly 형질, `StructOwner::scratch`의 강한 참조 검사)을 남긴다.

도구 시험 `meta`(246건)를 고치지 않은 MetaCompiler와 Clang 22.1.8로 돌리면 60건이 실패한다. 4건(`Identity*` 셋, `PropertyDirectPrefixAndSourceBoundaries`)은 시험 하네스가 저장소 밖에 둔 MetaCompiler 사본에서 include 경로를 못 찾은 것이라 빼고, 나머지 56건은 아래와 같다.

| 실패 모양 | 건수 | 예 |
|---|---|---|
| RMC2000 unsupported field type(한정 없는 레코드·enum·리플렉션 struct·Binds 실제 타입) | 43 | `EnumSchemaReachesDescriptorAndGeneratedCode`, `BindsResolveAliasesAndReflectTheRealType`, `EveryValueKindSpellingIsAcceptedWhereAllowed` |
| 여러 모듈 시험에서 아래 모듈이 위의 RMC2000으로 먼저 실패 | 6 | `ImportedStructCycleIsRejected`, `TypeToolNamesAreUniqueAmongImportedModules` |
| RMC2003(`Object` base) | 2 | `ModuleNameMayEqualAReflectedClassName` |
| 비리플렉션 빈 base의 생성 코드 컴파일 오류(`no member named 'Shared' in the global namespace`) | 1 | `StructNonReflectedBaseMustBeEmpty` |
| MetaCompiler가 놓친 WorldOnly 형질을 생성 `static_assert`가 대신 잡음(진단 코드가 RMC3000으로 바뀜) | 1 | `ArrayWorldOnlyElementNeedsWorldOnlyClass` |
| **거부해야 할 입력을 받아들임**(조용함) | 3 | `NativeMembersCannotHoldStrongReferences`, `NativeReferenceStructIsRejected`, `WorldOnlyOwnerTraitNeedsWorldOnlyClass` |

마지막 행이 GC 안전 검사(NATIVE가 강한 참조를 담지 못하게 하는 RMC2205, WorldOnly 소유자 RMC2206)의 구멍이다. 생성 코드의 2차 확인은 직접 형식만 보므로 중첩 레코드의 경우를 잡지 못한다.

### 1.4 실측: 정규 형식 탐침 시제품

4절 방식의 시제품(필드·NATIVE·base·Binds·Within·NATIVE 재귀 레코드, 커밋하지 않음)으로 같은 대상을 빌드했다.

| 확인 | 결과 |
|---|---|
| Clang 21.1.6 + 시제품의 생성물(descriptor 11개, `Generated`·`GeneratedSources` 전체) | 고치지 않은 MetaCompiler와 **byte 동일**. 빌드 888 action 모두 캐시 적중 |
| Clang 22.1.8 + 시제품의 생성물 | Clang 21 기준선과 **byte 동일** |
| 탐침이 돌려준 정규 철자(필드 1,134·base 119·NATIVE 33·레코드 26·Binds 28·Within 1) | 모듈 11개 모두 Clang 21과 22가 **같다** |
| private 필드·private base·별칭 base·클래스 안 타입·enum·Binds(템플릿 별칭)·Within | 모두 정규 철자로 나왔다(작은 시험 파일과 실제 fixture 모듈) |
| Clang 22 EngineTests Debug 빌드 | 888 action 실행, **경고·오류 0**(MSVC 14.29 STL) |
| Foundation Debug 빌드·시험(Clang 22) | 7절 |
| 도구 시험 `meta` 246건(Clang 22.1.8) | 246건 통과(시제품 첫 판은 base 이름 두 경우에서 2건 실패 — 4.3절. 시제품의 분석용 기록 파일이 병렬 시험끼리 겹친 1건은 따로 돌려 통과) |
| 도구 시험 `meta` 246건(Clang 21.1.6) | 246건 통과(분석용 기록 파일이 겹친 4건은 따로 돌려 통과). 정규 철자로 바뀐 진단 문구 때문에 기대값을 고칠 시험은 없었다 |
| 탐침 실행 수 | 모듈마다 1회 + NATIVE 재귀 라운드 0–2회(AssetRuntime·ReflectionTestFixtures·World 2회, WorldTestFixtures 1회) |
| 탐침 비용 | 4.8절 |

Clang 21에서 시제품과 기존 철자가 달랐던 곳은 C 배열(정적 배열 탐침이 이미 정규화하던 것)과 포인터 NATIVE·레코드 필드(`World *` → `rocks::World *`)뿐이다. 앞의 것은 결과가 같고, 뒤의 것은 1.2절의 Clang 21에서도 있던 조용한 누락을 메운다.

## 2. 용어

| Rocks | Clang | 뜻 |
|---|---|---|
| 정규 형식·정규 철자 | canonical type, `QualType::getCanonicalType` 출력 | sugar(typedef·별칭·한정 표기)를 모두 벗기고 이름을 완전 한정한 형식. 두 버전의 출력이 같다 |
| 쓴 그대로의 철자 | as-written `qualType` | 소스에 적힌 대로의 철자 |
| 첫 실행 | — | 리플렉션 헤더 전부를 include한 합성 번역 단위의 `-ast-dump=json` 실행(ReflectionFoundations 1.3절) |
| 정규 형식 탐침 | — | 첫 실행과 같은 번역 단위 끝에 질문을 붙여 Clang을 다시 실행하고 정규 철자를 읽는 실행(4절) |
| 질문 | — | 탐침 한 줄. 멤버 형식, 형식 철자, base, 사실(0·1) 중 하나 |
| 레코드 닫힘 | — | NATIVE 재귀 검사가 닿는 레코드의 필드·base를 차례로 묻는 라운드(4.4절) |
| 특수화 노드 | `ClassTemplateSpecializationDecl` | 첫 실행 JSON에 있는 템플릿 특수화(암시적 인스턴스화, 부분·명시 특수화)의 노드. 템플릿 인자와 실제 필드·base(치환된 형식)를 담는다 |
| 주입 클래스 이름 | injected-class-name | 클래스가 자기 범위에 넣는 자기 이름. 파생 클래스로 상속되므로 `::D::B`가 base B를 가리킨다 |
| 명시적 인스턴스화 | explicit instantiation | `template struct X<…>;`. 이 선언의 이름은 접근 검사를 받지 않는다([temp.spec.general]) |

## 3. 목표와 범위

목표:
- MetaCompiler가 Clang 21과 22(그리고 Apple clang 17, NDK r29 clang)를 함께 받는다. 같은 입력이면 어느 버전으로도 같은 byte의 생성물을 낸다.
- 타입 철자 해석을 Clang에 맡긴다. C#이 C++ 이름 조회를 흉내 내지 않는다.
- 조용한 실패를 없앤다. 해석할 수 없는 형식은 진단으로 거부한다.
- 검증한 도구 모음과 그 버전을 한 곳에서 정하고, 빌드가 확인한다.

범위 밖:
- 리플렉션 규칙(지원 형식, 거부 조건)의 변경.
- Clang 23 이후 지원. 정책(6절)만 정한다.
- 경고 정책 자체: [컴파일러 경고 정책](CompilerDiagnostics.ko.md).
- clang-format 버전 변경(CLGD11).

## 4. 설계: 정규 형식 탐침

### 4.1 원칙

형식에 관한 판단(값 종류, 참조 대상, base, NATIVE 재귀, Binds·Within)은 Clang이 출력한 정규 철자로만 한다. 첫 실행 JSON은 선언 구조(노드 종류, 이름, annotate 인자, 위치, 접근, 정의 여부, bitfield 폭, 열거자 값)를 읽는 데 쓰고, 형식 철자는 진단 문구에만 쓴다. 예외는 셋이다: enum 바탕 형식, Validate·표지 시그니처(4.6절), 그리고 base 질문에 쓸 식별자를 base 철자에서 뽑는 일(4.3절 — 답은 Clang이 확인한다).

### 4.2 질문과 탐침 텍스트

탐침은 첫 실행과 같은 인자·번역 단위에 아래 텍스트를 붙이고 `-ast-dump-filter=rocks_meta_canon`·`-w`로 실행한다(기존 탐침과 같은 방식).

```cpp
#include <Base/TypeTraits.h>
namespace rocks_meta_canon {
template <int RocksCanonN, class RocksCanonT>
struct RocksCanonType {};
template <int RocksCanonN, class RocksCanonD, class RocksCanonB>
struct RocksCanonBase {
    static_assert(::rocks::IsBaseOfV<RocksCanonB, RocksCanonD> && !::rocks::IsSameV<RocksCanonB, RocksCanonD>, "rocks canonical base probe");
};
template <int RocksCanonN, int RocksCanonV>
struct RocksCanonFact {};
}
template struct ::rocks_meta_canon::RocksCanonType<0, decltype(::rocks::config::DeviceCondition::trait)>;
template struct ::rocks_meta_canon::RocksCanonBase<1, ::rocks::Actor, struct ::rocks::Actor::Object>;
template struct ::rocks_meta_canon::RocksCanonFact<2, (::rocks::IsBaseOfV<::rocks::Object, ::rocks::TickSettings> ? 1 : 0)>;
```

질문 종류:

| 질문 | 탐침 줄 | 쓰는 곳 |
|---|---|---|
| 멤버 형식 | `RocksCanonType<N, decltype(::Owner::member)>` | PROPERTY 필드, NATIVE 멤버, 레코드 닫힘의 필드 |
| 형식 철자 | `RocksCanonType<N, ::이름<인자…>>`·`RocksCanonType<N, ::ns::v1::X>` | 레코드 닫힘의 색인 열쇠(특수화 노드, inline 이름공간 아래 레코드 — 4.4절) |
| base | `RocksCanonBase<N, ::Owner, struct ::Owner::식별자>`(base의 주입 클래스 이름), 안 되면 둘러싼 이름공간 별칭(4.3절) | 클래스 base, struct base, 레코드 닫힘의 base |
| 사실 | `RocksCanonFact<N, (상수식 ? 1 : 0)>` | 레코드 닫힘의 Object 파생 여부(`IsBaseOfV`), class 여부(`IsClassV`) |
| 별칭 철자 | `RocksCanonType<N, rocksBinds_N>`·`RocksCanonType<N, rocksWithin_N>` | Binds·Within(4.7절) |

읽기: 덤프에서 `ClassTemplateSpecializationDecl`(`RocksCanonType`·`RocksCanonBase`·`RocksCanonFact`)의 `TemplateArgument` 자식을 읽는다. 첫 인자 `value`가 질문 번호이고, 형식 인자의 `type.qualType`이 정규 철자, 사실 인자의 `value`가 0·1이다. 사실을 `bool` 인자로 두지 않는 이유: JSON 덤퍼는 `bool` 템플릿 인자 `true`를 `"value": -1`로 낸다(1비트 부호 있는 값의 확장, 21.1.6·22.1.8 실측). `int`로 0·1을 넘긴다. 같은 특수화가 클래스 템플릿 노드 안과 최상위에 두 번 나오므로 번호로 한 번만 받는다. 답이 없는 번호가 있으면 RMC2001이다.

명시적 인스턴스화를 쓰는 이유:
- 선언의 이름이 접근 검사를 받지 않는다. 그래서 GENERATED_BODY의 friend 없이도 private 멤버, 비리플렉션 레코드의 private 필드, private base에 닿는다(실측: 두 버전 모두 진단 없음). 기존 탐침(StaticArrayProbe·NativeMemberProbe)은 `GeneratedType<Tag>` 특수화 안의 변수로 friend 접근을 빌렸고, 그 방법은 비리플렉션 레코드의 private 필드에 닿지 못한다.
- 템플릿 인자로 들어간 형식은 특수화에 정규 형식으로 저장되므로, 출력이 sugar 표기와 무관하다. 1.1절 표의 마지막 행처럼 정규 출력은 두 버전이 같다.
- 멤버 형식·base 질문은 사용자 템플릿을 새로 인스턴스화하지 않는다. 인스턴스화하는 것은 탐침의 빈 템플릿과 Rocks 형질(`IsBaseOfV`·`IsSameV`·`IsClassV`)뿐이고, `decltype(::Owner::member)`가 가리키는 특수화는 첫 실행에서 이미 완전하다(값으로 담은 멤버이므로). 레코드 닫힘이 재사용하는 판정 형판은 NativeMemberProbe처럼 대상의 완전성을 보며 대상 템플릿을 인스턴스화할 수 있고, 그 실패는 4.5절의 격리가 맡는다.

### 4.3 base 질문

base는 `decltype`으로 이름할 수 없다. 그래서 파생 클래스를 거쳐 base의 주입 클래스 이름을 부르고, 안 되면 base 철자를 둘러싼 이름공간의 별칭으로 부른다.

base 목록과 식별자:
- 리플렉션 타입과 비템플릿 레코드는 첫 실행의 그 레코드 노드에서, 템플릿 특수화는 **특수화 노드**의 `bases`에서 base 목록을 읽는다. 특수화 노드의 base는 치환된 형식이다(`Leaf<I, T>...` → `rocks::Leaf<0, rocks::Payload>`·`rocks::Leaf<1, int>`, `Mixin<T> : T` → `rocks::Payload`, 두 버전 같음).
- 질문에 쓸 식별자는 base `type`의 `TypeSpelling`(desugar 철자 우선)에서 꼬리 템플릿 인자를 걷고 꺾쇠·괄호 깊이 0의 마지막 `::` 뒤를 잘라 얻은 식별자다(`Outer<int>::Inner` → `Inner`, 개정 4 — 첫 `<`에서 자르면 `Outer`가 나와 간접 base를 답으로 받는다). 문자·문자열 리터럴(`ByChar<'>'>`) 안의 글자는 깊이에 세지 않는다. 쓴 철자에서 뽑지 않는 이유: 별칭 템플릿(`BaseAlias<int>`), 특수화 안의 멤버 typedef(`Traits<int>::BaseType`), `decltype` base는 쓴 철자의 마지막 식별자가 base 클래스의 이름이 아니다. desugar 철자에서는 셋 다 `Base`가 나온다(리뷰어 실측, 두 버전 같음). 별칭 base(`ObjectAlias`)도 desugar 철자 `Object`·`rocks::Object`에서 `Object`가 나오므로 개정 1의 별칭 선언 id 색인은 필요 없다. 이 추출이 4.1절의 세 번째 예외다.

순서:
1. **정교한 형식 지정자**로 묻는다: `struct ::Owner::식별자`(`Object` → `struct ::rocks::Actor::Object`, 특수화 소유자는 `struct ::rocks::Mixin<rocks::Payload>::Payload`). 정교한 형식 지정자의 조회는 형식이 아닌 이름을 건너뛰므로 파생 클래스의 같은 이름 데이터 멤버·멤버 함수(`struct Leaf : A { int32 A; }`)에 가려지지 않는다.
2. **둘러싼 이름공간 별칭**으로 묻는다: base 철자(`TypeSpelling` — 특수화의 base는 정규 철자, 그 밖에는 쓴 철자)를 소유자를 둘러싼 이름공간의 별칭으로 부른다(`namespace probe { using rocksCanonAlias_N = rocks::Object; }` 뒤 `::probe::rocksCanonAlias_N`). base 절의 이름 조회 문맥과 같은 이름공간이다(그 뒤에 선언된 이름까지 보이는 차이는 아래 확인이 잡는다). 다음 경우에 쓴다.
   - C#이 미리 아는 경우: 파생 클래스 이름(템플릿이면 템플릿 이름)이 식별자와 같다(`class Object : public rocks::Object`, `template <int N> struct Rec : Rec<N - 1>` — `::Owner::Rec`은 파생 자신을 가리킨다). 한 레코드에 식별자가 같은 base가 둘 이상이다(`a::Node`·`b::Node`, pack 확장 `Leaf<I, T>...` — 1은 "found in multiple base classes"로 실패한다).
   - 1이 어떤 이유로든 실패한 경우(4.5절 격리로 그 질문만 다시 묻는다).
3. 둘 다 실패하면 거부한다: 리플렉션 타입은 RMC2001, 레코드 닫힘은 RMC2205 cannot verify.

확인: `RocksCanonBase`의 `static_assert(IsBaseOfV<B, D> && !IsSameV<B, D>)`. `IsBaseOfV`만으로는 B == D를 받으므로, `struct ::rocks::Rec<2>::Rec`가 Rec<2> 자신을 조용히 답한다(리뷰어 실측). 파생 클래스가 base와 같은 이름의 **중첩 타입**을 선언하면 1의 조회가 그 타입을 찾고 이 확인이 실패해 2로 간다. 1의 답은 마지막 식별자가 물은 식별자와 같을 때만 받는다(개정 4 — 같은 이름의 멤버 typedef가 다른 base를 가리키면 2로 간다).

실측(모두 두 버전 같음):
- 시제품(개정 1 방식 — 쓴 철자의 마지막 식별자, 별칭 id 색인): private base(`private detail::Leaf`), CRTP base, 별칭 base, `::rocks::Object`로 한정한 base, 같은 이름 데이터 멤버에 가려진 base(1), 파생과 같은 이름의 base(2), 중첩 클래스의 바깥 클래스 멤버 base(`struct Outer { struct Inner {}; struct Nested : Inner {}; }` — 1). 시제품 첫 판은 1을 정교한 형식 지정자 없이(`::Owner::A`) 물었고, meta 시험 246건 중 바로 이 두 경우(`Meta.StructInheritedToolNamesAreUnique`, `Meta.GeneratedBodyWorksInReflectedClass`)에서 탐침이 실패했다. 1·2 순서로 고친 뒤 두 시험이 통과했다.
- 작은 시험 파일(개정 2 방식): 별칭 템플릿·멤버 typedef·`decltype` base와 템플릿 매개변수 base(`Mixin<Payload> : Payload`)는 1로, pack 확장 base(`Storage<Seq<0, 1>, Payload, int> : Leaf<0, Payload>, Leaf<1, int>`)와 `Rec<2> : Rec<1>`은 2로 정규 철자가 나왔다. 개정 2 방식은 시제품에 넣지 않았다.

정규 base가 들어오므로 `SchemaValidator.ResolveType`·`StructBase`의 둘러싼 이름공간 순 탐색(근사 이름 조회)을 지운다. 리플렉션 클래스 base는 `classes`·`imported`·`rocks::Object`의 정확 일치로만 찾는다. 비리플렉션 빈 base의 생성 `IsEmptyV<::정규 철자>`도 정확한 타입을 검사하게 된다.

### 4.4 레코드 닫힘(NATIVE 재귀)

NATIVE 멤버의 강한 참조(RMC2205)·WorldOnly 소유자 형질(RMC2206) 재귀 검사는 값으로 담은 레코드의 필드·base와 템플릿 인자를 따라간다. 방식:

**레코드 색인.** 첫 실행 JSON에서 비템플릿 레코드 노드와 **특수화 노드**(암시적 인스턴스화, 부분·명시 특수화 — 첫 실행에서 완전한 것)의 필드 이름·base를 모은다. 개정 4에서 더한 것:
- 불완전한 레코드·특수화 노드(앞 선언, 인스턴스화되지 않은 특수화)도 "불완전"으로 넣는다. 템플릿 인자 규칙(아래 걷기)이 쓴다. 같은 열쇠에 완전한 노드가 있으면 그것이 이긴다.
- typedef로 이름을 받은 이름 없는 class(`typedef struct { … } Name;`)는 그 이름으로 넣는다. 정규 철자가 `ns::Name`이다(두 버전 같음, 실측).
- 필드는 멤버 경로다. 익명 struct·union(바로 뒤에 암시적인 이름 없는 필드가 온다)의 필드는 부모 범위로 주입되므로 부모 필드로 묻는다. 선언자가 있는 이름 없는 struct(`struct { int x; } inner;`)의 필드는 `decltype(::R::inner.x)`(배열이면 `inner[0].x`)로 묻는다(두 버전에서 컴파일됨, 실측). base가 있는 이름 없는 struct 멤버는 따라갈 수 없음(cannot verify)이다.
- WorldOnly 형질은 표지 변수를 읽지 않고 레코드마다 사실로 묻는다(아래 걷기). 암시적 인스턴스화 노드의 표지 변수에는 초기값이 없고, 패턴의 표지로 대신하면 부분·명시 특수화에서 틀린다(리뷰 실측).

패턴 레코드(`ClassTemplateDecl` 안의 `CXXRecordDecl`)는 색인하지 않는다 — 부분·명시 특수화는 패턴과 필드가 다르다(`Box<T, int>`의 필드는 패턴에 없어 `decltype(::rocks::Box<rocks::Payload, int>::data_)`가 "no member"로 실패한다, 리뷰어 실측). 특수화 노드의 필드·base는 두 버전에서 같다(실측).

**열쇠는 정규 철자다.** 걷기가 정규 철자로 레코드를 찾으므로 색인 열쇠도 정규 철자여야 한다.
- 비템플릿 레코드는 선언 경로(이름공간·클래스 이름)가 열쇠다. 단 inline 이름공간 아래 레코드(`NamespaceDecl`의 `isInline`)는 정규 출력이 inline 이름공간을 뺀다(`rocks::v1::Inl` → `rocks::Inl`, libc++의 `std::__1`도 같다, 리뷰어 실측). 그런 레코드는 선언 경로로 쓴 형식 철자 질문(`RocksCanonType<N, ::rocks::v1::Inl>`)의 답을 열쇠로 쓴다.
- 특수화 노드는 노드의 템플릿 인자(형식 인자는 정규 `qualType`, 정수 인자는 `value`, pack 인자는 펼친다)로 `::템플릿 이름<인자 전부>`를 만들어 같은 라운드에서 형식 철자를 묻고, 그 답을 열쇠로 쓴다. 문자열을 직접 비교하지 않는다: 정규 출력은 기본 인자를 생략한다(`rocks::ObjectRef<>`).
- 정수 인자의 `value`는 덤퍼가 64비트 2의 보수로 낸다(`bool` true와 `unsigned long long` 최댓값이 -1, 실측). 그래서 패턴의 매개변수 형식(`NonTypeTemplateParmDecl`, pack 원소 포함)으로 쓴다(개정 4): `bool`이면 `true`·`false`, 그 밖은 `static_cast<P>(값LL)`이다. P는 builtin이면 그대로, 템플릿의 형식 매개변수면 그 인자의 정규 철자(`IntegerSequence<T, T...>`), enum·typedef면 템플릿을 둘러싼 이름공간에 같은 줄로 연 별칭(`namespace probe { using rocksCanonParam_N_0 = Kind; }`)이다. 클래스 안 템플릿은 둘러싼 클래스로 한정한 철자(`::probe::Outer::E`)로도 묻고 답한 쪽을 쓴다. 틀린 쪽은 변환 실패로 거부되므로 두 답이 갈리지 않는다.
- 부를 수 없는 특수화: 템플릿 템플릿 인자, 주소·`nullptr`·class 값 인자, 다른 매개변수에 기대는 매개변수 형식(`typename T::X`, `auto`). 이런 레코드와 열쇠 질문이 실패한 레코드는 열쇠가 없다. 걷기가 같은 식별자의 형식에 닿아 열쇠로 찾지 못하면 그 이유를 붙여 cannot verify다(4.5절).
- 열쇠를 얻은 특수화(와 그 안의 레코드)의 필드·base·사실 질문은 열쇠 답(정규 철자, `::`를 붙인다)으로 부른다.

**걷기.** 1단계 탐침이 NATIVE 멤버의 정규 철자를 준 뒤, C#이 그 철자를 걸으며 값으로 닿는 형식을 모은다: 배열 원소, 템플릿 인자, 색인 레코드의 필드와 base.
- **템플릿 인자 걷기는 하한으로 남긴다**(비소유 템플릿 `WeakObjectRef`·`SoftObjectRef` 제외, 지금 `ClangAstParser.Traits.cs:66–76`). `Optional`·`Result`·`Variant`·`InlineArray`·`SharedPtr`는 T를 `alignas(T) unsigned char storage_[]`에 숨기므로(`Optional.h:209`, `Result.h:108`, `Variant.h:325`, `InlineArray.h:267`, `SharedPtr.h:93`) 필드로는 T가 보이지 않는다. `Optional<ObjectRef<X>>`는 인자 걷기만 잡는다. 지우는 것은 **패턴 필드 근사**(특수화를 패턴 필드로 대신 걷던 것)뿐이다.
- 아직 묻지 않은 레코드는 한 라운드에 묶어 묻는다: 필드마다 멤버 형식(`decltype(::열쇠::필드)`, 필드 이름은 그 레코드·특수화 노드에서), base마다 4.3절 질문, 그리고 `IsBaseOfV<rocks::Object, R>`·`RequiresWorldOnlyOwner<R>` 사실(뒤의 것은 생성 `static_assert`와 같은 형질이다, 개정 4). 새 레코드가 없을 때까지 되풀이한다.
- **색인에 없는 class는 거부한다.** 값으로 닿은 형식이 class인데 색인 열쇠에 없으면 RMC2205 cannot verify다. 부를 수 없는 특수화, 열쇠 질문 실패, 함수 안 지역 클래스가 여기에 든다. class 여부는 같은 라운드의 `IsClassV` 사실 질문으로 정한다. builtin·enum은 걷기가 끝난다. 조용히 건너뛰지 않는다.
- **템플릿 인자로만 닿은 형식**도 같은 규칙이다(개정 4 — 개정 3은 색인에 없으면 건너뛰어 열쇠를 잘못 만든 특수화를 조용히 놓쳤다). 다른 점은 하나다: 색인에서 **불완전한** 레코드면 건너뛴다. 인자로 쓰였을 뿐 값으로 담기지 않았다(`IntegerSequence<…>` 같은 태그, 선언만 있는 태그 class — 그 템플릿이 바이트 저장소에 담았다면 첫 실행에서 인스턴스화되어 완전하다). 완전성을 `sizeof` 사실로 묻지 않는다: 탐침의 `sizeof`가 그 특수화를 인스턴스화해 첫 실행과 다른 답을 낸다.
- **중복 제거.** 한 NATIVE 멤버의 걷기는 (형식, 직접 멤버인지, 템플릿 인자인지)를 열쇠로 한 번만 본다(개정 4 — 형식만 열쇠로 쓰면 직접 멤버 포인터나 인자 자리로 먼저 본 형식의 판정·class 질문이 빠져 결과가 NATIVE 순서에 따라 갈렸다).
- **포인터·참조**로 닿는 대상은 걷지 않는다. 그 대상이 Object 파생인지는 그 필드의 형식을 NativeMemberProbe의 판정 형판(`RocksProbeVerdict<decltype(::R::field)>`)으로 같은 라운드에서 묻고, C#의 base 사슬 걷기(`IsObjectDerived`)는 지운다. 불완전한 대상은 직접 멤버와 같은 규칙을 따른다: 모듈 public 헤더를 더해 다시 묻고, 그래도 불완전하면 허용한다(빌린 back-pointer — `Actor::world_`의 `World*`가 이 경우다). 그래서 Clang 21에서도 놓치던 중첩 포인터 가운데 대상이 완전한 Object 파생인 것이 판정되고, 불완전한 대상은 지금처럼 허용된다. 포인터·참조인지는 정규 철자에서 템플릿 꺾쇠 밖의 `*`·`&`로 가르고, 문자 리터럴 인자(`ByChar<'>'> *`) 안의 글자는 세지 않는다(개정 4).
- 전역 한정 철자로 부를 수 없는 레코드(함수 안 지역 클래스, 이름 없는 타입, 람다)에 값으로 닿으면 RMC2205 cannot verify다. 익명 이름공간은 한 칸을 빼고 부른다(`::ns::(anonymous namespace)::X` → `::ns::X`).
- 한도: NATIVE 멤버 하나가 걷는 형식 4,096개, 라운드 32회. 형식 수를 넘으면 그 NATIVE 위치에, 라운드 수를 넘으면 마지막 라운드에 물을 것이 남은 NATIVE 위치에 cannot verify다(개정 4 — 개정 3은 형식 수를 라운드의 모든 NATIVE가 함께 셌다). 라운드 수는 값으로 담는 중첩 깊이다. 실측 0–2회였다(1.4절).

시제품은 비템플릿 레코드만 선언 경로 열쇠로 물었고(특수화는 패턴 근사 그대로), 포인터 대상의 Object 파생은 C#의 base 사슬로 판정했다. 특수화 노드 색인, 정규 철자 열쇠, `IsClassV` 거부, 판정 형판 재사용은 시제품에 넣지 않았다.

### 4.5 실패 격리와 진단

탐침 줄 하나의 오류(이름할 수 없는 base, 인스턴스화 hard error)가 번역 단위 전체를 실패시키므로, NativeMemberProbe의 격리를 공용으로 뺀다: 진단의 `<stdin>` 줄(instantiation note 포함)을 질문에 대응시키고, 그 질문을 빼고 다시 실행한다. 대응하지 않는 오류는 질문 없이 템플릿만 실행해 전역 실패(RMC3000)와 가른다. 4.3절의 base 질문은 1이 실패하면 2로 다시 묻는다. 보고:

| 실패한 질문 | 진단 |
|---|---|
| 리플렉션 필드·NATIVE 멤버·base | RMC2001(그 선언 위치, Clang 진단 첫 error 줄을 붙인다) |
| 레코드 닫힘의 필드·base·열쇠·판정 | RMC2205 cannot verify(그 NATIVE 멤버 위치, 닿은 경로) |
| Binds·Within | 지금과 같다(RMC3000·RMC2208) |

### 4.6 첫 실행 JSON에 남는 의존

정규 형식 탐침 뒤에도 첫 실행 JSON 모양에 기대는 곳은 아래다. 실제 컴파일러 두 모양(Windows LLVM 22 leg와 macOS Apple clang leg)에서 도는 meta 시험이 고정한다(5.2절). 따로 모아 두는 기계적 리팩터링(개정 1의 `ClangAstShape`)은 하지 않는다.
- 노드 `kind`: `NamespaceDecl`·`CXXRecordDecl`·`ClassTemplateDecl`·`ClassTemplateSpecializationDecl`·`EnumDecl`·`EnumConstantDecl`·`FieldDecl`·`VarDecl`·`CXXMethodDecl`·`FunctionTemplateDecl`·`TypeAliasDecl`·`TypedefDecl`·`FriendDecl`·`UsingDecl`·`AccessSpecDecl`·`AnnotateAttr`·`ConstantExpr`·`IntegerLiteral`·`StringLiteral`·`CXXBoolLiteralExpr`·`ImplicitCastExpr`·`ParenExpr`·`TemplateArgument`.
- 키: `inner`·`name`·`id`·`loc`·`range`(위치 변형 — `ClangAstParser.Locations.cs`)·`completeDefinition`·`tagUsed`·`access`·`bases`(`access`·`isVirtual`·`type`)·`definitionData.isAbstract`·`isBitfield`·`storageClass`·`value`·`isInline`.
- 형식 철자로 남는 셋(4.1절): enum `fixedUnderlyingType`의 desugar(typedef → builtin이라 두 버전 같다), Validate hook·GENERATED_BODY 표지의 함수 형식 `qualType`(한정 선택 정규식, 생성 `static_assert`가 정확히 대조), base 식별자 추출(4.3절). bitfield 정수 판정(`IsIntegerSpelling`)은 첫 실행 철자로 남긴다(개정 3 — builtin 정수만 받고 typedef → builtin desugar는 두 모양에서 같다. 정수가 아닌 형식은 두 모양 모두 거부된다).
- 키가 없을 때 조용히 거짓이 되는 읽기(`Flag`)는 그 키에 기대는 meta 시험(추상 클래스, bitfield, 정의 여부)이 두 모양에서 잡는다.

### 4.7 기존 탐침과의 관계

- **StaticArrayProbe**: Clang 실행만 정규 형식 탐침에 합친다. 원소·크기 나누기는 Clang에 남긴다: 기존 `RocksStaticShape`(원소·크기)·`RocksStaticBase`(모든 차원을 벗긴 원소) 형판을 같은 탐침 텍스트에 옮겨, 원소는 형식 철자 질문(`RocksCanonType<N, typename RocksStaticShape<decltype(…)>::…>`)으로, 크기는 사실 질문으로 묻는다. 정규 철자를 문자열로 나누면 `int (*[2])[5]`, `void (*[3])(int)` 같은 철자에서 틀린다(리뷰어 실측).
- **Binds·Within**: 자기 실행을 유지한다(실패를 미러·클래스 위치로 대응하는 방식과 오류 코드가 다르다). 별칭 줄 뒤에 `template struct rocksBinds_canon::RocksCanonType<N, rocksBinds_N>;`(Within은 `rocksWithin_canon`)를 붙이고 별칭의 desugar 대신 그 인자를 읽는다. 래퍼 이름공간 이름이 기존 `-ast-dump-filter` 접두를 담으므로 필터는 그대로다. 비템플릿 타입을 가리키는 별칭(`using Alias = Plain;`)을 거친 Binds도 정규 철자가 된다.
- **NativeMemberProbe**: 그대로다. 그 판정 형판과 불완전 대상 규칙을 레코드 닫힘이 재사용한다(4.4절).

### 4.8 비용

정규 형식 탐침은 모듈마다 Clang 실행을 하나 더한다(정적 배열이 있던 모듈은 StaticArrayProbe가 빠지므로 같다). NATIVE 재귀가 레코드에 닿는 모듈은 라운드마다 하나를 더한다. 시제품을 Clang 22.1.8로 EngineTests Debug 메타 단계에서 잰 값이다(ms, 같은 기기에서 순차 실행, 한 번 측정).

| 모듈 | 첫 실행(JSON 크기) | 정적 배열 탐침 | 정규 형식 1단계(질문 수) | 레코드 닫힘 라운드(질문 수) |
|---|---|---|---|---|
| AssetRuntime | 687(85 MB) | — | 123(10) | 119(3), 130(1) |
| AssetTestFixtures | 2,087(275 MB) | 266 | 282(408) | — |
| Config | 549(59 MB) | — | 95(18) | — |
| ConfigTestFixtures | 895(126 MB) | — | 206(33) | — |
| CoreTypeBindings | 1,135(127 MB) | — | 175(94) | — |
| Curves | 778(109 MB) | — | 136(40) | — |
| CurvesTestFixtures | 1,274(160 MB) | — | 170(27) | — |
| ReflectionTestFixtures | 2,358(264 MB) | 271 | 249(615) | 218(8) |
| TraceControl | 1,758(139 MB) | — | 233(1) | — |
| World | 1,155(127 MB) | 304 | 237(32) | 410(11), 340(2) |
| WorldTestFixtures | 1,071(135 MB) | — | 244(8) | 226(1) |
| 합계 | 13,747 | 841 | 2,150 | 1,443 |

탐침 실행은 같은 번역 단위를 다시 해석하지만 덤프를 걸러 내므로(수 KB), 시간은 첫 실행(대부분 수백 MB JSON 출력)보다 훨씬 짧다. 질문 수(1–615)는 시간에 거의 닿지 않는다. 정적 배열 탐침을 합치면 순증은 모듈 11개에 약 2.8초(첫 실행 합계의 약 20%)다. C++ action이 모두 캐시된 EngineTests 빌드 전체(메타 단계와 생성 헤더 검증 컴파일이 대부분, 약 2.5분)에 비하면 2% 안쪽이다. 레코드 닫힘 라운드는 NATIVE가 값으로 레코드를 담는 모듈에만 생긴다.

## 5. 두 AST 모양을 함께 받기

### 5.1 플랫폼별 Clang

| 플랫폼 | 컴파일러 | AST 모양 | 시기 |
|---|---|---|---|
| Windows(로컬·CI (가) leg) | LLVM 21.1.6 clang-cl, MSVC 14.44 | 이전 | CLG4까지 |
| Windows(CI (나) leg, 이미지 기본 MSVC 14.52) | LLVM 22.1.8 clang-cl | 새 | CLG1+CLG2 병합부터 둘째 leg, CLG4부터 검증 도구 모음 |
| macOS(CI) | Apple clang 17.0.0(Xcode 16) | 이전 | Xcode가 LLVM 22 기반으로 올라갈 때까지 |
| Android(CI 없음) | NDK r29 clang 20.0.0 | 이전(실측: 1.1절 시험 파일의 JSON과 탐침 출력이 21.1.6과 같다) | NDK가 Clang 22로 올라갈 때까지 |

정규 형식 탐침 뒤에는 형식 철자가 모양과 무관하므로 MetaCompiler에 버전별 분기를 두지 않는다. 버전 차이는 4.6절의 구조 읽기에만 남는다.

### 5.2 시험이 두 모양을 고정하는 방법

- **meta 시험을 실제 컴파일러 두 모양에서 돌린다.** 기존 meta 시험(`Rocks.ToolTests meta`, Foundation Test에 들어 있다)이 새 모양은 Windows (나) leg(LLVM 22.1.8)에서, 이전 모양은 macOS leg(Apple clang 17)에서 돈다. CLG4까지는 Windows (가) leg(LLVM 21.1.6)도 돈다. 메시지에 철자를 담는 기대값은 정규 철자다(실측에서 고칠 기대값은 없었다 — 1.4절).
- **JSON fixture는 두지 않는다.** 첫 실행 덤프를 저장해 시험 입력으로 쓰는 방식은 성립하지 않는다: 노드 `id`가 실행마다 다른 포인터 값이고, `loc.file`이 절대 경로이며 파서가 그 경로에서 원문 byte를 다시 읽고(`PrefixSourceVerifier`·`DeclarationPrefixHas`), `-ast-dump-filter` 출력은 JSON 값을 이어 붙인 것이다(리뷰어 확인).
- **항상 도는 교차 확인은 두지 않는다.** 개정 1의 `--metadata-cross-check`(다른 Clang으로 메타데이터를 다시 만들어 byte 비교)는 (나) leg에서 LLVM 21.1.6을 MSVC 14.52 STL과 함께 써야 하는데, 리플렉션 번역 단위가 `yvals_core.h`에 닿아 `STL1000`으로 실패한다. 생성물의 버전 무관성은 Clang major를 올릴 때 로컬에서 한 번 잰다(1.4절처럼 두 버전의 생성물·탐침 정규 철자를 대조).

## 6. 도구 모음 버전 정책

### 6.1 도구 모음 표

`Build/Toolchain.json` 한 파일이 검증한 도구 모음을 정한다(평범한 JSON — PowerShell `ConvertFrom-Json`과 C#이 함께 읽는다).

| 칸 | 값(CLG4 뒤) | 읽는 곳 |
|---|---|---|
| `llvm.windows.version`, `llvm.windows.installers`(버전별 설치 파일 SHA-256) | 22.1.8 | `InstallLlvm.ps1`, CI |
| `clangFormat.version` | 21.1.6 | `InstallFormatter.ps1`(wheel URL·SHA-256 표는 스크립트에 두고 버전이 표와 같은지 확인한다), `FormatCheck`, MetaCompiler RMC3100 |
| `clang.minimum.appleClang` | 17 | Rocks.Build 도구 모음 확인(첫 줄이 `Apple clang version`인 컴파일러) |
| `clang.minimum.llvm`(플랫폼별) | Windows 21, macOS 21, Android 20(NDK r29 — CI가 없으므로 최소만 둔다) | Rocks.Build 도구 모음 확인(그 밖의 Clang — 공식 배포·Homebrew·NDK·포크) |
| `clang.verified`(플랫폼별 목록, CI가 있는 플랫폼만) | 정규화한 `--version` 첫 줄 전체: Windows `clang version 22.1.8 (ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)`, macOS `Apple clang version 17.0.0 (clang-1700.0.13.5)` | build-evidence 기록, CI 거부 |

CLG4 전에는 Windows `llvm.windows.version`이 21.1.6이고 `clang.verified`에 `clang version 21.1.6`(공식 배포는 괄호를 내지 않는다)도 있다. MSVC 도구 모음별 최소 Clang은 표에 두지 않는다(6.2절 — 고른 도구 모음의 STL 헤더에서 읽는다). CI의 MSVC 고정값도 표에 두지 않고 CI 행렬 한 곳에 둔다(행렬 값은 정적이어야 한다, 개정 5).

**검증 목록은 첫 줄 전체로 대조한다(개정 6).** `major.minor.patch`만 보면 Xcode가 바뀌어도(`clang-1700.0.13.5` → `clang-1700.3.19.1`) 같은 `17.0.0`이고, swiftlang 포크나 배포판 빌드도 업스트림과 같은 번호를 낸다. 그래서 `NativeToolchain.CompilerVersion`(첫 줄에서 경로·URL 괄호를 hex 토큰만 남기고 지운 것)을 목록과 그대로 비교한다. **최소는 제조사로 나눈다.** Apple clang 번호는 업스트림 번호와 다른 계열이라(Apple clang 17은 LLVM 19 무렵) 한 표로 두면 Homebrew LLVM 17·18이 Apple 최소 17을 통과한다.

**설치 SDK.** SDK 게임 빌드도 MetaCompiler와 도구 모음 확인을 돌리지만 설치 SDK는 `Build/` 파일을 싣지 않는다(`InstalledSdk.cs:171–177`). 그래서 C# 도구는 이 파일을 빌드 때 `Rocks.Tooling.Common`의 포함 리소스로 넣어 읽는다. SDK에 실린 도구가 그 엔진 버전의 표를 함께 가지므로 SDK 배치에서 파일을 찾는 규칙이 필요 없다. PowerShell 스크립트(`InstallLlvm.ps1`·`InstallFormatter.ps1`)는 저장소의 파일을 읽는다. 통합 시험(설치 SDK 게임 빌드)이 SDK 도구의 표로 도구 모음 확인이 도는지 본다.

지금 `21.1.6`이 적힌 곳(`InstallLlvm.ps1`의 `$version`, `FormatCheck.RequiredVersion`, MetaCompiler의 `"version 21.1.6"`, CI 주석)은 이 표를 읽게 바꾼다. clang-format의 위치는 바꾸지만 버전은 그대로다(CLGD11).

### 6.2 빌드의 버전 확인

Rocks.Build가 도구 모음을 찾을 때(지금 `--version`을 읽는 곳) 확인한다.
- 최소 Clang(Apple clang은 `clang.minimum.appleClang`, 그 밖은 플랫폼의 `clang.minimum.llvm`) 미만이면 거부한다.
- Windows에서는 고른 도구 모음의 STL이 요구하는 최소 Clang을 그 STL 헤더에서 읽는다: `<VCToolsInstallDir>/include/yvals_core.h`의 `defined(__clang__)` 분기 아래 `#if __clang_major__ < N`(14.29는 570행, `N = 11`). Clang major가 N 미만이면 첫 컴파일 전에 거부하고 `ROCKS_MSVC_TOOLSET`으로 다른 도구 모음을 고르거나 LLVM을 올리라고 안내한다. 표로 두지 않는 이유: 표는 새 14.5x가 나와도 미리 알 수 없고, 도구 모음 이름에서 표를 찾는 규칙도 따로 정해야 한다. 헤더에서 그 조건(`#if __clang_major__ < N` 바로 다음 줄이 `#error STL1000`)을 찾지 못하거나 파일을 읽지 못하면(입출력 오류 포함 — discovery를 실패시키지 않는다) build-evidence의 `stlClangCheck`에 이유를 남기고 진행한다 — 그때는 지금처럼 첫 컴파일의 `STL1000`이 알린다. 확인했으면 `stlClangCheck`에 요구한 최소를 남긴다(개정 6).
- 검증 목록 밖(새 major·patch)이면 build-evidence에 `toolchainVerified: false`와 버전을 남기고 한 줄 알린 뒤 진행한다. 로컬 실험은 막지 않는다. CI는 빌드 옵션 `--require-verified-toolchain on`을 주어 이 경우를 실패로 만든다(이미지가 Xcode·LLVM을 바꿔도 검증하지 않은 도구 모음으로 조용히 돌지 않게). 이 확인은 build-evidence를 시작하고 `toolchainVerified`를 적은 뒤에 하고, 실패도 evidence에 FAIL로 남긴다(앞 실행의 PASS evidence가 남지 않게, 개정 6). 게임 빌드의 `game-build-evidence.json`도 같은 `toolchainVerified`·`stlClangCheck`를 싣는다.
- MetaCompiler는 버전을 따로 확인하지 않는다. 새 모양은 meta 시험과 RMC2001이 잡는다.

### 6.3 CI

- **CLG1+CLG2 병합**: `InstallLlvm.ps1`이 `-Version`을 받아 표 밖의 버전도 설치하게 하고(`out/toolchains/llvm-<버전>`, PATH 맨 앞에 그 버전), Windows (나) leg를 더한다: LLVM 22.1.8·이미지 기본 MSVC(14.52, `ROCKS_MSVC_TOOLSET` 없음) — Foundation Test(Debug·Asan·Release, meta 시험 포함), EngineTests Debug 빌드. (가) leg(LLVM 21.1.6·MSVC 14.44)와 macOS leg는 그대로다. 병합 커밋은 `[ci]`를 달고 세 leg가 모두 통과해야 한다. 그래서 MSVC 14.52 STL + Clang 22 조합이 병합 전에 확인된다.
- **CLG3**: CI 빌드 명령에 `--require-verified-toolchain`을 준다.
- **CLG4**: 검증 도구 모음을 (나)로 바꾼다. (나)가 (가)가 하던 단계 전부(통합 시험, 유니티 끔 검증 빌드, commandlet, 식별자 병합 확인)를 맡고, (가) leg와 MSVC 14.44 고정을 없앤다. 이전 AST 모양은 macOS leg의 meta 시험이 계속 고정한다. (가)를 남기지 않는 이유: (가)는 14.44가 필요한데 이미지가 14.44를 계속 싣는다는 보장이 없고, 이전 모양은 이미 macOS leg가 덮는다.

### 6.4 MSVC 14.44 고정을 푸는 시점

CLG4에서 (가) leg와 함께 CI의 `ROCKS_MSVC_TOOLSET: 14.44`를 뺀다. 조건은 (나) leg가 CLG1+CLG2 병합 뒤 Foundation Test(Debug·Asan·Release)와 EngineTests Debug를 계속 통과한 것이다. `ROCKS_MSVC_TOOLSET` 기능(VsDevCmd `-vcvars_ver`, 만든 환경의 `VCToolsVersion` 확인)은 남긴다 — 로컬에서 여러 도구 모음 중 하나를 고를 때 쓴다. 이미지가 MSVC를 다시 올려 STL이 더 높은 Clang을 요구하면 6.2절의 확인이 첫 컴파일 전에 알린다.

## 7. C++ 코드의 Clang 22 경고

| 대상 | 결과 |
|---|---|
| EngineTests Debug 빌드(MSVC 14.29 STL, 888 action) | 경고·오류 0 |
| Foundation Debug 빌드(746 action) | 경고·오류 0 |
| Foundation Debug native 시험(seed 7351) | 2,585건 통과 |
| 도구 시험 `self`·`native`(Clang 22로 컴파일 계약 시험 포함) | 148건·124건 통과 |
| 도구 시험 `meta`(시제품 MetaCompiler) | 246건 통과(1.4절) |

지금까지 고칠 C++ 경고는 없다. 확인하지 못한 조합은 MSVC 14.52 STL + Clang 22(로컬에 Visual Studio 2026 없음), Development·Release(ThinLTO)·Asan 구성, macOS·Android다. CLG1+CLG2 병합의 (나) leg가 앞의 둘을 확인한다. 새 경고가 나오면 코드에서 고친다(경고를 끄지 않는다 — CLGD13).

## 8. 검증

| 단계 | 확인 |
|---|---|
| CLG0 | 공용 격리 helper로 바꾼 NativeMemberProbe의 기존 meta 시험(격리·cannot verify 포함)이 그대로 통과 |
| CLG1 | Clang 21.1.6으로 meta 시험 전체 통과, Foundation·EngineTests 생성물이 바뀌기 전과 byte 동일. Clang 22.1.8로 같은 생성물(로컬 대조). 새 시험: 한정 없는 레코드·enum 필드, `Object` base, 별칭 base, 별칭 템플릿·멤버 typedef·`decltype` base, private base, 데이터 멤버에 가려진 base, 파생과 같은 이름의 base, 같은 식별자 base 둘, 비템플릿 별칭 Binds, 비리플렉션 빈 base(이름공간 안), C 배열 원소가 함수 포인터·배열 포인터인 정적 배열 |
| CLG2 | 새 시험: 중첩 레코드의 완전한 Object 파생 포인터(Clang 21에서도 놓치던 것)는 거부, 불완전한 대상 포인터는 허용, 중첩 WorldOnly 형질, 부분 특수화·명시 특수화를 값으로 담은 NATIVE, NATIVE `Optional<ObjectRef<…>>`·`Tuple<ObjectRef<…>>`, inline 이름공간 아래 레코드를 값으로 담은 NATIVE, 재귀 base(`Rec<N> : Rec<N - 1>`)·pack 확장 base를 가진 특수화, 지역 클래스·선언만 있는 primary에 닿는 NATIVE(cannot verify). 개정 4 리뷰 사례: 직접 멤버 포인터와 같은 중첩 포인터, 인자 자리 뒤 값 자리, enum·`bool` pack·`unsigned long long` 최댓값·형식 매개변수에 기대는 NTTP 특수화, 클래스 안 템플릿의 enum NTTP, 바이트 저장소 템플릿에 숨긴 enum NTTP 특수화·특수화 안 중첩 레코드·typedef 이름 struct, 부분 특수화에만 있는 WorldOnly 표지, 표지를 없앤 명시 특수화, 특수화를 거친 중첩 WorldOnly, 템플릿 안 중첩 레코드(값·WorldOnly), 간접 base 이름을 가진 중첩 base, private base, pack 확장 base, base를 가리는 중첩 타입, 선언자가 있는 이름 없는 struct·익명 union, 문자 리터럴 인자 포인터, 공개 헤더로 완전해지는 중첩 포인터 대상, 템플릿 템플릿 인자 특수화(cannot verify), 함수 포인터·배열 포인터 원소 C 배열의 `ElementSpelling`. 기존 RMC2205·2206 시험 통과. **CLG1+CLG2 병합 조건**: `[ci]` 커밋으로 Windows (가)·(나) leg와 macOS leg 통과 |
| CLG3 | 도구 모음 확인의 거부·기록 시험(Rocks.Build.Tests — 제조사별 최소 미만, `yvals_core.h`의 최소 Clang 미달·조건을 찾지 못한 헤더, 첫 줄 전체 대조(다른 Xcode 빌드·포크는 미검증), 검증 목록 밖 기록, `--require-verified-toolchain`의 on·off·틀린 값과 미검증 실패), 설치 SDK 게임 빌드가 SDK 도구의 표로 확인을 도는지(통합 시험 — SDK `Tools/Rocks.Build.Cli/Rocks.Tooling.Common.dll`의 `Rocks.Toolchain.json` 리소스, `game --require-verified-toolchain on`과 `game-build-evidence.json`의 `toolchainVerified`) |
| CLG4 | 검증 도구 모음 전환 뒤 CI 전체 통과, 13절 문서 개정 |

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험으로 검증한다.

## 9. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| CLGD1 | 형식 판단은 정규 형식 탐침이 돌려준 Clang 정규 철자로만 한다. 첫 실행 JSON의 형식 철자는 진단 문구와 4.1절의 세 예외(enum 바탕 형식, Validate·표지 시그니처, base 식별자 추출)에만 쓴다 | 확정 |
| CLGD2 | 탐침은 명시적 인스턴스화의 템플릿 인자로 묻는다(접근 검사 면제, 정규 형식 저장). GENERATED_BODY friend에 기대지 않는다. 사실은 `int` 0·1로 넘긴다 | 확정 |
| CLGD3 | 정규 형식 탐침은 모듈마다 한 번(1단계) + 레코드 닫힘 라운드다. StaticArrayProbe는 Clang 실행만 합치고 원소·크기 나누기는 Clang 형판에 남긴다. Binds·Within은 자기 실행에 래퍼만 붙이고, NativeMemberProbe는 그대로 둔다 | 확정 |
| CLGD4 | base 목록은 레코드·특수화 노드에서 읽고, 식별자는 `TypeSpelling`의 마지막 식별자로 뽑는다. 정교한 형식 지정자(`struct ::Owner::식별자`)로 묻고, 파생(템플릿) 이름과 같거나 같은 식별자 base가 여럿이거나 1이 실패하면 둘러싼 이름공간 별칭으로 묻는다. 답은 `IsBaseOfV && !IsSameV`로 확인하고, 둘 다 실패하면 거부한다. 둘러싼 이름공간 순 탐색(`ResolveType`·`StructBase`)은 지운다 | 확정 |
| CLGD5 | NATIVE 재귀 색인은 비템플릿 레코드와 특수화 노드에서 만들고 열쇠는 정규 철자다(inline 이름공간 레코드와 특수화는 형식 철자 질문의 답). 템플릿 인자 걷기는 하한으로 남기고 패턴 필드 근사만 지운다. 값으로 닿은 class가 색인에 없거나 부를 수 없으면 RMC2205 cannot verify(`IsClassV` 사실). 템플릿 인자로만 닿은 형식은 색인에서 불완전할 때만 건너뛴다. WorldOnly 형질은 `RequiresWorldOnlyOwner<R>` 사실로 묻는다(개정 4). 포인터 대상의 Object 파생은 NativeMemberProbe 판정 형판으로 묻고, 불완전한 대상은 직접 멤버처럼 다시 물은 뒤 허용한다 | 확정 |
| CLGD6 | 탐침 실패는 `<stdin>` 줄로 질문에 대응해 격리한다(NativeMemberProbe의 격리를 공용으로). 리플렉션 선언은 RMC2001, 레코드 닫힘은 RMC2205 cannot verify, 대응 없는 실패는 RMC3000 | 확정 |
| CLGD7 | 첫 실행 JSON에 남는 의존(4.6절)은 실제 컴파일러 두 모양에서 도는 meta 시험이 고정한다. JSON fixture와 `ClangAstShape` 리팩터링은 하지 않는다 | 확정 |
| CLGD8 | 두 모양은 CI의 macOS leg(이전)와 Windows LLVM 22 leg(새)가 고정한다. 항상 도는 교차 확인(`--metadata-cross-check`)은 두지 않고, Clang major를 올릴 때 로컬에서 두 버전의 생성물·정규 철자를 한 번 대조한다 | 확정 |
| CLGD9 | 검증 도구 모음은 `Build/Toolchain.json` 한 파일이 정한다. C# 도구는 빌드 때 포함 리소스로 넣어(설치 SDK에도 실린다) 읽고, 설치 스크립트는 저장소 파일을 읽는다 | 확정 |
| CLGD10 | Rocks.Build가 도구 모음 발견 때 확인한다: 플랫폼 최소 미만은 거부, Windows는 고른 STL의 `yvals_core.h`에서 읽은 최소 Clang 미달을 거부, 검증 목록 밖은 build-evidence에 기록하고 진행하되 CI는 `--require-verified-toolchain`으로 실패시킨다 | 확정 |
| CLGD11 | clang-format은 21.1.6 고정을 유지한다. 컴파일러 버전과 따로 움직이고, 올리는 것은 전체 재서식을 담은 별도 변경이다 | 확정 |
| CLGD12 | CLG4에서 Windows 검증 도구 모음을 LLVM 22.1.8·이미지 기본 MSVC로 바꾸고, CI의 MSVC 14.44 고정과 Windows LLVM 21.1.6 leg를 함께 없앤다. 이전 AST 모양은 macOS leg가 덮는다. `ROCKS_MSVC_TOOLSET` 기능은 남긴다 | 확정 |
| CLGD13 | Clang 22에서 나오는 C++ 경고는 코드에서 고친다. 끄거나 낮추지 않는다 | 확정 |
| CLGD14 | 기존 문서의 "clang 21.1.6"은 현재 요구를 적은 곳만 고치고, 측정 기록은 그대로 둔다(13절) | 확정 |
| CLGD15 | CLG1과 CLG2는 한 병합으로 낸다. 그 병합에 `InstallLlvm.ps1 -Version`과 Windows LLVM 22 leg를 넣고, `[ci]` 커밋으로 Windows 두 leg와 macOS leg가 통과해야 병합한다 | 확정 |

## 10. 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| CLG0 | 공용 탐침 실행·격리 helper(NativeMemberProbe에서 뺀다) | 8절 CLG0 |
| CLG1 | 정규 형식 탐침 1단계: PROPERTY·NATIVE 멤버·클래스/struct base(4.3절), bitfield 정수 판정, Binds·Within 래퍼. StaticArrayProbe의 Clang 실행 합침(나누기는 Clang 형판). `ResolveType`·`StructBase` 근사 탐색 제거 | 8절 CLG1 |
| CLG2 | 레코드 닫힘: NATIVE 검사를 첫 실행 순회 밖(탐침 뒤)으로 옮기고, 특수화 노드 색인·정규 철자 열쇠·라운드·템플릿 인자 하한·`IsClassV` 거부·판정 형판 재사용과 불완전 대상 규칙. `IsObjectDerived`와 패턴 필드 근사 제거. 배열 원소 진단(`ArrayElementFailure`)은 쓴 그대로의 철자로 남긴다. `InstallLlvm.ps1 -Version`, CI Windows LLVM 22 leg. CLG1과 한 병합(CLGD15) | 8절 CLG2 |
| CLG3 | `Build/Toolchain.json`(C# 도구에 포함 리소스), Rocks.Build 버전 확인(`yvals_core.h`), CI `--require-verified-toolchain` | 8절 CLG3 |
| CLG4 | Windows 검증 도구 모음 전환, MSVC 14.44 고정·LLVM 21.1.6 leg 제거(CLGD12), 13절 문서 개정 | 8절 CLG4 |

CLG1만으로도 Clang 22 빌드는 통과하지만 NATIVE 재귀가 조용히 줄어드는 상태가 되므로 CLG2와 한 병합으로 낸다.

## 11. 위험과 한계

- **탐침 비용.** 모듈마다 Clang 실행이 하나 이상 늘어난다(4.8절). 메타 단계는 증분 key로 캐시되므로 헤더를 고친 모듈에서만 낸다.
- **정규 출력도 Clang 출력이다.** 기본 템플릿 인자 생략(`rocks::ObjectRef<>`), inline 이름공간 생략, `(anonymous namespace)` 표기, 정수 인자 접미 같은 정규 출력 규칙이 새 major에서 바뀔 수 있다. 21과 22는 실제 헤더 전체에서 같았다. major를 올릴 때 로컬 대조(CLGD8)와 meta 시험이 바뀜을 잡는다.
- **진단 철자가 바뀐다.** 포인터·배열·typedef를 담은 진단이 정규 철자로 나온다(`World *` → `rocks::World *`, `uint32[2]` → `unsigned int[2]`). 사용자에게 쓴 철자가 필요하면 진단에 첫 실행 `qualType`을 덧붙인다.
- **진단 순서가 바뀐다.** NATIVE 강한 참조 검사가 첫 실행 순회 밖으로 가므로, 한 헤더에 오류가 여럿이면 먼저 나오는 진단이 달라질 수 있다.
- **이름공간 별칭은 접근 검사를 받는다.** 4.3절의 2는 별칭 선언(명시적 인스턴스화가 아니다)이라, base가 다른 클래스의 protected·private 중첩 타입이면 거부될 수 있다. 1로 부를 수 있으면 생기지 않는다.
- **base 이름 부르기의 남은 한계.** 1과 2로도 부르지 못하는 base(중첩 클래스가 바깥 클래스의 멤버 타입 둘을 같은 식별자로 base로 둔 경우 등)는 거부로 드러난다. 조용히 다른 타입을 고르지는 않는다(`IsBaseOfV && !IsSameV` 확인).
- **레코드 닫힘이 따라가지 못하는 것.** 부를 수 없는 특수화(4.4절 열쇠 — 템플릿 템플릿 인자, `auto` NTTP, 바깥 클래스 템플릿 매개변수에 기대는 NTTP 형식), 이름 없는 struct를 가리키는 포인터, base가 있는 이름 없는 struct 멤버에 닿으면 cannot verify로 거부한다. 조용히 받지 않는 대신 그런 형식을 NATIVE로 값으로 담은 헤더는 바꿔야 한다.
- **명시적 인스턴스화의 면제 범위.** 접근 검사 면제는 명시적 인스턴스화 선언의 이름에만 적용된다. 탐침 템플릿 본문(`RocksCanonBase`의 `static_assert`)은 템플릿 매개변수만 쓰므로 접근 문제가 없다.
- **확인하지 못한 것.**
  - MSVC 14.52 STL + Clang 22, Clang 22의 Development·Release·Asan 구성, macOS·Android에서의 탐침(CLG1+CLG2 병합의 CI가 앞의 둘과 macOS를 확인한다).
  - 개정 2 방식(4.3절의 `TypeSpelling` 식별자·`IsSameV` 확인·2로 다시 묻기, 4.4절의 특수화 노드 색인·정규 철자 열쇠·`IsClassV` 거부·판정 형판 재사용)은 작은 시험 파일로만 확인했고 시제품에는 없다.
  - 리플렉션 타입에 같은 식별자 base가 둘인 경로: 시제품은 RMC2001로 멈출 뿐 2로 다시 묻는 것을 실제 헤더에서 재지 않았다(1.4절의 실측이 아니다).
  - 배열 원소 진단의 철자.

## 12. 버린 대안과 UE와 다르게 정한 점

버린 대안:

| 대안 | 버린 이유 |
|---|---|
| C#이 AST 위에서 C++ 이름 조회를 흉내 낸다(using 지시·선언, inline 이름공간, 클래스 범위와 base 멤버, 선언 순서, 별칭 사슬, 템플릿) | 근사다. 틀리면 다른 타입을 고르거나 조용히 못 찾는다. 지금의 둘러싼 이름공간 탐색(`ResolveType`)도 이 근사이고, 이 문서가 지운다 |
| AST 모양을 감지해 Clang 21은 기존 경로, 22는 새 경로 | 두 경로를 유지해야 하고, 22 경로는 결국 이 문서의 해법이나 위 근사가 필요하다 |
| Clang 21 고정을 계속한다 | MSVC STL 14.52가 22 미만을 거부한다. Visual Studio 갱신마다 고정 비용이 커지고, Apple clang·NDK도 결국 22 기반이 된다 |
| `-fdump-record-layouts-canonical`(`-complete`)로 레코드 정규 형식을 읽는다 | 텍스트 형식이 ABI마다 다르다(clang-cl의 MS 레이아웃과 Itanium). 완전한 레코드만 나오고, 특수화 이름에 sugar가 남는다(`Field<0, decltype(…)>`) |
| `-ast-dump-decl-types` | 실측: 타입 선언의 형식만 덧붙이고 필드 형식은 덧붙이지 않는다 |
| `GeneratedType<Tag>` 특수화 안 변수로 묻는다(기존 탐침 방식) | 리플렉션 타입은 되지만 비리플렉션 레코드의 private 필드에 닿지 못한다. 명시적 인스턴스화 하나로 통일한다 |
| libclang(ClangSharp)·Clang 플러그인·자체 덤퍼 | 플랫폼·버전마다 네이티브 바이너리를 빌드·배포해야 한다. 정규 출력은 결국 같은 Clang 출력이다 |
| 두 버전의 첫 실행 JSON을 fixture로 저장해 시험 입력으로 쓴다(개정 1) | 노드 `id`가 실행마다 다르고, `loc.file` 절대 경로에서 파서가 원문을 다시 읽으며, 필터 출력이 이어 붙인 JSON 값이다(5.2절). 실제 컴파일러 두 모양의 meta 시험으로 고정한다 |
| CI에서 다른 Clang으로 메타데이터를 다시 만들어 byte 비교한다(개정 1 `--metadata-cross-check`) | LLVM 21.1.6은 MSVC 14.52 STL에서 `STL1000`으로 리플렉션 번역 단위를 컴파일하지 못한다. major를 올릴 때 로컬에서 한 번 잰다 |
| CLG4 뒤에도 Windows LLVM 21.1.6 leg를 남긴다(개정 1) | MSVC 14.44가 필요한데 이미지가 계속 싣는다는 보장이 없고, 이전 AST 모양은 macOS leg가 덮는다 |
| MSVC 도구 모음별 최소 Clang 표(개정 1) | 새 14.5x를 미리 알 수 없고 찾는 규칙도 따로 정해야 한다. 고른 STL의 `yvals_core.h`에서 읽는다 |
| `Build/Toolchain.json`을 설치 SDK에 복사하고 `rocks-sdk.json`에 기록한다 | 두 배치(저장소·SDK)의 찾는 규칙과 SDK manifest 변경이 필요하다. C# 도구의 포함 리소스가 더 단순하다(도구가 SDK에 실린다) |
| 정규 철자를 문자열로 나눠 정적 배열 원소·크기를 얻는다(개정 1) | `int (*[2])[5]`, `void (*[3])(int)`에서 틀린다. Clang 형판이 나눈다 |
| 템플릿 인자로만 닿은 형식의 완전성을 `sizeof` 사실로 묻는다(개정 4 리뷰 제안) | 탐침의 `sizeof`가 그 특수화를 인스턴스화해 완전하게 만든다. 첫 실행 JSON의 노드가 완전한지로 가른다 |
| 지역 클래스를 이름 목록으로 가른다(개정 3) | 이름만 비교해 다른 범위의 같은 이름을 잘못 가른다. 색인에 없으면 `IsClassV`로 가르는 규칙 하나로 덮는다 |
| WorldOnly 표지를 첫 실행 JSON의 변수 초기값으로 읽고, 특수화는 패턴의 표지로 대신한다(개정 3) | 암시적 인스턴스화 노드의 표지에는 초기값이 없고, 부분 특수화에만 있는 표지를 놓치며 명시 특수화가 없앤 표지를 잘못 본다. 생성 코드와 같은 형질을 사실로 묻는다 |
| NTTP 값을 정규 출력 문자열(`probe::Kind::B`, `true`)과 직접 맞춘다 | 정규 출력이 기본 인자를 생략해 노드의 인자 목록과 길이가 다르고, 값 표기(`(probe::Kind)5`, 접미 없는 큰 정수)가 Clang 출력 규칙에 묶인다. 노드의 값을 매개변수 형식으로 캐스트해 Clang에 묻는다 |

UE와 다르게 정한 점:

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 헤더 해석 | UnrealHeaderTool이 자체 C# 토크나이저·파서로 헤더를 읽고 이름을 스스로 해석한다(`Engine/Source/Programs/Shared/EpicGames.UHT/Tokenizer/`) | Clang이 헤더를 해석하고 MetaCompiler는 Clang 출력을 읽는다 | 기존 구조다. 이름 해석·특수화 선택을 Clang에 맡기는 것이 이 문서의 원칙이다 |
| 컴파일러 버전 표 | `Engine/Config/Windows/Windows_SDK.json`의 `PreferredClangVersions`(범위 목록), `MinimumClangVersion`(18.1.8), MSVC 도구 모음별 최소 Clang `MinimumRequiredClangVersion`(`14.44-19` 등), `PreferredVisualCppVersions`·`BannedVisualCppVersions` | `Build/Toolchain.json`의 검증 버전 목록과 플랫폼 최소. MSVC 도구 모음별 최소 Clang은 표 대신 고른 STL의 `yvals_core.h`에서 읽는다 | UE는 최소 미만이면 그 도구 모음을 오류로 두고(`MicrosoftPlatformSDK.cs` `AddClangToolChain`), 선호 목록은 여럿 중 고르는 순위로만 쓴다. Rocks는 하나를 명시해 쓰므로 검증 목록 밖을 증거에 기록하고 CI에서는 실패시킨다. STL 헤더를 읽으면 새 도구 모음도 표를 고치지 않고 확인된다 |
| CI 도구 모음 | (해당 없음) | 검증 버전을 SHA-256 고정 설치 파일로 깐다(`InstallLlvm.ps1`) | 이미지 갱신과 무관하게 같은 결과를 낸다 |

## 13. 다른 문서 개정

이번 개정에서 고친 것:
- [빌드 안내](../../Build/README.md) 요구 사항 절: 검증한 Clang 문장 끝에 이 문서 링크를 더했다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 1.3절: 합성 번역 단위 문단 끝에 형식 철자 해석이 이 문서를 따른다는 한 줄을 더했다.

구현할 때 고칠 것(현재 요구를 적은 곳):
- `Build/README.md` 요구 사항 절의 "검증한 Clang은 21.1.6이다(MetaCompiler가 Clang 22의 JSON AST 타입 철자를 아직 받지 않는다)"와 LLVM 21.1.6·MSVC 14.44 문장: CLG4.
- `.github/workflows/foundation.yml`: Windows LLVM 22 leg(CLG2), `--require-verified-toolchain`(CLG3), 도구 모음 주석·`ROCKS_MSVC_TOOLSET`·LLVM 21.1.6 leg 제거(CLG4).
- `Build/InstallLlvm.ps1`: `-Version`(CLG2), 머리 주석("the verified version stays at 21.1.6 until the MetaCompiler supports Clang 22")과 `$version`을 표에서 읽기(CLG3).
- [리플렉션 기반](ReflectionFoundations.ko.md) 2.4절(NATIVE 재귀가 Clang AST 철자를 걷는다는 서술), 4.6절(Binds `using rocksBinds_N`과 desugar): CLG1·CLG2.
- [리플렉션 확장](ReflectionExtensions.ko.md) 8절(정적 배열 탐침 — 실행 합침, 나누기는 그대로), 9.2절(Within 별칭 desugar): CLG1.
- [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md) Binds 절(`using rocksBinds_N = ::<철자>` 서술)과 "현재 한계와 검증 범위": CLG1·CLG2.
- `Programs/Rocks.MetaCompiler` 주석: `TypeSpelling`(Clang 정규 철자라는 주석), `StaticArrayProbe` 머리 주석, `NativeMemberProbe` 머리 주석의 "값으로 담는 타입은 첫 실행 AST의 재귀 검사가 본다": CLG1·CLG2.

그대로 두는 것(측정 기록 — 그때의 도구 모음을 적은 것): [압축](Compression.ko.md) 검증 표, [로깅](LoggingSystem.ko.md)·[동기화](Synchronization.ko.md)의 실측, [리플렉션 기반](ReflectionFoundations.ko.md) 8절 교차검증·도구 지문 예시, [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md)·[유니티 빌드·PCH](UnityBuildPch.ko.md)의 측정 도구 모음, `Docs/Implementation`·`Docs/Reviews`의 실행 기록, 코드 주석의 예시(`NativeBuildModels.cs:171`, `ToolchainFingerprint.cs:27`, `ActionGraph.cs:664`). clang-format 21.1.6을 적은 곳(`Build/README.md` 6·169행, [코딩 스타일](../CodingStyle.md), 리플렉션 메타데이터 생성 형식 절)은 CLGD11에 따라 그대로다.

## 14. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-20 | Clang 22 지원 설계. Clang 21·22 JSON 차이 실측(1.1절), MetaCompiler 의존 조사(1.2절), 고치지 않은 MetaCompiler의 실패 실측(1.3절 — 실제 헤더와 meta 시험 56건, 조용한 GC 안전 검사 누락 3건), 정규 형식 탐침 시제품 실측(1.4절 — 두 버전 생성물 byte 동일, meta 시험 통과, Clang 22 C++ 경고 0), 설계(4절 — base는 정교한 형식 지정자와 이름공간 별칭, `bool` 사실 인자는 `int`로), 두 모양 시험(5절), 도구 모음 정책(6절), 결정 CLGD1–CLGD14, 단계 CLG0–CLG4 |
| 2 | 2026-11-20 | 리뷰어 교차 검증 적용(Clang 21.1.6·22.1.8 양쪽 탐침, 13건 모두 받아들임). 레코드 닫힘(4.4절): 패턴 필드 근사만 지우고 템플릿 인자 걷기는 하한으로 남김(`Optional`·`Result`·`Variant`·`InlineArray`·`SharedPtr`가 T를 바이트 저장소에 숨긴다), 특수화의 필드·base는 첫 실행의 특수화 노드에서 읽고 노드 → 정규 철자는 형식 철자 질문으로 대응, inline 이름공간 레코드도 정규 철자 열쇠, 색인에 없는 class는 `IsClassV`로 가려 RMC2205 cannot verify, 불완전한 중첩 포인터 대상은 직접 멤버처럼 다시 물은 뒤 허용(`World*`가 판정된다는 개정 1 서술 정정). base(4.3절): 확인을 `IsBaseOfV && !IsSameV`로(`Rec<2>::Rec`가 자신을 답함), 특수화 소유자의 base는 특수화 노드에서, 식별자는 `TypeSpelling`의 마지막 식별자(별칭 템플릿·멤버 typedef·`decltype` base 해결, 별칭 id 색인 삭제), 파생 이름 비교는 템플릿 이름으로, 1 실패 시 이름공간 별칭으로 다시 묻기, 템플릿 매개변수 base 한계 삭제(실제로 풀린다), 4.1절 예외 셋. 정적 배열(4.7절): 실행만 합치고 나누기는 Clang 형판. 시험(5.2절·CLGD7·CLGD8): JSON fixture·갱신 명령·`ClangAstShape` 삭제, 항상 도는 교차 확인 삭제(MSVC 14.52에서 LLVM 21이 `STL1000`), 실제 컴파일러 두 모양의 meta 시험으로 고정. 도구 모음(6절): Windows LLVM 21 leg는 CLG4에서 14.44 고정과 함께 삭제, MSVC별 최소 Clang 표 대신 `yvals_core.h` 읽기, `Build/Toolchain.json`은 C# 도구 포함 리소스(설치 SDK), Android는 최소만, CI는 `--require-verified-toolchain`. 단계: CLG0은 격리 helper만, `InstallLlvm.ps1 -Version`과 LLVM 22 leg를 CLG2로 당기고 CLG1+CLG2 병합 조건(`[ci]`, macOS·Windows 두 leg)을 CLGD15로 더함. 한계(11절)에 이름공간 별칭의 접근 검사, 같은 식별자 base 경로가 실측이 아님을 적음. 결정 CLGD1–CLGD15 |
| 3(구현 기록) | 2026-11-20 | CLG0–CLG2 구현(`feature/clang-version-support`). 구현하며 정한 점: (가) 공용 실행·격리는 `ClangProbe`(질문 한 줄 = `<stdin>` 한 줄, 판정 형판·정규 형식 탐침·공개 헤더로 다시 묻기가 함께 쓴다). (나) 사실 질문은 `long long` 인자다(C 배열 크기가 `size_t`). (다) bitfield 정수 판정은 첫 실행 철자로 남긴다(4.6절). (라) 레코드 닫힘의 포인터 판정은 필드가 아니라 **정규 형식 철자**로 묻는다(`RocksProbeVerdict<rocks::Allocator *>` — 접근 검사 면제는 명시적 인스턴스화 인자 전체에 걸린다). 그래서 템플릿 인자로만 보이는 포인터(`Optional<Node *>`)도 하한 걷기에서 판정된다. (마) 1단계는 첫 실행과 같은 include 순서, 레코드 닫힘과 그 공개 헤더 재질문은 NATIVE 판정과 같은 순서(이 모듈 출력 루트 제외)로 돈다. (바) 특수화 노드의 `bool` 인자는 패턴의 템플릿 매개변수 종류로 `true`·`false`로 쓴다(덤퍼는 -1을 낸다). (사) 배열 원소 실패 진단(`ArrayElementFailure`)은 문구에만 쓰는 쓴 철자 재귀(기존 `StrongReferencePath`)를 그대로 쓴다 — 판정이 아니고, 찾지 못하면 Clang 원문 진단으로 거부된다. (아) 레코드 닫힘이 한 번에 걷는 형식 4,096개·라운드 32회를 넘으면 cannot verify다. (자) `_Atomic(…)`·벡터 형식은 따라가지 않는다(전과 같다). 시험: `MetaTests.CanonicalTypes.cs`(8절 CLG1·CLG2 항목). 비템플릿 별칭 Binds 새 시험은 두지 않았다 — 실제 헤더의 Binds 28개가 정규 철자로 같고, 기존 Binds 시험이 두 모양에서 돈다. 로컬 검증(MSVC 14.29 STL): Foundation·EngineTests Debug 빌드·시험(meta 252건 포함)이 Clang 21.1.6과 22.1.8에서 모두 통과했고, EngineTests 메타 모듈 12개(MA1의 MeshSource 포함) 생성물이 두 버전에서 byte 동일하며, Rocks.Build.Tests·format·policy가 통과했다 |
| 4(리뷰 반영) | 2026-11-20 | CLG1·CLG2 리뷰(Clang 21.1.6·22.1.8 임시 meta 시험) 반영. 레코드 닫힘(4.4절): (가) 불완전한 레코드·특수화 노드도 색인한다. 템플릿 인자로만 닿은 형식은 색인에서 완전하면 걷고, 불완전하면 건너뛰고, 없으면 값 자리처럼 `IsClassV`로 가려 class면 cannot verify다(인자 자리에서 조용히 건너뛰던 퇴행 수정, 지역 클래스 이름 목록 삭제). (나) 특수화 열쇠의 NTTP 값은 패턴의 매개변수 형식으로 쓴다(`bool`은 `true`·`false`, 그 밖은 `static_cast<P>(값LL)`, P는 builtin·형식 매개변수의 인자·이름공간 별칭·둘러싼 클래스 한정). 부를 수 없는 특수화와 열쇠 질문이 실패한 레코드에 닿으면 cannot verify다. 열쇠를 얻은 특수화의 질문은 열쇠 답으로 부른다. (다) WorldOnly 형질은 레코드마다 `RequiresWorldOnlyOwner<R>` 사실로 묻는다(표지 초기값·패턴 표지 읽기 삭제). (라) 식별자는 꺾쇠·괄호 깊이 0의 마지막 `::` 뒤에서 꼬리 템플릿 인자를 걷어 얻고, 템플릿 인자 나누기는 끝 `>`와 짝인 `<`에서 하며, 문자·문자열 리터럴 안은 세지 않는다. 1단계 base 답은 마지막 식별자가 물은 식별자와 같아야 받는다(4.3절). (마) 걷기의 중복 제거 열쇠에 직접 멤버·템플릿 인자 여부를 넣는다. (바) 익명 struct·union만 부모 필드로 펼치고, 선언자가 있는 이름 없는 struct는 멤버 경로(`inner.x`, `arr[0].x`)로 묻는다. typedef 이름을 받은 이름 없는 class는 그 이름으로 색인한다. (사) 한도(개정 3 (아) 정정): 형식 4,096개는 NATIVE마다 세고, 넘친 NATIVE 위치에 cannot verify다. 라운드 32회를 넘으면 물을 것이 남은 NATIVE만 cannot verify다. 정리: 호출처 없는 `RequiresWorldOnlyOwnerPath`와 표지 초기값 읽기 삭제, NATIVE 탐침·레코드 색인 주석 갱신. 한계(11절)와 버린 대안(12절)에 부를 수 없는 특수화, `sizeof` 완전성 질문, 표지 초기값 읽기를 적음. 시험은 8절 CLG2 항목 |
| 5(구현 기록) | 2026-11-20 | CLG3 구현. 구현하며 정한 점: (가) `Build/Toolchain.json`에는 LLVM 설치 파일 SHA-256(버전별)·기본 LLVM 버전·clang-format 버전·플랫폼 최소·검증 목록만 둔다. clang-format wheel URL·SHA-256 표는 `InstallFormatter.ps1`에 그대로 두고 그 버전이 표와 같은지 스크립트가 확인한다(플랫폼별 wheel 표를 옮길 이득이 없다). CI의 MSVC 고정값(`msvc.ciToolset`)도 표에 두지 않고 CI 행렬 한 곳에 둔다(행렬 값은 정적이어야 한다). (나) C# 도구는 표를 `Rocks.Tooling.Common`의 포함 리소스 `Rocks.Toolchain.json`으로 읽는다(`ToolchainTable`). 설치 SDK에 도구가 실리므로 SDK 게임 빌드도 같은 표로 확인한다. (다) 확인은 `NativeToolchains.Discover*`가 하고(`ToolchainCheck`), 검증 목록 밖이면 빌드 CLI가 한 줄 알리고 build-evidence에 `toolchainVerified: false`를 남긴다. `--require-verified-toolchain on`(Rocks.proj `RequireVerifiedToolchain=on`, CI는 job 환경 변수로 준다)이면 실패한다. (라) STL 최소 Clang은 고른 도구 모음의 `<VCToolsInstallDir>/include/yvals_core.h`에서 읽고, 읽지 못하면 확인을 건너뛴다(첫 컴파일의 `STL1000`). |
| 6(리뷰 반영) | 2026-11-20 | CLG3 리뷰 반영. (가) 검증 목록은 정규화한 `--version` 첫 줄 전체로 대조하고, 최소 Clang은 제조사로 나눈다(`clang.minimum.appleClang`, `clang.minimum.llvm` — 6.1절). (나) `--require-verified-toolchain` 판정을 Rocks.Build(`ToolchainCheck.RequireVerified`·`Enforce`)로 옮겨 on·off·틀린 값을 Rocks.Build.Tests가 보고, 통합 시험이 SDK 도구의 표 리소스와 `game --require-verified-toolchain on`을 본다. (다) 미검증 실패는 build-evidence를 시작하고 `toolchainVerified`를 적은 뒤 확인해 FAIL evidence로 끝낸다. 게임 evidence에도 `toolchainVerified`·`stlClangCheck`를 싣는다. (라) Rocks.Build.Tests는 발견한 도구 모음의 `ToolchainVerified`가 표 판정과 같은지만 단언한다(로컬 도구 모음이 미검증이어도 깨지지 않는다). (마) `yvals_core.h`를 읽지 못하거나(입출력 오류 포함) 조건을 찾지 못하면 evidence `stlClangCheck`에 이유를 남기고, 조건 정규식은 `#error STL1000` 줄까지 묶는다(6.2절). (바) 6.1절 표·6.2절·8절을 개정 5 결정에 맞췄다. (사) clang-format 버전 판정은 `ToolchainTable.IsClangFormatVersion` 하나로 `FormatCheck`와 MetaCompiler RMC3100이 함께 쓴다(`version 21.1.60`을 받지 않는다). (아) 빌드 안내와 `Programs/Rocks.Build/README.ko.md`에 `Build/Toolchain.json`과 `RequireVerifiedToolchain`·`--require-verified-toolchain`을 적었다. (자) CLG3 검증에서 드러난 출력 게시 실패(CLG3과 무관): 컴파일 action의 출력 게시(`Executor.PublishOutputs`)가 대상 .obj를 다른 핸들(백신·인덱서)이 열고 있으면 경로 없는 "Access to the path is denied"로 실패했다. 게시 실패는 단계와 경로를 담은 오류("Cannot publish 원본 -> 대상")로 감싸고, Windows의 대상 교체는 `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)`로 한다 — 보유자가 삭제 공유를 주면 성공한다(대기·재시도는 쓰지 않는다). 이를 지원하지 않는 파일 시스템이면 기존 교체로 돌아간다. 시험은 Rocks.Build.Tests(삭제 공유로 연 대상은 교체, 삭제 공유 없이 연 대상은 경로를 담은 실패) |
