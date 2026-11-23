#pragma once

#include <Core/Containers/Array.h>
#include <Object/ClassDefaults.h>
#include <Object/PropertyPath.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/TypeLease.h>
#include <ObjectSerialization/ObjectGraph.h>

// 클래스 기본값 레코드를 만들고 읽는다(Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 구현
// 명세 — A5a 레코드'). 레코드 자료와 등록 레코드 슬롯은 ObjectRuntime Object/ClassDefaults.h에
// 있고, 인코딩 본문(그래프 저장과 같은 필드 루프·thunk)이 이 모듈에 있어 만드는 쪽이 여기다.
namespace rocks {

class TypeRegistry;

// 레코드 경로 조회 결과다: 그 값의 wire와 payload(레코드 byte 안 — 레코드를 지키는 lease 수명 동안
// 유효).
struct DefaultPayload {
    uint32 wire = 0;
    Span<const uint8> bytes{};
};

// lease 클래스의 기본값 레코드를 얻는다. 처음이면 만든다: 생성자를 Registry 밖 임시 인스턴스 둘에
// 실행해(scratch 저장소 — 동시에 산 채로) 그래프 저장과 같은 필드 루프·thunk로 인코딩하고(원본 모드
// — FieldEncodeContext::RawSource), 두 editor 인코딩이 같은지 본 뒤 첫 인스턴스를 cooked 정책으로
// 한 번 더 인코딩하고 지문을 계산해 게시한다(TypeLease::ResolveClassDefaults — 생성 잠금 안 한 번,
// TypeRegistry allocator로 복사). ObjectRegistry를 받지 않으므로 어느 스레드에서나 부를 수 있다.
// 지문 = SHA-256("RocksClassDefaults" ‖ 구성 byte ‖ ObjectGraphFormatVersion(u32 LE) ‖ 레코드
// byte). 레코드의 객체 참조는 null만이고(non-null 강한·weak·Soft는 NonNullReference), Name은
// None만, ClassRef는 TypeRegistry::Acquire로 판정한다.
//
// 반환: 레코드(lease가 살아 있는 동안 유효). 오류(diagnostic에 이유·경로): 클래스가 아닌 lease
// InvalidArgument, 추상·completeFieldLayout 거짓·생성 코덱 표지 없음 Unsupported, 비결정 생성자·
// non-null 기본 참조 InvalidArgument, 그 밖의 codec 오류(EncodeFailed — 원본 오류), 할당 실패
// OutOfMemory. 결정적 실패는 등록 세대 동안 기억한다(IsRememberedClassDefaultsFailure).
//
// 사용 예(저장 비교 기준):
//   ClassDefaultsDiagnostic why;
//   auto defaults = AcquireClassDefaults(lease, allocator, &why);
//   if (!defaults) { /* why.reason: 레코드 없음이면 전부 저장 */ }
//   Span<const uint8> base = defaults.Value()->Record(ClassDefaultsConfiguration::Editor);
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<const ClassDefaults*>
AcquireClassDefaults(const TypeLease& lease, Allocator& scratch,
                     ClassDefaultsDiagnostic* diagnostic = nullptr) noexcept;

// 레코드에서 path(lease 스키마로 해석한 경로 — ParsePropertyPath)의 값 wire·payload를 찾는다. 필드
// → 비Atomic struct 멤버 사슬까지 내려간다(Atomic struct는 그 struct 경로까지 — 멤버 경로는
// InvalidArgument). 원소·key 단계(배열·정적 배열·Map·Set·Optional 안쪽)는 기준 위치가 없어
// Unsupported다(되돌리기는 그 필드 통째 — RuntimeClassMetadata 'A5a 레코드'). 레코드에 없는 경로
// (cooked 구성의 EditorOnly 경로 포함)는 NotFound, struct가 아닌 값 뒤의 단계는 TypeMismatch, 빈
// 경로는 InvalidArgument다. scratch는 Record 색인에 쓴다.
//
// 사용 예(A5d 계약 설정 판정 — 필드 payload를 기준과 비교):
//   auto base = FindDefaultPayload(lease, *defaults, path, ClassDefaultsConfiguration::Editor,
//                                  allocator);
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<DefaultPayload>
FindDefaultPayload(const TypeLease& lease, const ClassDefaults& defaults,
                   const PropertyPathBuffer& path, ClassDefaultsConfiguration configuration,
                   Allocator& scratch) noexcept;

// 레코드 유무 분류다(RuntimeClassMetadata 'A5c 지문 연결'): 있음(지문 있음), 레코드 없는
// 타입(IsClassWithoutDefaults — 정상), 실패(그 밖의 기억되는 이유 — 타입 계약 위반).
enum class ClassDefaultsAvailability : uint8 {
    Present,
    WithoutRecord,
    Failed
};

struct ClassDefaultsStatus {
    ClassDefaultsAvailability availability = ClassDefaultsAvailability::Failed;
    // Present일 때 레코드다(lease 수명 동안 유효).
    const ClassDefaults* defaults = nullptr;
    // WithoutRecord·Failed일 때 이유다.
    ClassDefaultsDiagnostic diagnostic{};
};

// lease 클래스를 AcquireClassDefaults로 분류한다. 패키지 writer(Summary ClassDefaults 행)·
// reader(지문 판정)·쿠커(cookKey 상태)·class-defaults-dump가 같은 분류를 쓴다.
//
// 반환: 분류. 오류: 기억하지 않는 일시 실패(OutOfMemory·Busy·ShuttingDown·NotFound 등 —
// IsRememberedClassDefaultsFailure가 거짓)는 그 오류다(다시 부르면 만들 수 있다).
//
// 사용 예:
//   auto status = ClassifyClassDefaults(lease, allocator);
//   if (!status) { return status.Error(); }
//   if (status.Value().availability == ClassDefaultsAvailability::Present) {
//       const Sha256Digest digest =
//           status.Value().defaults->Fingerprint(ClassDefaultsConfiguration::Cooked);
//   }
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<ClassDefaultsStatus>
ClassifyClassDefaults(const TypeLease& lease, Allocator& scratch) noexcept;

// ForEachClassDefaults가 구체 클래스마다 부르는 함수다. 성공이면 defaults(호출 동안만 유효)가 있고
// diagnostic.reason이 None, 실패면 defaults가 null이고 diagnostic에 이유가 있다. 실패를 돌려주면
// 순회를 그 오류로 멈춘다.
using ClassDefaultsVisitor = Result<void> (*)(void* user, TypeId type,
                                              const ClassDefaults* defaults,
                                              const ClassDefaultsDiagnostic& diagnostic) noexcept;

// 등록된 구체 클래스(추상은 뺀다)마다 레코드를 만들어 visitor에 넘긴다(쿠킹 시작·CI 시험 — 실패를
// 미리 드러낸다). 등록 목록을 TypeRegistry 잠금 아래에서 복사한 뒤 하나씩 Acquire해 lease를 쥐므로
// 어느 스레드에서나 부를 수 있다(순회 사이에 해제된 타입은 건너뛴다). 순서는 목록 순서(TypeId
// 순)다. 오류: 목록 복사·Acquire의 오류(NotFound 말고), visitor의 오류.
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<void>
ForEachClassDefaults(TypeRegistry& types, Allocator& scratch, ClassDefaultsVisitor visitor,
                     void* user) noexcept;

// 되돌리기 계획의 경로 쓰기 연산 하나다(RuntimeClassMetadata 'A5b2 decode 명세'). kind에 따라
// 기존 경로 API 하나로 재생한다: Write = PathWrite(value), ResetOptional = PathResetOptional,
// EmplaceOptional = PathEmplaceOptional(이어지는 멤버 Write가 값을 채운다), ClearInstanced =
// PathClearInstanced. 경로는 필드 단계만이다(원소·key 단계 없음). 이동 전용이다.
struct RevertOperation {
    enum class Kind : uint8 {
        Write,
        ResetOptional,
        EmplaceOptional,
        ClearInstanced
    };

    explicit RevertOperation(Allocator& allocator) noexcept : value(allocator) {
    }

    RevertOperation(RevertOperation&&) noexcept = default;
    RevertOperation& operator=(RevertOperation&&) noexcept = default;

    // 연산의 경로 view다(이 연산이 사는 동안 유효).
    PropertyPath Path() const noexcept {
        return PropertyPath{Span<const PathStep>(steps, stepCount)};
    }

    Kind kind = Kind::Write;
    PathStep steps[MaxPropertyPathSteps]{};
    usize stepCount = 0;
    // Write의 값이다(그 밖의 kind는 비어 있다).
    FieldValue value;
};

// 되돌리기 계획이다: 대상 객체와 순서대로 재생할 연산들. PlanRevert가 채우고 ApplyRevert가 쓴다.
struct RevertPlan {
    explicit RevertPlan(Allocator& allocator) noexcept : operations(allocator) {
    }

    ObjectHandle object{};
    Array<RevertOperation> operations;
};

// object의 path(필드 → struct 멤버 단계만 — ParsePropertyPath로 해석)를 클래스 기본값(Editor
// 레코드)으로 되돌리는 계획을 만든다. 대상 값은 바꾸지 않는다: 레코드의 그 경로 payload를 binary로
// 해석해(텍스트 왕복 없음) 경로 쓰기 연산으로 펼친다 — 단일 값·bitfield·Optional 단일 값은 Write,
// 비Atomic·Atomic struct는 멤버마다(Transient 제외), Optional<struct>는 unset이면 ResetOptional,
// set이면 EmplaceOptional + 멤버, InstancedStruct는 ClearInstanced(기준은 늘 비어 있다). 참조
// 기준은 null, Name은 대상 Registry 풀로 intern, ClassRef는 지금 TypeRegistry에서 Base 파생을 다시
// 본다.
//
// 오류(실패하면 out.operations는 비어 있다): 빈 경로·단계 수 초과 InvalidArgument, 원소·key 단계와
// 1차 범위 밖(배열·Set·Map·정적 배열과 그것을 품은 struct 경로) Unsupported, 경로 위 어디든
// ReadOnly 유효 플래그 Unsupported(되돌리기 전체 거부),
// AcquireType·AcquireClassDefaults(diagnostic에 이유)· FindDefaultPayload의 오류, Name intern 실패,
// ClassRef 재판정 실패(NotFound·InvalidArgument), 할당 실패 OutOfMemory. scratch는 레코드 색인·연산
// 목록·값 복사에 쓴다(out도 그 allocator여야 한다).
//
// 사용 예(asset-set --reset=: 모든 경로를 먼저 계획한 뒤 적용):
//   RevertPlan plan(allocator);
//   if (auto planned = PlanRevert(registry, access, asset, path, allocator, plan); !planned) {
//       return planned;
//   }
//
//   auto applied = ApplyRevert(registry, access, plan);
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<void>
PlanRevert(ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           const PropertyPathBuffer& path, Allocator& scratch, RevertPlan& out,
           ClassDefaultsDiagnostic* diagnostic = nullptr) noexcept;

// 계획의 연산을 순서대로 경로 API로 재생한다. 연산마다 원자적이지만 계획 전체는 원자적이지 않다
// (중간 실패면 앞 연산은 이미 적용됐다 — 사전 단계가 ReadOnly·범위·값 판정을 끝냈으므로 남는
// 실패는 경로 API의 수명·고정·할당 오류다). failure가 있으면 실패한 연산의 사유를 채운다.
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<void>
ApplyRevert(ObjectRegistry& registry, const ObjectAccessContext& access, const RevertPlan& plan,
            PropertyPathFailure* failure = nullptr) noexcept;

// PlanRevert 뒤 ApplyRevert다(경로 하나를 바로 되돌릴 때).
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<void>
RevertToDefault(ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                const PropertyPathBuffer& path, Allocator& scratch,
                PropertyPathFailure* failure = nullptr,
                ClassDefaultsDiagnostic* diagnostic = nullptr) noexcept;

} // namespace rocks
