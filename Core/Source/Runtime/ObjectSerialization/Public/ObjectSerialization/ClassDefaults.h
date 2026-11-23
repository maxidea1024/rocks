#pragma once

#include <Object/ClassDefaults.h>
#include <Object/PropertyPath.h>
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

} // namespace rocks
