#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Object/ConstructionContext.h>
#include <Object/FieldCodec.h>
#include <Serialization/Record.h>

// 객체 그래프 저장·복원이다(graph v2 — Docs/Design/ObjectFoundations.ko.md 5절). 저장은 호출자가
// 고른 노드 집합(순서 = local ID)을 쓰고, 복원은 단계형(ObjectGraphStage — 검증은 어느 스레드나,
// 생성·적용·Required 검사는 소유 스레드, Commit은 호출자)과 그 위의 편의 API(LoadObjectGraph —
// NodeTable이 있는 그래프를 한 트랜잭션으로 공개)다.
namespace rocks {

// byte 한도는 파일/필드와 migration 결과 누계다. peak RAM은 소유 index·필드 색인·참조 token·output
// 복사도 포함한다. work는 codec의 반복/복사 비용이며 신뢰된 ctor/accessor/migration/hook의 임의
// 코드를 중단하지 않는다. 이 콜백들은 유한 작업만 하고 registry/input/현재 wrapper를 재진입
// 변경하지 않아야 한다.
//
// 기본값의 결과(Docs/Design/ObjectFoundations.ko.md 5.6절·11절 결정 4·5): 값 하나가 64 MiB까지라
// 값 임시 저장의 단일 할당도 64 MiB까지이고, 저장 피크는 필드 writer·Record builder·Build 사본·
// 출력 중 둘이 단계마다 함께 있어 약 2배(약 128 MiB — Record·필드 writer 한도가 남은 파일 byte라
// 노드 Record의 합이 maxBytes로 묶인다, writer 성장 중에는 잠깐 더)에 외부 참조 위치 표(참조
// 하나에 4 byte — 출력 byte와 합쳐 maxBytes 안)다. 로드는 원본을 빌리므로 피크가 staged 값 복사와
// Required 검사 버퍼(Required를 품은 가장 큰 필드 하나의 검사 출력 — 성장 중에는 그 약 2.5배)다.
struct ObjectGraphLimits {
    // 노드 수다(AS export 상한). 저장 집합과 Include가 더한 노드, 복원의 nodeCount에 건다. local
    // ID가 외부 참조 비트(0x80000000)와 겹치지 않게 2^31 − 1 이하여야 한다(넘으면 저장·복원 모두
    // 옵션 오류 InvalidArgument).
    uint32 maxObjects = 4096;
    // 외부 참조 수 상한이다(AS object import 상한): 저장의 서로 다른 외부 대상 수(넘으면
    // CapacityExceeded)와 AssignExternalIndices의 count, 복원의 header externalCount(넘으면
    // CapacityExceeded)에 건다. 외부 번호가 외부 참조 비트와 겹치지 않게 2^31 − 1 이하여야 한다
    // (넘으면 저장·복원 모두 옵션 오류 InvalidArgument).
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
    // 그래프 파일 전체 byte다(AS 비-Bulk 섹션 상한). 저장은 외부 참조 위치 표 메모리(참조 하나에
    // 4 byte)를 출력 byte와 합쳐 이 값과 비교한다.
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

// 저장 집합 밖 강한 참조의 처리다(Docs/Design/ObjectFoundations.ko.md 5.2·5.3절). 어느 정책이든
// 대상 생존·기대 형식 검사(StaleHandle·Destroyed·TypeMismatch)를 먼저 한다.
enum class GraphExternalPolicy : uint8 {
    // 저장 집합 밖 참조는 ExternalReference 진단(value 0) 뒤 InvalidArgument다(기본).
    Reject,
    // hooks->AdmitExternalReference가 정한다(hooks가 없으면 Reject): Import면 외부 참조
    // 0x80000000 | slot(slot은 대상의 첫 출현 순 1부터 — 같은 대상은 같은 slot, 두 번째 출현부터는
    // hook을 부르지 않는다)으로 쓰고 값 위치를 기록한다(결과의 Externals·AssignExternalIndices),
    // Null이면 그 위치를 0으로 쓰고 ExternalReference 진단(value 1)을 남기며(Required 위치면 null에
    // Required 규칙 적용), Reject면 진단(value 0) 뒤 InvalidArgument다. 서로 다른 외부 대상이
    // maxExternalReferences를 넘으면 CapacityExceeded다.
    Admit,
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
// 참조 수집·struct 치환·외부 참조 허락(Admit)을 받는다(없으면 버리고, Admit은 모두 Reject다).
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

// 외부 참조 대상 하나다(저장의 Admit 정책이 Import로 쓴 대상 — slot 순서 = 첫 출현 순,
// Docs/Design/ObjectFoundations.ko.md 5.3절). A1이 ObjectImports·Dependencies를 만든다.
struct GraphExternal {
    ObjectHandle target{};
    // 대상의 실제 등록 타입이다.
    TypeId type{};
    // 이 대상을 가리키는 참조 값의 수다(배열 원소는 원소마다).
    uint32 occurrences = 0;
    // 모든 출현 위치 유효 플래그의 AND다(예: EditorOnly 위치에서만 쓰였는지 — A1 Dependencies
    // 종류).
    uint32 commonFlags = NumericLimits<uint32>::Max();
    // Required 위치에서 한 번이라도 쓰였는지다(A1 Dependencies.required).
    bool required = false;
};

namespace graph_detail {
struct SaveResultAccess;
}

// 저장 결과다. 그래프 byte·노드 목록(local ID 순 — Include면 늘어난 집합)·외부 참조 목록(slot 순)과
// 외부 참조 값의 위치 표(참조 하나에 u32 출력 byte offset)를 소유하며 Registry를 참조하지 않는다
// (handle 값만 담는다 — 그 사이 대상이 회수되면 handle은 stale이다). 이동 전용이다. 위치 표 덕에
// 외부 번호를 다시 매겨 제자리로 덮어쓸 수 있다(AssignExternalIndices — 11절 결정 7).
//
// 사용 예(A1 writer — import 순서로 번호를 다시 매긴 뒤 지문을 계산한다):
//   auto saved = SaveObjectGraphSet(allocator, registry, access, exports.View(), options);
//   if (!saved) {
//       return Result<void>::Failure(saved.Error());
//   }
//
//   // Externals()로 ObjectImports를 (assetId, 객체 경로) 순으로 정렬하고 outer import를 더해
//   // indexBySlot(slot k → import 번호)과 import 수 count를 만든다
//   if (auto assigned = saved.Value().AssignExternalIndices(indexBySlot.View(), count); !assigned)
//   {
//       return assigned;
//   }
//
//   Array<uint8> bytes = saved.Value().TakeBytes(); // CRC·SHA-256은 이 byte로 계산한다
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

    // 그래프 byte의 소유권을 넘긴다(저장에 준 allocator를 빌린다). 이후 Bytes()는 비어 있고
    // AssignExternalIndices는 InvalidArgument다(노드·외부 참조 목록은 남는다). 재배정을 더 할 수
    // 없으므로 위치 표와 역표를 이때 반환한다.
    Array<uint8> TakeBytes() noexcept {
        positions_.Empty();
        slotByIndex_.Empty();
        return Move(bytes_);
    }

    // 노드 handle이다(local ID − 1 위치).
    Span<const ObjectHandle> Nodes() const noexcept {
        return nodes_.View();
    }

    // 외부 참조 대상이다(slot − 1 위치, slot = 첫 출현 순). Admit 정책이 아니면 비어 있다. 결과가
    // 살아 있는 동안 유효하다.
    Span<const GraphExternal> Externals() const noexcept {
        return externals_.View();
    }

    // slot k의 모든 참조 값을 0x80000000 | indexBySlot[k − 1]로 제자리에서 바꾸고 header
    // externalCount를 count로 쓴다(Docs/Design/ObjectFoundations.ko.md 5.3절). count는 slot 수보다
    // 클 수 있다(A1의 outer import — 어떤 참조도 가리키지 않는 번호). 다시 불러도 된다(지금
    // 번호에서 새 번호로 바꾼다). CRC·SHA-256 같은 바이트 지문은 이 호출 뒤에 계산한다(A1 5.5 저장
    // 순서).
    //
    // 인자: indexBySlot(크기 = Externals().Size(), 1..count의 서로 다른 값), count(header
    // externalCount — 복원의 importTypes 크기).
    //
    // 순서와 오류(실패하면 byte·번호는 그대로다): TakeBytes 뒤·indexBySlot 크기가 slot 수와 다름
    // InvalidArgument → count가 maxExternalReferences(저장 옵션의 값) 초과 CapacityExceeded → 검사
    // 표 할당 OutOfMemory → 값이 0·count 초과·중복 InvalidArgument. 작업량은 위치 수 + slot 수 +
    // count(역표 초기화)에 선형이다(그래프 예산 밖 — 저장이 끝난 뒤다).
    Result<void> AssignExternalIndices(Span<const uint32> indexBySlot, uint32 count) noexcept;

private:
    friend struct graph_detail::SaveResultAccess;

    ObjectGraphSaveResult(Array<uint8>&& bytes, Array<ObjectHandle>&& nodes,
                          Array<GraphExternal>&& externals, Array<uint32>&& positions,
                          uint32 maxExternalReferences) noexcept
        : bytes_(Move(bytes)), nodes_(Move(nodes)), externals_(Move(externals)),
          positions_(Move(positions)), slotByIndex_(externals_.GetAllocator()),
          maxExternalReferences_(maxExternalReferences) {
    }

    Array<uint8> bytes_;
    Array<ObjectHandle> nodes_;
    Array<GraphExternal> externals_;
    // 외부 참조 값(u32)의 출력 byte offset이다(출현 순 — 값의 slot은 그 자리 값에서 읽는다).
    Array<uint32> positions_;
    // 마지막 AssignExternalIndices의 역표(외부 번호 − 1 → slot, 0 = slot 없음)다. 비어 있으면
    // 아직 재배정하지 않아 외부 번호가 곧 slot이다.
    Array<uint32> slotByIndex_;
    uint32 maxExternalReferences_ = 0;
};

// 저장 집합을 graph v2로 쓴다(Docs/Design/ObjectFoundations.ko.md 5.1·5.2절). nodes가 저장 집합이고
// 순서가 local ID다(1부터). 각 노드는 이 Registry의 생존 객체이고 서로 달라야 한다. 저장 집합은
// 참조 순회가 아니라 호출자가 정하므로 어느 노드에서도 닿지 않는 노드(고아)도 쓴다. 소유 스레드
// 전용이다. 노드마다 encode 동안 고정(PinForEncode)하고 첫 노드의 Borrow를 저장 끝까지 쥐어 GC
// slice를 막는다. caller/allocator/accessor/hook callback도 실행 동안 그래프를 변경하지 않는다.
// 마지막에 모든 노드와 외부 참조 대상의 생존을 다시 확인한 뒤 결과를 게시한다. 주소/registry
// domain/slot/generation은 파일에 저장하지 않는다(참조는 U32 — 0 = null, 1..노드 수 = local ID,
// 0x80000000 | i = 외부 참조 i).
//
// 필드마다 등록된 codec(RuntimeFieldOps::encode)이 값을 쓴다. completeFieldLayout=true(상속 필드
// 포함)와 GUID별 1:1 codec이 없으면 Unsupported다. C++에서 직접 넣은 잘못된 값(미선언 enum, 소프트
// 참조 규칙 위반)은 InvalidArgument다. 배열 필드의 원소 수가 필드 maxElements를 넘으면 그 배열
// 필드 경로의 ArrayTooLong 진단(value = 원소 수)을 hooks에 보고한 뒤 CapacityExceeded다. encoded
// payload가 maxValueBytes를 넘거나 원소 누계가 maxTotalElements를 넘어도 CapacityExceeded다.
//
// 오류 순서(5.7절): 옵션(maxObjects·maxExternalReferences가 2^31 − 1 초과 InvalidArgument) →
// 입력(노드 0개·null handle InvalidArgument → 개수 maxObjects 초과 CapacityExceeded → 중복
// InvalidArgument) → 접근·생존(첫 노드 Borrow·노드마다 AcquireType — WrongThread·WrongContext·
// StaleHandle·Destroyed) → 배치(ValidateLayout Unsupported) → 노드 encode(필드 wire TypeMismatch,
// Required InvalidArgument, ArrayTooLong CapacityExceeded, 값 InvalidArgument·Overflow, 바이트
// 상한 CapacityExceeded — 노드마다 남은 파일 byte와 외부 참조 위치 표로 누계 검사) 안의 참조(대상
// 생존 → 기대 형식 TypeMismatch → 집합 밖 정책: Reject·Include 거부 조건·Admit hook의 Reject
// InvalidArgument(hook 실패는 그 오류, Null은 진단 뒤 null — Required 위치면 Required 규칙),
// 서로 다른 외부 대상 수 CapacityExceeded, Include의 노드 수 CapacityExceeded, 외부 참조를
// 직렬화기가 준 필드 writer 밖에 씀 InvalidArgument, 위치 표 포함 바이트 상한 CapacityExceeded)
// → NodeTable(Outer 순서·기준 Outer 불일치·혼합 InvalidArgument — 모든 노드를 encode한 뒤) →
// 마지막 생존 재확인(노드·외부 참조 대상 — StaleHandle·Destroyed). 할당 실패는 OutOfMemory다.
//
// 사용 예(패키지의 export 집합 — A1 writer, 다른 패키지 객체는 외부 참조):
//   Array<ObjectHandle> exports(allocator);
//   // CollectInners(DepthFirst)로 대표 에셋과 그 아래 객체를 Outer가 앞서는 순서로 모은다
//   ObjectGraphSaveOptions options;
//   options.externals = GraphExternalPolicy::Admit;
//   options.hooks = &writerHooks; // AdmitExternalReference: 다른 패키지면 Import
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

// 복원 옵션이다. limits·policy·migrate·migrateStruct·migrationUser·hooks는 단계형 복원
// (ObjectGraphStage)과 편의 복원(LoadObjectGraph)이 같이 쓰고, baseOuter·imports·prepare·
// prepareUser는 편의 복원 전용이다(단계형 복원은 무시한다 — 기준 Outer는 CreateObjects 인자, 외부
// 참조 대상은 ObjectGraphInput::importTypes와 ApplyFields 인자, prepare는 호출자가 Commit 전에
// 직접 한다).
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
    // 않는다). 없으면 Required 검사를 하지 않는다(진단을 받을 곳이 없다). 단계형 복원에서 1단계
    // hook은 Validate를 부른 스레드에서, RequiredMissing은 소유 스레드(CheckRequired)에서 불린다
    // (ObjectGraphStage 주석의 스레드 계약).
    FieldCodecHooks* hooks = nullptr;
    // 편의 복원 전용: BaseOuter 비트가 있는 그래프의 기준 Outer다(생존 객체 — outer 0 노드의
    // Outer가 된다). null이면 BaseOuter 비트가 없는 그래프여야 한다(비트와 다르면 InvalidArgument).
    ObjectHandle baseOuter{};
    // 편의 복원 전용: 외부 참조 대상이다(i − 1 위치가 외부 참조 i — 크기는 그래프 header의
    // externalCount와 같아야 한다, 아니면 InvalidArgument). 모두 생존 객체여야 하고(아니면
    // StaleHandle — 파괴 요청 대상 포함) 선언 타입은 대상의 실제 타입이다(단계형 입력의
    // importTypes를 이것으로 만들고, 1단계가 기록한 기대 형식을 생성자 전에 실제 타입으로
    // 판정한다). 배열은 호출 동안 유효해야 한다.
    Span<const ObjectHandle> imports;
    // 편의 복원 전용: 적용·Required 진단 뒤, Commit 전에 부른다(AS 6.1 prepare — Transient 필드·
    // AssetId 설정). nodes는 노드 handle(local ID − 1 위치, 호출 동안 유효)이다. 실패하면 그 오류로
    // rollback한다.
    // staged 객체만 바꾸고(Registry의 다른 객체를 바꾸지 않는다) 도달성을 깨지 않는다 — 노드 1에서
    // 닿게 하는 staged 참조를 끊지 않고 그 트랜잭션의 Commit·Rollback을 부르지 않는다(도달 검사는
    // 1단계에 끝났으므로 끊긴 노드는 다음 GC가 회수한다).
    Result<void> (*prepare)(ConstructionContext&, Span<const ObjectHandle> nodes,
                            void* user) noexcept = nullptr;
    void* prepareUser = nullptr;
};

// 단계형 복원의 노드 설명 하나다(A1이 패키지 Exports 표에서 만든다 — NodeTable이 없는 그래프의
// 노드 표, Docs/Design/ObjectFoundations.ko.md 5.4절).
struct GraphNodeDesc {
    // 노드 객체의 등록 타입이다(0이면 InvalidArgument). 노드 Record의 schema와 같아야 한다.
    TypeId type{};
    // 0 = 그래프 밖(hasBase면 기준 Outer, 아니면 Outer 없음 — AS 5.1 Exports outerIndex 0과 같다),
    // 1..자기 번호 − 1 = 노드(Outer가 Inner보다 앞이다).
    uint32 outer = 0;
    // 이름이다(빈 문자열은 이름 없음 또는 대표 자식 — 1.1절 규칙). byte는 Stage 수명 동안 유효해야
    // 한다(Stage가 빌린다).
    StringView name{};
};

// 단계형 복원 입력이다(ObjectGraphStage::Validate 인자).
struct ObjectGraphInput {
    // graph v2 byte다. Stage 수명 동안 유효하고 불변이어야 한다(ApplyFields가 1단계의 필드 view로
    // 다시 읽는다 — 복원한 객체는 빌리지 않는다).
    Span<const uint8> bytes;
    // 노드 표다(local ID − 1 위치, 크기 = nodeCount). 비면 그래프의 NodeTable을 쓴다(둘 다 있거나
    // 둘 다 없으면 InvalidArgument). 배열은 Validate 동안, 이름 byte는 Stage 수명 동안 유효해야
    // 한다.
    Span<const GraphNodeDesc> nodes;
    // outer 0 노드의 Outer가 기준 Outer인지다. NodeTable이면 BaseOuter 비트와 같아야 하고(아니면
    // InvalidArgument), CreateObjects의 baseOuter 유무와도 같아야 한다.
    bool hasBase = false;
    // 기준 Outer의 요약이다(hasBase일 때만 쓴다 — 이미 있는 객체면 DescribeOuter 결과, 곧 만들
    // 패키지면 OuterSummary::ForNewOutermost(true)). 1단계 정체성 규칙의 입력이고 CreateObjects가
    // 실제 기준 Outer의 isOutermost·outermostIsPackage·pathBytes와 대조한다. pathBytes가
    // MaxObjectPathBytes를 넘으면 InvalidArgument다.
    OuterSummary base;
    // 외부 참조 i의 선언 타입이다(i − 1 위치, 크기 = 그래프 header의 externalCount — A1은
    // ObjectImports의 typeId). 크기가 다르거나 0인 원소가 있으면 InvalidArgument, 등록 클래스가
    // 아니면 NotFound·TypeMismatch다. 1단계는 외부 참조 위치의 기대 형식과 양립하는지만 보고(한쪽이
    // 다른 쪽의 native 파생), ApplyFields가 실제 대상이 선언 타입의 파생인지와 기록한 기대 형식을
    // 만족하는지를 판정한다. 배열은 Validate 동안 유효해야 한다.
    Span<const TypeId> importTypes;
};

// 외부 참조 하나의 쓰임이다(1단계가 그래프 안 외부 참조 값으로 모은다 — 저장의 GraphExternal과 같은
// 집계 규칙, A1 Dependencies 대조용).
struct GraphExternalUse {
    // 이 외부 참조를 가리키는 non-null 참조 값의 수다(배열 원소는 원소마다, 0이면 쓰이지 않는 번호
    // — A1의 outer import 등).
    uint32 occurrences = 0;
    // 모든 출현 위치 유효 플래그의 AND다(출현이 없으면 모든 비트).
    uint32 commonFlags = NumericLimits<uint32>::Max();
    // Required 위치에서 한 번이라도 쓰였는지다.
    bool required = false;
};

namespace graph_detail {
struct StageData;
struct StageAccess;
} // namespace graph_detail

// 단계형 그래프 복원이다(Docs/Design/ObjectFoundations.ko.md 5.4절 — A1 SCC 로드의 allocate-all →
// deserialize-all → fixup-all → publish와 worker 해석). 그래프 하나를 Validate(1단계 — 어느
// 스레드나) → CreateObjects(2a) → ApplyFields(2b) → CheckRequired(2c) 순서로 한 번씩 진행하고,
// Commit은 호출자가 ConstructionContext::Commit(roots, out)으로 한다. 여러 Stage(그래프)를 한
// 트랜잭션에 올려 함께 공개할 수 있고(하드 순환 SCC — 모든 Stage의 CreateObjects 뒤에 모든 Stage의
// ApplyFields), 같은 트랜잭션에 올린 다른 그래프의 객체는 ConstructionContext::FindObject로 찾아
// 외부 참조 대상(ApplyFields의 imports)으로 준다. 고아 노드를 허용한다(도달 검사는 편의 복원만 —
// 호출자가 root·Outer로 붙잡는다).
//
// 상태 기계: 단계마다 먼저 호출 검사(생성자가 기억한 오류, 진행 중인 같은 Stage에 다시 들어오면
// Busy, 순서 위반 InvalidArgument, 소유 스레드·Registry·접근 문맥, 트랜잭션 일치·상태)를 하고, 그
// 실패는 아무 상태도 바꾸지 않는다(잘못된 호출 하나가 유효한 Stage를 망치지 않는다 — 올바른 호출로
// 계속할 수 있다). 호출 검사를 통과하면 Stage는 진행 중이 되고(그동안 생성자·hook·allocator 콜백이
// 같은 Stage의 단계를 부르면 Busy), 성공하면 다음 단계로, 실패하면 실패 상태가 되어 이후 모든
// 단계가 InvalidArgument다(다시 시도하지 않는다 — 새 Stage를 만든다). 트랜잭션에 객체를 넣은 뒤의
// 실패(CreateObjects 도중의 Create 실패, ApplyFields의 imports 확인·쓰기 실패, CheckRequired의
// 실패)는 그 트랜잭션을 그 오류로 고정한다(ConstructionContext::Fail — 이후 Commit이 실패하고
// Rollback·소멸자가 되돌린다, 부분 공개 없음). 그래서 한 트랜잭션에 올린 Stage 하나의 실패가
// 트랜잭션 전체를 실패시킨다.
// CreateObjects의 사전 확인 실패는 객체를 만들지 않았으므로 트랜잭션을 고정하지 않는다. registry와
// types가 같은 등록 저장소가 아니거나 생성자를 소유 스레드·문맥 밖에서 부르면 모든 단계가 그
// 오류(WrongContext·WrongThread)다.
//
// 스레드: 생성자·CreateObjects·ApplyFields·CheckRequired는 Registry 소유 스레드 전용이다
// (WrongThread). Validate는 ObjectRegistry에 닿지 않으므로 어느 스레드에서나 부를 수 있다(Registry
// 정책은 생성자가 사본으로 두고, TypeRegistry 조회·lease 계수는 잠금으로 보호된다). Validate를 부른
// 스레드에서 hooks(진단·Soft 수집·관찰)·마이그레이션 콜백(migrate·migrateStruct·등록 struct
// 마이그레이션)·등록 codec의 1단계 해석(객체·Registry에 닿지 않는 검증)이 실행되고 allocator가
// 쓰인다. 그래서 worker에서 부르는 호출자는 그 스레드에서 안전한 hook·마이그레이션·allocator를
// 준다(소유 스레드나 다른 worker와 동시에 쓰일 수 있으면 스레드 안전해야 한다). RequiredMissing
// 진단은 CheckRequired에서 소유 스레드로 온다. 한 Stage의 단계를 동시에 부르지 않으며, 단계 사이의
// 스레드 넘김(worker 완료 → 소유 스레드)은 호출자가 동기화한다(작업 완료 대기 등).
//
// 수명: Stage는 입력 byte·노드 이름을 빌리고 노드 타입·기대 타입 lease를 쥔다(소멸 때 반환).
// allocator·registry·types는 Stage보다 오래 살아야 한다. options가 가리키는 hooks·migrationUser
// (와 그 사용자 데이터)는 Stage의 마지막 단계(CheckRequired)까지 살아야 한다 — worker 지역 hooks를
// Validate 뒤에 버리면 안 된다(RequiredMissing이 CheckRequired에서 온다). allocator는 스레드 사이의
// 할당·해제를 지원해야 한다(Validate가 worker에서 할당한 노드 상태를 소유 스레드가 2a–2c에서 쓰고
// Stage 소멸 때 해제한다 — worker 전용 arena는 안 된다). 트랜잭션이 열려 있는 동안 GC가 진행하지
// 않으므로 Nodes()의 handle은 그 트랜잭션의 Commit·Rollback까지 공개 전 객체로 유효하다.
//
// 사용 예(A1 로더 — 5.5절 흐름):
//   ObjectGraphStage stage(allocator, registry, types, access, options); // 소유 스레드
//   ObjectGraphInput input;
//   input.bytes = objectsSection;
//   input.nodes = exportNodes;                        // Exports 표에서 만든 노드 설명
//   input.hasBase = true;
//   input.base = OuterSummary::ForNewOutermost(true); // 곧 만들 Package
//   input.importTypes = importTypes;                  // ObjectImports의 typeId
//   if (auto valid = stage.Validate(input); !valid) { // worker에서 불러도 된다
//       return valid;                                 // A1이 CorruptPackage 등으로 바꾼다
//   }
//
//   ConstructionContext transaction(registry, access); // Game 스레드(한 Pump 안)
//   auto package = transaction.Create<Package>(PackageType);
//   if (!package) {
//       return Result<void>::Failure(package.Error());
//   }
//
//   if (auto created = stage.CreateObjects(transaction, package.Value()); !created) {
//       return created;
//   }
//
//   // import 해석: 생존 패키지는 registry.FindObject, 같은 트랜잭션의 SCC 형제는
//   // transaction.FindObject로 객체 경로를 handle로 바꾼다
//   if (auto applied = stage.ApplyFields(transaction, importHandles); !applied) {
//       return applied;
//   }
//
//   if (auto checked = stage.CheckRequired(transaction); !checked) {
//       return checked;
//   }
//
//   // prepare: 대표 에셋의 AssetId(Transient) 등을 Commit 전에 쓴다
//   const ObjectHandle roots[] = {package.Value()};
//   RootRef<Object> held[1];
//   return transaction.Commit(roots, held);
class ObjectGraphStage {
public:
    // Registry 정책을 사본으로 두고 options를 보관한다(baseOuter·prepare·prepareUser는 쓰지
    // 않는다). 실패를 돌려줄 수 없으므로 registry와 types가 같은 등록 저장소가 아니면
    // WrongContext를, 그 밖에 소유 스레드·접근 문맥이 아니면 그 접근
    // 오류(WrongThread·WrongContext)를 기억해 이후 모든 단계가 먼저 돌려준다.
    ObjectGraphStage(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                     const ObjectAccessContext& access,
                     ObjectGraphLoadOptions options = {}) noexcept;
    ~ObjectGraphStage();
    ObjectGraphStage(const ObjectGraphStage&) = delete;
    ObjectGraphStage& operator=(const ObjectGraphStage&) = delete;

    // 1단계: envelope·노드 표·Record·값·참조를 검증하고 이후 단계의 작업량을 미리 청구한다(2단계는
    // 예산으로 실패하지 않는다). 생성자를 실행하지 않고 ObjectRegistry에 닿지 않는다(어느 스레드나
    // — 클래스 주석의 스레드 계약). 노드마다 노드 표의 정체성 규칙(Registry 정책 사본으로
    // detail::CheckNewObjectIdentity — Outer 요약은 앞 노드들로 계산하고 outer 0이면 hasBase ?
    // input.base : Outer 없음, 어느 쪽이든 앞서 나온 같은 Outer의 형제(대표 자식·이름 있는 Inner)를
    // 더한다)과 그래프 안 형제 유일성을 생성 전에 판정한다.
    //
    // 외부 참조 값(0x80000000 | i)은 1 ≤ i ≤ externalCount여야 하고, 위치마다 (import i, 기대 형식)
    // 쌍을 기록해(서로 다른 쌍마다 하나) ApplyFields가 실제 대상으로 판정하며, import마다 쓰임
    // (ExternalUses)을 모은다.
    //
    // 순서와 오류: 호출 검사(생성자가 기억한 오류 → 진행 중 Busy → 순서 InvalidArgument — 상태
    // 불변) → 옵션(hook 없는 ReportAndDefault·LoadNewerSkippingUnknown, maxObjects·
    // maxExternalReferences 2^31 − 1 초과 InvalidArgument) → envelope(magic InvalidArgument,
    // formatVersion이 2가 아니면 Unsupported, 정의되지 않은 flags·NodeTable 없는 BaseOuter·
    // nodeCount 0·길이 부족 InvalidArgument, nodeCount·externalCount 상한 CapacityExceeded,
    // NodeTable 노드 타입 0·남는 byte InvalidArgument) → 노드 출처(NodeTable과 input.nodes가 둘 다
    // 있거나 둘 다 없음, NodeTable의 BaseOuter 비트와 hasBase 불일치, input.nodes 수가 nodeCount와
    // 다름·타입 0, importTypes 수가 externalCount와 다름·타입 0, hasBase인데 base.pathBytes가
    // MaxObjectPathBytes 초과 — InvalidArgument) → 노드 k마다(타입 미등록 NotFound → Abstract·생성
    // 함수 없음 Unsupported → 배치 Unsupported → outer가 자기 번호 이상 InvalidArgument → 정체성
    // 규칙(WorldOnly Unsupported, 이름 형식·패키지 규칙·패키지 이름·빈 이름·대표 자식
    // InvalidArgument, 객체 경로 4 KiB 초과 CapacityExceeded) → 형제 중복 InvalidArgument(파일
    // 손상) → Record(schema가 노드 타입과 다름 InvalidArgument, 버전 Unsupported, 마이그레이션,
    // 모르는 필드 정책, Transient 존재 Unsupported, excludedFlags 존재 InvalidArgument, 값 — 참조
    // 값의 범위: nodeCount 초과·외부 번호 0·externalCount 초과 InvalidArgument)) → 참조(importTypes
    // 미등록 NotFound·클래스 아님 TypeMismatch → token마다 노드 대상의 기대 형식 TypeMismatch·외부
    // 참조의 선언 타입과 기대 타입이 양립하지 않음 TypeMismatch, 기대 타입 미등록 NotFound) →
    // 예산(CapacityExceeded). 할당 실패는 OutOfMemory다.
    Result<void> Validate(const ObjectGraphInput& input) noexcept;

    // Validate가 모은 외부 참조 i의 쓰임이다(i − 1 위치, 크기 = externalCount — 저장의
    // GraphExternal과 같은 집계 규칙이라 A1이 Dependencies와 대조한다). 1단계가 해석한 파일 값
    // 기준이다: LoadNewerSkippingUnknown으로 건너뛴 필드·멤버의 외부 참조는 세지 않고,
    // 마이그레이션이 바꾼 참조는 결과 값으로 센다(그래서 새 버전 파일은 저장 때의 집계보다 작을 수
    // 있다 — A1이 Dependencies와 대조할 때 유의한다). Validate가 성공하기 전에는 비어 있고, 성공한
    // 뒤에는 이후 단계의 실패와 무관하게 Stage가 살아 있는 동안 유효하다.
    Span<const GraphExternalUse> ExternalUses() const noexcept;

    // 2a: 노드 객체를 local ID 순서로 transaction에 만든다(노드 표의 Outer·이름 — outer 0은
    // baseOuter, hasBase가 아니면 Outer 없음). 만들기 전에 Registry 상태를 확인하므로 이후 Create가
    // 실패할 수 있는 이유는 할당·슬롯 상한·종료·재진입뿐이다. baseOuter는 생존 객체이거나 이
    // 트랜잭션의 객체(예: 같은 트랜잭션에서 방금 만든 Package)다.
    //
    // 순서와 오류: 호출 검사(상태 불변 — 생성자가 기억한 오류 → 진행 중 Busy → Validate 뒤가
    // 아니면 InvalidArgument → 접근 WrongThread·WrongContext → transaction이 이 Registry·접근
    // 문맥의 것이 아니면 WrongContext → 트랜잭션 상태 규칙(ConstructionContext::CheckOpen —
    // 끝났으면 InvalidArgument, 고정된 오류면 그 오류 — 이름 점유 확인보다 먼저)) → 사전
    // 확인(실패하면 실패 상태, 트랜잭션은 고정하지 않는다): hasBase와 baseOuter 유무 불일치
    // InvalidArgument → (hasBase) transaction.DescribeOuter(baseOuter)의 오류(StaleHandle 등)와 그
    // isOutermost·outermostIsPackage·pathBytes가 input.base와 다름 InvalidArgument(handle·Inner
    // 유무는 비교하지 않는다) → 기준 Outer 노드마다 실제 요약으로 정체성 규칙 재판정(기준 Outer에
    // 이름 있는 Inner가 있는데 대표 노드, 대표 자식이 있는데 이름 있는 노드 — InvalidArgument) →
    // 기준 Outer 노드 이름의 점유(대표 노드는 빈 비교 키 — 기준 Outer에 이미 대표 자식이 있으면
    // AlreadyExists), hasBase가 아니면 outer 0 이름 있는 노드의 최상위 이름 공간 점유
    // (AlreadyExists) → 생성(Create의 오류 — 할당 OutOfMemory·슬롯 CapacityExceeded·ShuttingDown·
    // Busy, 트랜잭션은 그 오류로 고정된다). 사전 확인 실패는 아무것도 만들지 않는다. 실패하면
    // Nodes()는 비어 있다.
    Result<void> CreateObjects(ConstructionContext& transaction,
                               ObjectHandle baseOuter = {}) noexcept;

    // 2a 뒤의 노드 handle이다(local ID − 1 위치). 공개 전 객체라 Commit 전에는 그 트랜잭션의 Get·
    // GetForWrite·참조 대상·Commit roots로만 쓴다. CreateObjects가 성공하기 전에는 비어 있고,
    // Stage가 살아 있는 동안 유효하다.
    Span<const ObjectHandle> Nodes() const noexcept {
        return nodes_.View();
    }

    // node 위치(local ID − 1)의 Record 저장 버전이다(A1 Summary 대조). Validate가 성공한 뒤에만
    // 되고(그 전이나 범위 밖은 InvalidArgument) 이후 단계의 실패와 무관하다.
    Result<uint32> StoredVersion(usize node) const noexcept;

    // 2b: 1단계가 보관한 필드를 staged 객체에 쓴다(노드 참조는 2a의 handle로, 외부 참조 i는
    // imports[i − 1]로 바꾼다). imports는 생존 객체 또는 이 트랜잭션의 객체(같은 트랜잭션에 올린
    // SCC 형제 — transaction.FindObject로 찾는다)이고 배열은 호출 동안 유효해야 한다. 쓰기 전에
    // imports를 모두 확인하므로 확인 실패는 아무 필드도 쓰지 않는다. 작업량은 1단계가 청구했다.
    //
    // 순서와 오류: 호출 검사(상태 불변 — 생성자가 기억한 오류 → 진행 중 Busy → CreateObjects 뒤가
    // 아니면 InvalidArgument → 접근 WrongThread·WrongContext → transaction의 Registry·접근 문맥
    // WrongContext → CreateObjects와 다른 트랜잭션이거나 끝난 트랜잭션 InvalidArgument → 다른
    // 단계의 실패로 고정된 트랜잭션은 그 오류) → imports 확인(imports 수가 importTypes 수와 다름
    // InvalidArgument → import마다 생존·이 트랜잭션 객체가 아님 StaleHandle(종료·재진입은
    // ShuttingDown·Busy) → 실제 타입이 선언 타입의 native 파생이 아님 InvalidArgument(호출자 계약)
    // → 기록한 (import, 기대 형식) 쌍마다 실제 대상이 기대를 만족하지 않음 TypeMismatch, 확인 표
    // 할당 OutOfMemory) → 쓰기(staged 쓰기·codec의 오류 — 할당 OutOfMemory 등). imports 확인과
    // 쓰기의 실패는 객체를 만든 뒤의 실패라 Stage를 실패 상태로 두고 트랜잭션을 그 오류로
    // 고정한다(imports 수 불일치도 — 인자의 내용은 호출 검사가 아니다).
    Result<void> ApplyFields(ConstructionContext& transaction,
                             Span<const ObjectHandle> imports = {}) noexcept;

    // 2c: Required 실효 값 진단이다(4절 — 적용을 마친 staged 객체의 실효 값이 null인 Required
    // 위치마다 options.hooks->Report(RequiredMissing)가 소유 스레드에서 한 번 온다. hooks가 없으면
    // 아무것도 하지 않는다). 작업량·크기 한도를 걸지 않는다. 순서와 오류: 호출 검사(ApplyFields와
    // 같다 — ApplyFields 뒤가 아니면 InvalidArgument) → 검사(hook 실패, 할당, readStaged 없는 수동
    // Required 필드 Unsupported, Required 위치 자체의 저장 불가 값 — 예: 생성자가 둔 Required
    // Soft의 규칙 위반, MaxArrayElements를 넘는 Required 배열 — 트랜잭션을 그 오류로 고정한다).
    // 검사 동안 transaction에 Required 검사 중 표시가 선다(그 트랜잭션의 쓰기·GetForWrite·Commit
    // Busy).
    Result<void> CheckRequired(ConstructionContext& transaction) noexcept;

private:
    friend struct graph_detail::StageAccess;

    enum class Phase : uint8 {
        Constructed,
        Validated,
        Created,
        Applied,
        Checked,
        // 호출 검사를 통과한 단계가 일하는 중이다(같은 Stage의 다른 호출은 Busy).
        Running,
        Failed
    };

    // 모든 단계의 첫 호출 검사(생성자가 기억한 오류 → 진행 중 Busy → expected가 아니면
    // InvalidArgument)다. 상태를 바꾸지 않는다.
    Result<void> CheckCall(Phase expected) const noexcept;
    // 소유 스레드 단계의 공통 판정(접근 → transaction의 Registry·접근 문맥)이다.
    Result<void> CheckOwnerStage(const ConstructionContext& transaction) const noexcept;
    // ApplyFields·CheckRequired 공통: CheckOwnerStage → CreateObjects의 트랜잭션인지 → 트랜잭션
    // 상태 규칙(고정된 오류).
    Result<void> CheckSameTransaction(const ConstructionContext& transaction) const noexcept;

    Allocator& allocator_;
    ObjectRegistry& registry_;
    TypeRegistry& types_;
    ObjectAccessContext access_;
    ObjectGraphLoadOptions options_;
    RegistryPolicy policy_;
    ErrorCode contextError_ = ErrorCode::None;
    Phase phase_ = Phase::Constructed;
    bool hasBase_ = false;
    OuterSummary base_;
    uint64 transaction_ = 0;
    UniquePtr<graph_detail::StageData> data_;
    Array<ObjectHandle> nodes_;
};

// NodeTable이 있는 graph v2를 한 트랜잭션으로 복원하고 노드 1을 root로 Commit한다(Docs/Design/
// ObjectFoundations.ko.md 5.5절). ObjectGraphStage 위의 편의 API다: Validate(기준 Outer 요약 =
// DescribeOuter(baseOuter), importTypes = imports의 실제 타입) → 도달 검사 → imports의 기록한 쌍
// 판정(실제 타입을 알므로 생성자 전) → ConstructionContext → CreateObjects → ApplyFields(imports) →
// CheckRequired → prepare → Commit(). 외부 참조는 노드가 아니므로 도달 검사가 따라가지 않는다.
// 1단계는 생성자를 실행하지 않고 노드 표의 이름·정책 규칙까지 생성 전에 판정하며 2단계 몫의
// 작업량을 미리 청구한다. 2단계 실패(할당·Registry 상태·이름 점유 AlreadyExists·Required 진단 hook
// 실패·readStaged 없는 수동 Required 필드의 Unsupported·Required 위치 자체의 저장 불가 값 — 예:
// 생성자가 둔 Required Soft의 규칙 위반, MaxArrayElements를 넘는 Required 배열·prepare 실패)는 부분
// 객체 공개 없이 역순 rollback한다(이름 점유는 생성 전에 확인하므로 생성자를 실행하지 않는다).
// 할당기 통계/slot generation이 rollback 전 값으로 돌아간다는 뜻은 아니다. 원본 bytes는 호출 동안
// 유효하고 불변이어야 하며 결과는 원본을 빌리지 않는다. registry와 types는 같은 등록 저장소여야
// 한다. 다중 root·여러 그래프·호출자 Commit은 ObjectGraphStage를 직접 쓴다.
//
// 도달 검사: 노드 1에서 영속 강한 참조, inner → outer(노드 표의 outer), 패키지 정책 Registry면
// outer → inner와 기준 Outer 아래 보존으로 닿지 않는 노드가 있으면 생성 전에 InvalidArgument다(공개
// 뒤 다음 GC가 조용히 지우지 않게). 패키지 정책 Registry에서 기준 Outer가 없으면 노드 1이 아닌
// outer 0 노드(다른 패키지 — root 없이 공개되어 Discard 없이 회수된다)도 InvalidArgument다(여러
// 패키지는 ObjectGraphStage와 Commit(roots)). 편의 저장이 만든 그래프는 언제나 통과한다.
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
// LoadNewerSkippingUnknown·maxObjects·maxExternalReferences 2^31 − 1 초과 InvalidArgument) →
// baseOuter(DescribeOuter — 접근·StaleHandle) → imports(ObjectRegistry::AcquireType — 접근,
// 생존 객체가 아님 StaleHandle(파괴 요청·cutoff 포함 — 단계형 2b와 같은 코드), 타입 표 할당
// OutOfMemory) → 소유 스레드·접근 문맥(WrongThread·
// WrongContext — Stage 생성자) → envelope(magic InvalidArgument, formatVersion이 2가 아니면
// Unsupported, 정의되지 않은 flags·NodeTable 없는 BaseOuter·nodeCount 0·길이 부족
// InvalidArgument, nodeCount·externalCount 상한 CapacityExceeded, NodeTable 노드 타입 0·남는 byte
// InvalidArgument) → 노드 출처(NodeTable 없음·BaseOuter 비트와 baseOuter 불일치·imports 수가
// externalCount와 다름 InvalidArgument) → 노드(ObjectGraphStage::Validate의 노드 순서 — 정체성
// 규칙·형제 중복·참조 값 범위 포함) → 참조(형식 TypeMismatch, 기대 타입 미등록 NotFound) →
// 예산(CapacityExceeded) → 도달 검사(패키지 정책의 다른 패키지 포함 InvalidArgument) → imports의
// 기록한 쌍 판정(기대 형식 불만족 TypeMismatch — 생성자 전) → 트랜잭션 시작 → 2a 사전 확인(기준
// Outer 요약 불일치·실제 요약 재판정 InvalidArgument, 이름 점유 AlreadyExists) → 2단계(생성·imports
// 확인·적용·Required 검사·prepare·Commit).
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
