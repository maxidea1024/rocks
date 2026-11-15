#pragma once

#include <Object/ConstructionContext.h>
#include <Object/FieldCodec.h>
#include <Serialization/Record.h>

// 객체 그래프 저장·복원이다(graph v2 — Docs/Design/ObjectFoundations.ko.md 5절). 저장은 호출자가
// 고른 노드 집합(순서 = local ID)을 쓰고, 복원은 NodeTable이 있는 그래프를 한 트랜잭션으로
// 공개한다.
namespace rocks {

// byte 한도는 파일/필드와 migration 결과 누계다. peak RAM은 소유 index·필드 색인·참조 token·output
// 복사도 포함한다. work는 codec의 반복/복사 비용이며 신뢰된 ctor/accessor/migration/hook의 임의
// 코드를 중단하지 않는다. 이 콜백들은 유한 작업만 하고 registry/input/현재 wrapper를 재진입
// 변경하지 않아야 한다.
//
// 기본값의 결과(Docs/Design/ObjectFoundations.ko.md 5.6절·11절 결정 4·5): 값 하나가 64 MiB까지라
// 값 임시 저장의 단일 할당도 64 MiB까지이고, 저장 피크는 필드 writer·Record builder·Build 사본·
// 출력 중 둘이 단계마다 함께 있어 약 2배(약 128 MiB — Record·필드 writer 한도가 남은 파일 byte라
// 노드 Record의 합이 maxBytes로 묶인다, writer 성장 중에는 잠깐 더)다. 로드는 원본을 빌리므로
// 피크가 staged 값 복사와 Required 검사
// 버퍼(Required를 품은 가장 큰 필드 하나의 검사 출력 — 성장 중에는 그 약 2.5배)다.
struct ObjectGraphLimits {
    // 노드 수다(AS export 상한). 저장 집합과 Include가 더한 노드, 복원의 nodeCount에 건다. local
    // ID가 외부 참조 비트(0x80000000)와 겹치지 않게 2^31 − 1 이하여야 한다(넘으면 저장·복원 모두
    // 옵션 오류 InvalidArgument).
    uint32 maxObjects = 4096;
    // 그래프 header의 외부 참조 수(externalCount) 상한이다(AS object import 상한 — 외부 참조 저장·
    // 복원은 A0-Oe).
    uint32 maxExternalReferences = 65536;
    // 원본과 migration 결과를 검증한 TLV 수의 누계다(중첩 struct Record의 TLV 포함). Record 하나를
    // 열 때도 남은 누계를 그 Record의 필드 수 한도로 쓴다(큰 입력이 누계 검사 전에 색인을 할당하지
    // 않게).
    uint32 maxTotalFields = 1024 * 1024;
    // 리플렉션 배열 원소 수의 누계다(Docs/Design/ReflectionFoundations.ko.md 5.5절 — 저장·로드
    // 모두, 원본과 struct 원소 배열 포함). 배열 하나의 원소 수는 따로 MaxArrayElements 이하다.
    // 배열 원소의 강한 참조 token은 non-null 원소마다 하나이므로 원소 참조 token 수의 상한도 이
    // 값이다. 로드 1단계는 struct 원소 목록을 열 때도 남은 누계를 항목 수 한도로 쓴다(색인을
    // 할당하기 전에 같은 CapacityExceeded).
    uint32 maxTotalElements = 4 * 1024 * 1024;
    // 그래프 파일 전체 byte다(AS 비-Bulk 섹션 상한).
    usize maxBytes = 64 * 1024 * 1024;
    // 필드 값 하나의 encoded byte 한도다(배열 필드 payload 전체 포함 — 넘으면 CapacityExceeded).
    usize maxValueBytes = 64 * 1024 * 1024;
    // 작업량 상한이다. 기본값은 8 × (maxTotalFields + maxTotalElements + maxBytes)(약 5.8억)로,
    // 바이트·필드·원소 상한을 각각 채운 입력이 기본 한도 안에서 저장·복원된다(11절 결정 5 — 저장은
    // byte당 약 5–7, 복원은 약 6을 청구한다).
    uint64 maxWork = 8 * (uint64{1024 * 1024} + uint64{4 * 1024 * 1024} + uint64{64 * 1024 * 1024});
    usize maxMigrationBytes = 64 * 1024 * 1024;
};

using ObjectGraphMigration = Result<Array<uint8>> (*)(Allocator&, TypeId type, uint32 sourceVersion,
                                                      uint32 targetVersion,
                                                      Span<const uint8> sourceRecord,
                                                      void* user) noexcept;

// 저장 집합 밖 강한 참조의 처리다(Docs/Design/ObjectFoundations.ko.md 5.2절). 외부 참조 허락
// hook으로 외부 참조를 쓰는 Admit은 A0-Oe가 더한다.
enum class GraphExternalPolicy : uint8 {
    // 저장 집합 밖 참조는 ExternalReference 진단(value 0) 뒤 InvalidArgument다(기본).
    Reject,
    // 저장 집합 끝에 노드로 더한다(발견 순 BFS — 편의 SaveObjectGraph가 쓴다). 더할 대상의 Outer
    // 사슬 중 집합에 없는 조상을 바깥쪽부터 먼저 더한다(조상 순회는 집합 안 조상·기준 Outer(노드
    // 1의 Outer)·Outer 없음에서 멈춘다). 대상이나 그 조상이 기준 Outer·노드 1의 조상이거나, 패키지
    // 정책 Registry에서 대상의 outermost가 노드 1의 것과 다르거나, 노드 1에 기준 Outer가 있는데
    // 사슬이 Outer 없음에 닿으면(다른 트리) ExternalReference 진단 뒤 InvalidArgument다(11절 결정
    // 17).
    Include
};

// 저장 옵션이다. policy.rejectMissingRequired면 Required 참조 null을 RequiredMissing 진단 뒤
// InvalidArgument로 거부하고, policy.excludedFlags의 필드는 저장하지 않는다. hooks는 진단·소프트
// 참조 수집·struct 치환을 받는다(없으면 버린다).
struct ObjectGraphSaveOptions {
    ObjectGraphLimits limits;
    FieldEncodePolicy policy;
    FieldCodecHooks* hooks = nullptr;
    GraphExternalPolicy externals = GraphExternalPolicy::Reject;
    // 참이면 NodeTable(노드마다 타입·Outer·이름)을 쓴다 — 독립 그래프(시험·스냅샷·복제·편의 API).
    // 거짓이면 노드 표는 바깥(A1 패키지의 Exports)이 가진다. NodeTable을 쓰면 모든 노드의 Outer가
    // 그 노드보다 앞의 노드이거나 기준 Outer(노드 1의 Outer — 없으면 Outer 없음)여야 한다(아니면
    // InvalidArgument — 기준 Outer와 다른 집합 밖 Outer, Outer 없음과 기준 Outer의 혼합 포함).
    bool writeNodeTable = false;
};

namespace graph_detail {
struct SaveResultAccess;
}

// 저장 결과다. 그래프 byte와 노드 목록(local ID 순 — Include면 늘어난 집합)을 소유하며 Registry를
// 참조하지 않는다(handle 값만 담는다 — 그 사이 대상이 회수되면 handle은 stale이다). 이동 전용이다.
// 외부 참조 목록·번호 재배정(Externals·AssignExternalIndices)은 A0-Oe가 더한다.
//
// 사용 예:
//   const ObjectHandle set[] = {asset, body, socket};
//   auto saved = SaveObjectGraphSet(allocator, registry, access, set);
//   if (!saved) {
//       return Result<void>::Failure(saved.Error());
//   }
//
//   Array<uint8> bytes = saved.Value().TakeBytes();
class ObjectGraphSaveResult {
public:
    ObjectGraphSaveResult(ObjectGraphSaveResult&&) noexcept = default;
    ObjectGraphSaveResult& operator=(ObjectGraphSaveResult&&) noexcept = default;
    ObjectGraphSaveResult(const ObjectGraphSaveResult&) = delete;
    ObjectGraphSaveResult& operator=(const ObjectGraphSaveResult&) = delete;

    // 그래프 byte다(TakeBytes 뒤에는 비어 있다). 결과가 살아 있는 동안 유효하다.
    Span<const uint8> Bytes() const noexcept {
        return bytes_.View();
    }

    // 그래프 byte의 소유권을 넘긴다(저장에 준 allocator를 빌린다). 이후 Bytes()는 비어 있다.
    Array<uint8> TakeBytes() noexcept {
        return Move(bytes_);
    }

    // 노드 handle이다(local ID − 1 위치).
    Span<const ObjectHandle> Nodes() const noexcept {
        return nodes_.View();
    }

private:
    friend struct graph_detail::SaveResultAccess;

    ObjectGraphSaveResult(Array<uint8>&& bytes, Array<ObjectHandle>&& nodes) noexcept
        : bytes_(Move(bytes)), nodes_(Move(nodes)) {
    }

    Array<uint8> bytes_;
    Array<ObjectHandle> nodes_;
};

// 저장 집합을 graph v2로 쓴다(Docs/Design/ObjectFoundations.ko.md 5.1·5.2절). nodes가 저장 집합이고
// 순서가 local ID다(1부터). 각 노드는 이 Registry의 생존 객체이고 서로 달라야 한다. 저장 집합은
// 참조 순회가 아니라 호출자가 정하므로 어느 노드에서도 닿지 않는 노드(고아)도 쓴다. 소유 스레드
// 전용이다. 노드마다 encode 동안 고정(PinForEncode)하고 첫 노드의 Borrow를 저장 끝까지 쥐어 GC
// slice를 막는다. caller/allocator/accessor/hook callback도 실행 동안 그래프를 변경하지 않는다.
// 마지막에 모든 노드의 생존을 다시 확인한 뒤 결과를 게시한다. 주소/registry
// domain/slot/generation은 파일에 저장하지 않는다(참조는 U32 local ID — 0 = null).
//
// 필드마다 등록된 codec(RuntimeFieldOps::encode)이 값을 쓴다. completeFieldLayout=true(상속 필드
// 포함)와 GUID별 1:1 codec이 없으면 Unsupported다. C++에서 직접 넣은 잘못된 값(미선언 enum, 소프트
// 참조 규칙 위반)은 InvalidArgument다. 배열 필드의 원소 수가 필드 maxElements를 넘으면 그 배열
// 필드 경로의 ArrayTooLong 진단(value = 원소 수)을 hooks에 보고한 뒤 CapacityExceeded다. encoded
// payload가 maxValueBytes를 넘거나 원소 누계가 maxTotalElements를 넘어도 CapacityExceeded다.
//
// 오류 순서(5.7절): 옵션(maxObjects가 2^31 − 1 초과 InvalidArgument) → 입력(노드 0개·null handle
// InvalidArgument → 개수 maxObjects 초과 CapacityExceeded → 중복 InvalidArgument) → 접근·생존(첫
// 노드 Borrow·노드마다 AcquireType — WrongThread·WrongContext·StaleHandle·Destroyed) →
// 배치(ValidateLayout Unsupported) → 노드 encode(필드 wire TypeMismatch, Required InvalidArgument,
// ArrayTooLong CapacityExceeded, 값 InvalidArgument·Overflow, 바이트 상한 CapacityExceeded —
// 노드마다 남은 파일 byte로 누계 검사) 안의 참조(대상 생존 → 기대 형식 TypeMismatch → 집합 밖 정책:
// Reject· Include 거부 조건 InvalidArgument, Include의 노드 수 CapacityExceeded) → NodeTable(Outer
// 순서·기준 Outer 불일치·혼합 InvalidArgument — 모든 노드를 encode한 뒤) → 마지막 생존 재확인.
// 할당 실패는 OutOfMemory다.
//
// 사용 예(패키지의 export 집합 — A1 writer):
//   Array<ObjectHandle> exports(allocator);
//   // CollectInners(DepthFirst)로 대표 에셋과 그 아래 객체를 Outer가 앞서는 순서로 모은다
//   auto saved = SaveObjectGraphSet(allocator, registry, access, exports.View(), options);
Result<ObjectGraphSaveResult> SaveObjectGraphSet(Allocator&, ObjectRegistry&,
                                                 const ObjectAccessContext&,
                                                 Span<const ObjectHandle> nodes,
                                                 ObjectGraphSaveOptions = {}) noexcept;

// 편의 저장이다. root에서 영속 강한 참조로 닿는 객체를 발견 순(metadata 필드 순 BFS)으로 모아
// NodeTable과 함께 쓴다(options의 externals를 Include로, writeNodeTable을 참으로 바꿔
// SaveObjectGraphSet({root})를 부른다). 같은 입력은 같은 byte다. root는 노드 1이고 root의 Outer는
// 기준 Outer가 된다(노드로 넣지 않는다 — NodeTable의 BaseOuter 비트). 결과 buffer만 소유하며
// allocator는 결과 Array보다 오래 살아야 한다. 오류는 SaveObjectGraphSet과 같다.
//
// 사용 예:
//   auto bytes = SaveObjectGraph(allocator, registry, access, level);
//   if (!bytes) {
//       return Result<void>::Failure(bytes.Error());
//   }
Result<Array<uint8>> SaveObjectGraph(Allocator&, ObjectRegistry&, const ObjectAccessContext&,
                                     ObjectHandle root, ObjectGraphSaveOptions = {}) noexcept;

struct ObjectGraphLoadOptions {
    ObjectGraphLimits limits;
    // 미선언 enum·높은 버전·모르는 필드의 처리와 이 데이터에 없어야 하는 플래그(excludedFlags —
    // 쿠킹 로드의 EditorOnly, 있으면 InvalidArgument)다(기본은 미선언 enum·높은 버전 거부, 모르는
    // 필드는 진단 후 건너뛰기). Record에 없는 필드·멤버는 정책이 아니라 형식 규칙으로 기준
    // 값(생성자 값)을 유지한다. ReportAndDefault·LoadNewerSkippingUnknown은 hooks가 있어야
    // 한다(없으면 InvalidArgument — 진단 없이 값을 바꾸지 않는다).
    FieldDecodePolicy policy;
    // 저장 버전이 현재보다 낮을 때만 한 번 호출한다(높으면 policy.newerVersion). callback은 전달
    // allocator로 소유 결과를 반환해야 한다. 반환 record도 현재 schema/version/TLV/값 폭으로 다시
    // 검증한다. callback 자체는 신뢰 경계다.
    ObjectGraphMigration migrate = nullptr;
    // struct의 옛 버전을 올리는 override다(시험·도구용 — type 인자가 struct ID). Unsupported를
    // 돌려주거나 없으면 타입이 등록한 마이그레이션(RuntimeStructOps.migrate)을 쓰고, 둘 다 없으면
    // 그 로드는 Unsupported다. 결과는 정확히 현재 버전이어야 한다.
    ObjectGraphMigration migrateStruct = nullptr;
    void* migrationUser = nullptr;
    // 진단·소프트 참조 수집을 받는다(RequiredMissing은 적용 뒤 Required 검사에서 한 번, 그 밖은
    // 1단계에서 한 번). 실패하면 복원이 실패한다(Required 검사의 실패는 rollback — 객체를 공개하지
    // 않는다). 없으면 Required 검사를 하지 않는다(진단을 받을 곳이 없다).
    FieldCodecHooks* hooks = nullptr;
    // BaseOuter 비트가 있는 그래프의 기준 Outer다(생존 객체 — outer 0 노드의 Outer가 된다).
    // null이면 BaseOuter 비트가 없는 그래프여야 한다(비트와 다르면 InvalidArgument). 외부 참조
    // 대상(imports)은 A0-Oe가 더한다.
    ObjectHandle baseOuter{};
    // 적용·Required 진단 뒤, Commit 전에 부른다(AS 6.1 prepare — Transient 필드·AssetId 설정).
    // nodes는 노드 handle(local ID − 1 위치, 호출 동안 유효)이다. 실패하면 그 오류로 rollback한다.
    // staged 객체만 바꾸고(Registry의 다른 객체를 바꾸지 않는다) 도달성을 깨지 않는다 — 노드 1에서
    // 닿게 하는 staged 참조를 끊지 않고 그 트랜잭션의 Commit·Rollback을 부르지 않는다(도달 검사는
    // 1단계에 끝났으므로 끊긴 노드는 다음 GC가 회수한다).
    Result<void> (*prepare)(ConstructionContext&, Span<const ObjectHandle> nodes,
                            void* user) noexcept = nullptr;
    void* prepareUser = nullptr;
};

// NodeTable이 있는 graph v2를 한 트랜잭션으로 복원하고 노드 1을 root로 Commit한다(Docs/Design/
// ObjectFoundations.ko.md 5.5절). 1단계는 envelope·노드 표·Record·값·참조와 도달성을 검증하고
// (생성자를 실행하지 않는다) 2단계 몫의 작업량을 미리 청구하며, 2단계는 모든 노드를 노드 표의
// Outer·이름으로 비공개 생성해 값을 쓰고 Required 실효 값을 진단한 뒤 prepare를 부르고 한번에
// Commit한다. 2단계 실패(할당·Registry 상태·Create의 이름·정책 규칙·Required 진단 hook 실패·
// readStaged 없는 수동 Required 필드의 Unsupported·Required 위치 자체의 저장 불가 값 — 예: 생성자가
// 둔 Required Soft의 규칙 위반, MaxArrayElements를 넘는 Required 배열·prepare 실패)는 부분 객체
// 공개 없이 역순 rollback한다(노드 표의 이름·정책 규칙을 생성 전에 판정하는 것은 단계형 복원
// A0-Od2다). 할당기 통계/slot generation이 rollback 전 값으로 돌아간다는 뜻은 아니다. 원본 bytes는
// 호출 동안 유효하고 불변이어야 하며 결과는 원본을 빌리지 않는다. registry와 types는 같은 등록
// 저장소여야 한다.
//
// 도달 검사: 노드 1에서 영속 강한 참조, inner → outer(노드 표의 outer), 패키지 정책 Registry면
// outer → inner와 기준 Outer 아래 보존으로 닿지 않는 노드가 있으면 생성 전에 InvalidArgument다(공개
// 뒤 다음 GC가 조용히 지우지 않게). 패키지 정책 Registry에서 기준 Outer가 없으면 노드 1이 아닌
// outer 0 노드(다른 패키지 — root 없이 공개되어 Discard 없이 회수된다)도 InvalidArgument다(여러
// 패키지는 단계형 복원과 Commit(roots) — A0-Od2). 편의 저장이 만든 그래프는 언제나 통과한다.
//
// 값 규칙: 모르는 필드는 policy.unknownField다. nonnull 강한 참조의 대상은 value.type 등록 타입과
// 생성 thunk의 정적 대상 타입의 native 타입이거나 그 파생이어야 하며 TypeId 별명은 허용한다.
// value.type=0은 기반 Object다. nonnull의 명시 대상 타입 미등록은 NotFound다. 소프트 참조는
// 값(에셋 ID + 객체 경로)으로 복원하며 대상을 그래프에 넣거나 찾지 않는다. Record에 없는 영속
// 필드·struct 멤버(배열 원소 struct의 멤버 포함)는 오류가 아니며 생성자 값(기준)을 유지한다(누락
// = 기준). policy.excludedFlags의 필드·멤버가 있으면 InvalidArgument다(생성자 전). Required 위치는
// 거부하지 않고, 적용 뒤·Commit 전에 staged 객체의 실효 값(파일 값이든 기준 값이든)이 null이면
// RequiredMissing을 한 번 보고한다(Required 배열은 null 원소마다 [f[i]], 생성자가 non-null로 둔
// Soft 참조가 빠지면 진단하지 않는다 — hooks가 없으면 검사하지 않는다). 그 검사는 필드별
// MaxElements·SubstituteStruct·Soft 수집을 하지 않고 작업량·크기 한도도 걸지 않는다. 배열 필드는
// 필드별 maxElements를 걸지 않고 배열마다 MaxArrayElements와 그래프 한도(maxValueBytes·
// maxTotalElements·maxTotalFields·work)로만 막는다(상한을 낮춰도 기존 데이터가 열린다).
//
// 오류 순서(5.7절): 저장소 불일치 WrongContext → 옵션(hook 없는 ReportAndDefault·
// LoadNewerSkippingUnknown·maxObjects 2^31 − 1 초과 InvalidArgument) → baseOuter(DescribeOuter —
// 접근·StaleHandle) → envelope(magic InvalidArgument, formatVersion이 2가 아니면 Unsupported,
// 정의되지 않은 flags·NodeTable 없는 BaseOuter·nodeCount 0·길이 부족 InvalidArgument,
// nodeCount·externalCount 상한 CapacityExceeded, NodeTable 노드 타입 0·남는 byte InvalidArgument) →
// 노드 출처(NodeTable 없음·BaseOuter 비트와 baseOuter 불일치·externalCount가 0이 아님 — 외부 참조
// 대상을 줄 수 없다 — InvalidArgument) → 노드(타입 미등록 NotFound → Abstract·생성 함수 없음
// Unsupported → 배치 Unsupported → outer가 자기 번호 이상 InvalidArgument → Record schema가 노드
// 타입과 다름 InvalidArgument, 버전 Unsupported, 마이그레이션, 모르는 필드 정책, Transient 존재
// Unsupported, excludedFlags 존재 InvalidArgument, 값) → 참조(범위 — nodeCount 초과·외부 비트
// InvalidArgument, 형식 TypeMismatch, 기대 타입 미등록 NotFound) → 예산(CapacityExceeded) → 도달
// 검사(패키지 정책의 다른 패키지 포함 InvalidArgument) → 2단계.
//
// 사용 예:
//   ObjectGraphLoadOptions options;
//   options.hooks = &diagnostics;
//   auto root = LoadObjectGraph(allocator, registry, types, access, bytes.View(), options);
//   if (!root) {
//       return Result<void>::Failure(root.Error());
//   }
Result<RootRef<Object>> LoadObjectGraph(Allocator&, ObjectRegistry&, TypeRegistry&,
                                        const ObjectAccessContext&, Span<const uint8>,
                                        ObjectGraphLoadOptions = {}) noexcept;

} // namespace rocks
