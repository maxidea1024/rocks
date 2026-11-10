# Reflection과 Clang 메타 컴파일러 구현

이 문서는 현재 구현한 Reflection 범위와 사용 계약을 정리한다. 엔진 런타임은 자체 타입·컨테이너를 사용하고, 메타 컴파일러는 호스트 C#/.NET 10과 네이티브 Clang을 사용한다. 전체 엔진 메타 빌드 시스템, 에디터 프로퍼티 UI, 자동 스키마 마이그레이션 생성은 아직 구현 범위에 포함하지 않는다. ObjectGraph의 명시 migration callback과 그 출력 검증은 별도 Serialization 계층에서 제공한다.

## 선언과 지원 형식

`ROCKS_CLASS("안정 GUID", 버전, 지정자...)`로 객체 타입을, `ROCKS_PROPERTY("안정 GUID", 지정자...)`로 필드를 선언한다. 기존 `ROCKS_REFLECT_TYPE`과 `ROCKS_REFLECT_FIELD`도 호환된다. GUID는 0이 아닌 128비트 값이며 해당 입력 번역 단위에서 타입·필드 ID의 중복을 거부한다. 이름 변경과 무관하게 같은 의미의 필드는 같은 ID를 유지해야 한다. 버전은 Clang이 평가한 정수 상수의 값이 1부터 `UINT32_MAX` 사이여야 한다. ID는 좁은 문자열 리터럴만 허용하며 문자열 포인터 연산을 ID로 해석하지 않는다.

| 필드 형식 | 런타임 표현 |
| --- | --- |
| `bool` | `ValueKind::Bool` |
| `uint8`, `uint16`, `uint32`, `uint64` | 폭에 대응하는 부호 없는 정수 |
| `int32`, `int64` | 폭에 대응하는 부호 있는 정수 |
| `float`, `double` | `Float32`, `Float64` |
| `Guid` | 두 개의 64비트 정수로 구성된 값 |
| `String` | 소유한 유효 UTF-8 문자열; 길이는 바이트 수 |
| `ObjectRef<T>` | `ObjectHandle`을 담은 참조 값; 실제 변경은 Registry barrier 경유 |

현재 생성 대상은 이름 있는 비템플릿 타입이며, 모든 비정적 필드에 annotation이 필요하다. private/protected 필드는 해당 클래스에 `ROCKS_GENERATED_BODY()`를 선언해야 한다. raw pointer, 배열·컨테이너, enum, const/volatile 필드, bitfield와 annotation을 붙인 static 필드는 거부한다. static 보조 데이터는 annotation 없이 둘 수 있다. 수동 `Trace`와 자동 추적을 섞어 완전한 필드 레이아웃으로 오인하지 않도록, 생성 대상의 직접 선언된 `Trace`는 거부한다.

단일 public 비가상 상속만 지원한다. 기반 타입은 `Object` 또는 같은 입력 번역 단위에서 확인된 reflected 타입이어야 한다. 파생 스키마는 상속 필드를 안정 ID 순서로 펼치며, 상속을 포함한 필드 수는 최대 4,096개다. 생성 접근기는 멤버 포인터를 사용한다. 객체 레이아웃에서 offset을 추정하거나 raw 필드 주소를 반환하지 않는다.

## 생성물과 등록

생성 헤더는 `GeneratedType<T>`의 `Id`, `Schema`, `Ops`, `Register`를 제공한다. `Schema()`는 타입·필드 ID, 버전, 이름, 값 형식, 기반 타입 ID와 참조 대상 타입 ID를 제공한다. `Ops()`는 실제 C++ 타입으로 검증되는 read/write/writeStaged 함수와 자동 Trace를 제공한다.

`MakeRuntimeTypeOps<T>`는 `noexcept T(Allocator&)`가 있으면 Registry allocator를 전달하고, 없으면 `noexcept T()`를 호출한다. 실패 가능한 초기화는 생성자 이후의 ConstructionContext 단계에서 처리한다. 자동 생성한 `Ops()`는 전체 지원 필드를 확인한 후 `completeFieldLayout = true`를 지정하고, 부수 효과 없이 ObjectRef를 방문하는 Trace에 `parallelTraceSafe = true`를 지정한다. 수동 Ops의 두 값은 기본적으로 false다. 실제 병렬 실행의 허용 여부는 GC의 단계·safepoint 규약에 달려 있다.

TypeRegistry는 스키마, 이름·속성 바이트와 RuntimeFieldOps 배열을 등록 시 깊게 복사한다. 원본 배열을 함수 지역 변수로 두어도 등록 이후의 배열 수명에 의존하지 않는다. 코드 포인터의 동적 모듈 수명은 `ModuleLease`가 유지한다. 등록 실패 시 기존 레코드는 유지되며 값으로 전달한 Ops의 ModuleLease는 정상 반환된다.

필드 ID의 중복, 지원하지 않는 형식 값, 스키마와 함수표의 불일치, 빠진 callback, 기반 타입의 미등록, 기반 native type 불일치, 상속 필드 표현의 불일치를 등록 단계에서 거부한다. 수동 함수 포인터가 실제로 올바른 C++ 필드를 처리하는지는 런타임에서 증명하지 못한다. 수동 메타데이터 제공자는 스키마·typed thunk·Trace의 일치를 지킬 책임이 있다.

## TypeLease와 상속 수명

기반 타입을 먼저 등록한다. 파생 레코드는 기반 레코드에 의존하므로, 외부 lease가 없어도 파생 타입이 남아 있으면 기반 타입 Unregister는 `Busy`를 반환한다. 파생 타입의 TypeLease가 있으면 파생 스키마와 그 기반 연쇄가 유지된다. 내부 기반 의존성은 외부 TypeLease 계수와 분리하여 Shutdown을 영구적으로 막는 순환 계수를 만들지 않는다.

Shutdown은 새 Acquire/Clone을 닫고 외부 lease가 있으면 `Busy`를 반환한다. 모든 lease를 반환한 뒤 재호출하면 파생 타입부터 기반 타입 순서로 정리한다. allocator·모듈 정리 callback은 Registry 잠금 밖에서 호출한다. 등록·해제·Shutdown은 생성 스레드 전용이며 Acquire와 서로 다른 lease의 계수 조작은 잠금으로 보호된다. 같은 lease 래퍼의 동시 변경은 허용하지 않는다.

`TypeLease::IsA`와 `TypeRegistry::IsA`는 메타데이터의 상속 관계 조회다. 이를 근거로 native pointer 변환을 허용하지 않는다. 현재 `Resolve<T>`는 일반적인 파생 객체를 기반 C++ 타입으로 변환하는 기능을 제공하지 않는다. 자동 생성한 파생 필드 thunk는 정확한 파생 native type을 Resolve하고, 상속 멤버에 대한 typed member pointer로 접근한다.

## 필드 접근과 실패 보장

`FieldValue`는 이동 전용 소유 값이며 String도 자체 복제한다. kind에 대응하는 payload만 유효한 값으로 읽는다. 같은 FieldValue의 동시 변경이나 allocator callback에서 그 래퍼로 변경 재진입하는 동작은 금지한다. allocator는 값과 진행 중인 호출보다 오래 살아야 한다.

일반 read와 쓰기 가능 필드의 write는 ObjectAccessContext와 ObjectHandle을 받아 Registry에서 owner thread, context, generation, 논리 수명과 정확한 native type을 검사한다. raw object pointer나 StringView를 외부 반환값으로 유출하지 않는다. staged write는 ConstructionContext의 아직 게시되지 않은 객체를 검사한다.

- String read는 임시 소유 값에 복제하고, 할당 callback 이후 Borrow의 논리 유효성을 다시 확인한 뒤 output에 게시한다. OOM 또는 논리 파괴는 기존 output을 유지한다.
- String write는 대상 필드의 allocator로 replacement를 완성하고, 논리 유효성을 다시 검사한 후 할당·해제 없는 Swap으로 commit한다. OOM과 commit 전 논리 파괴는 기존 필드를 유지한다.
- Swap 이후 이전 문자열의 storage 반환 callback에서 객체를 논리 파괴해도 이미 완료한 commit은 성공이다. 일반 write의 Borrow는 이전 storage 정리까지 객체의 물리 수명을 보존한다. 이때 추가 객체 접근을 하지 않는다.
- 정수 폭 축소는 범위를 확인하고 `Overflow`를 반환한다. 타입이 다르면 `TypeMismatch`이며 필드는 변경하지 않는다.
- ObjectRef write는 `ObjectRegistry::SetReference` 또는 `ConstructionContext::SetReference`를 통과한다. owner/target 검증, typed target 규칙, GC barrier를 raw store로 우회하지 않는다.

`ReadOnly`는 등록된 `TypeLease::Ops()`의 일반 write에 적용되는 편집 정책이다. 등록 시 함수표를 복사하면서 해당 write를 공통 거부 callback으로 교체하므로, 입력 schema가 ReadOnly인데 제공된 callback이 쓰기를 수행하는 경우에도 등록된 함수표는 쓰기를 거부한다. owner thread·context와 handle의 생존 여부를 검사하며, 정상 객체에 대해서는 `Unsupported`를 반환한다. 공통 callback의 서명에는 선언 타입 식별자가 없어 다른 유효 native 타입 객체를 전달해도 `TypeMismatch`가 아니라 `Unsupported`를 반환한다. 미게시 객체에 대한 `writeStaged`는 초기화와 복원을 위해 허용한다.

이 정책은 C++ 멤버에 대한 직접 접근이나 등록 전 `GeneratedType<T>::Ops()`의 원본 callback 호출을 차단하는 보안 경계가 아니다. 수동 코드 제공자는 원본 callback과 필드 접근의 사용 범위를 관리해야 한다.
`String::Swap`은 allocator와 storage 소유권도 함께 교환한다. 양쪽 allocator가 각 저장소의 최종 반환보다 오래 살아야 한다. 같은 String의 변경 재진입은 기존 String 계약대로 금지한다.

## 호스트 실행과 빌드 연결

다음 예시는 저장소 루트에서 실행한다.

```text
dotnet build Tools/Rocks.Tools.csproj
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll --input Tests/Reflection/ReflectionFixture.h --include Tests/Reflection/ReflectionFixture.h --output out/Generated/ReflectionFixture.generated.h --compiler clang-cl
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll meta --root . --compiler clang-cl --metacompiler out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll
```
컴파일러는 C++23, 예외·RTTI 비활성화와 저장소의 경고 오류 정책을 적용한다. Windows SDK 환경은 공통 NativeEnvironment가 준비한다. 추가 `--include-dir`, 반복 가능한 `--clang-arg`를 받을 수 있으며, 경고 강등·억제와 response file 우회는 허용하지 않는다. annotation은 Clang 기능이다. 지원하지 않는 컴파일러에서 annotation을 빈 매크로로 바꾸는 fallback은 없다.

C++ 구문은 원문 정규식이 아닌 실제 Clang JSON AST에서 읽는다. 선언의 annotation 인자, 접근 지정, 기반 관계와 컴파일러가 확정한 필드 타입을 사용한다. `--include`는 출력 헤더에 넣을 논리 경로이며 절대 입력 경로는 생성물에 포함하지 않는다.

생성 형식은 `ROCKS_CLANG_FORMAT` 환경변수를 우선 사용하고 clang-format 21.1.6을 요구한다. 같은 입력·타깃 문맥이면 같은 UTF-8/LF 바이트를 생성한다. 기존 출력과 같으면 다시 쓰지 않아 timestamp를 보존한다. 변경된 결과는 같은 디렉터리의 고유 임시 파일에 완성한 뒤 교체한다. `--check`는 비교만 하며 stale 결과를 수정하지 않는다. 서로 다른 생성 설정이 같은 출력을 공유하는 빌드 그래프는 금지하며, 고유 임시 파일 사용이 그런 논리 충돌을 해결하는 것은 아니다.

MSBuild와 C# 빌드 도구는 객체 런타임을 포함하는 구성에서 fixture 생성, 생성 헤더 단독 컴파일과 ReflectionTests를 연결한다. Core-only 구성은 이 생성 작업을 요구하지 않는다. 실제 생성 헤더도 일반 엔진 코드와 같은 경고 오류 기준으로 컴파일한다.

## 현재 한계와 검증 범위

현재 입력은 단일 umbrella 헤더의 번역 단위다. 포함된 reflected 타입을 모두 수집하므로 여러 생성 헤더가 같은 타입의 specialization을 중복 소유하지 않도록 빌드 입력을 구성해야 한다. 모듈별 타입 소유 manifest, compile_commands 기반 정확한 타깃 문맥 복원, 원격·로컬 생성 캐시, depfile 기반 최소 재생성은 아직 지원하지 않는다. 여러 타깃의 define·include 차이는 호출 측에서 정확히 전달해야 한다. 컴파일러나 타깃 문맥이 달라져도 항상 같은 산출물을 보장한다는 의미는 아니다.

런타임 회귀는 scalar/Guid/UTF-8 소유 값, 상속 관계와 native cast 분리, 잘못된 context·thread, staged write와 참조 barrier, typed target 거부, descriptor 함수표의 깊은 복사, String OOM·논리 파괴 재진입, Swap의 allocator 소유권, 등록 OOM 후 기반 의존성 원복을 검사한다. 계수·allocator ledger가 종료 후 0으로 돌아오는지도 확인한다.

C# 하네스는 실제 Clang을 호출하여 정상 상수 평가, 음수·범위 밖 버전, ID 오류·중복, 빠진 annotation, 미지원 필드·상속·Trace, 컴파일 오류와 경고 강등 옵션을 검사한다. 오류가 기존 출력 파일을 훼손하지 않는지, 같은 입력의 바이트·timestamp가 유지되는지, `--check`가 변경 없이 stale 출력을 거부하는지도 검사한다.

이전 구현은 Windows 로컬 Debug·Release·ASan과 [Windows/macOS native CI](https://github.com/maxidea1024/rocks/actions/runs/42775786556)에서 생성 코드 및 실제 Clang 회귀 실행을 확인하였다. 최종 수치·산출물 hash는 [5차 검증 기록](../Reviews/ImplementationRound5.FoundationValidation.ko.md)을 따른다. macOS는 ARM64 Debug·ASan 범위이며 Intel Mac과 TSan은 미검증이다.
