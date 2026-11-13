#pragma once

#include <Object/ConstructionContext.h>
#include <Object/FieldCodec.h>
#include <Serialization/Record.h>

namespace rocks {

// byte 한도는 파일/필드와 migration 결과 누계다. peak RAM은 소유 index·필드 색인·참조 token·output
// 복사도 포함한다. work는 codec의 반복/복사 비용이며 신뢰된 ctor/accessor/migration/hook의 임의
// 코드를 중단하지 않는다. 이 콜백들은 유한 작업만 하고 registry/input/현재 wrapper를 재진입
// 변경하지 않아야 한다.
struct ObjectGraphLimits {
    uint32 maxObjects = 1024;
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
    usize maxBytes = 16 * 1024 * 1024;
    // 필드 값 하나의 encoded byte 한도다(배열 필드 payload 전체 포함 — 넘으면 CapacityExceeded).
    usize maxValueBytes = 4 * 1024 * 1024;
    uint64 maxWork = 64 * 1024 * 1024;
    usize maxMigrationBytes = 16 * 1024 * 1024;
};

using ObjectGraphMigration = Result<Array<uint8>> (*)(Allocator&, TypeId type, uint32 sourceVersion,
                                                      uint32 targetVersion,
                                                      Span<const uint8> sourceRecord,
                                                      void* user) noexcept;

// 저장 옵션이다. policy.rejectMissingRequired면 Required 참조 null을 RequiredMissing 진단 뒤
// InvalidArgument로 거부하고, policy.excludedFlags의 필드는 저장하지 않는다. hooks는 진단·소프트
// 참조 수집을 받는다(없으면 버린다).
struct ObjectGraphSaveOptions {
    ObjectGraphLimits limits;
    FieldEncodePolicy policy;
    FieldCodecHooks* hooks = nullptr;
};

struct ObjectGraphLoadOptions {
    ObjectGraphLimits limits;
    // 미선언 enum·높은 버전·모르는 필드·누락 필드의 처리다(기본은 모두 거부, 모르는 필드는 진단 후
    // 건너뛰기). ReportAndDefault·LoadNewerSkippingUnknown은 hooks가 있어야 한다(없으면
    // InvalidArgument — 진단 없이 값을 바꾸지 않는다).
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
    // 진단·소프트 참조 수집을 받는다(1단계에서 한 번). 실패하면 복원이 실패한다.
    FieldCodecHooks* hooks = nullptr;
};

// owner thread에서 단일 strong root의 전체 그래프를 저장한다. root Borrow가 모든 GC slice를 막는다.
// caller/allocator/accessor/hook callback도 실행 동안 그래프를 변경하지 않는다. 결과 buffer만
// 소유한다. allocator는 결과 Array보다 오래 살아야 한다. metadata 순서 BFS로 local ID를 부여하여
// 같은 입력은 같은 byte 순서다. 주소/registry domain/slot/generation은 파일에 저장하지 않는다.
// 필드마다 등록된 codec(RuntimeFieldOps::encode)이 값을 쓴다. completeFieldLayout=true(상속 필드
// 포함)와 GUID별 1:1 codec이 없으면 Unsupported다. C++에서 직접 넣은 잘못된 값(미선언 enum, 소프트
// 참조 규칙 위반)은 InvalidArgument다. 배열 필드의 원소 수가 필드 maxElements를 넘으면 그 배열
// 필드 경로의 ArrayTooLong 진단(value = 원소 수)을 hooks에 보고한 뒤 CapacityExceeded다. encoded
// payload가 maxValueBytes를 넘거나 원소 누계가 maxTotalElements를 넘어도 CapacityExceeded다.
Result<Array<uint8>> SaveObjectGraph(Allocator&, ObjectRegistry&, const ObjectAccessContext&,
                                     ObjectHandle root, ObjectGraphSaveOptions = {}) noexcept;

// 두 단계로 복원한다(Docs/Design/ReflectionFoundations.ko.md 6.2·6.4절). 1단계는 전체 구조/값/참조/
// 도달성을 검증하고(생성자를 실행하지 않는다) 2단계 몫의 작업량을 미리 청구하며, 2단계는 모든
// 객체를 비공개 생성해 값을 쓰고 한번에 Commit한다. 2단계는 입력 내용이나 예산 때문에 실패하지
// 않으며 실패(할당·Registry 상태)는 부분 객체 공개 없이 역순 rollback한다. 할당기 통계/slot
// generation이 rollback 전 값으로 돌아간다는 뜻은 아니다. 원본 bytes는 호출 동안 유효하고
// 불변이어야 하며 결과는 원본을 빌리지 않는다. registry와 types는 같은 등록 저장소여야 한다. 모르는
// 필드는 policy.unknownField, orphan/dangling local ID는 거부다. nonnull 강한 참조의 대상은
// value.type 등록 타입과 생성 thunk의 정적 대상 타입의 native 타입이거나 그 파생이어야 하며 TypeId
// 별명은 허용한다. value.type=0은 기반 Object다. nonnull의 명시 대상 타입 미등록은 NotFound다.
// 소프트 참조는 값(에셋 ID + 객체 경로)으로 복원하며 대상을 그래프에 넣거나 찾지 않는다. Required
// 필드의 null·누락은 거부하지 않고 RequiredMissing을 보고한다(Required 배열은 null 원소마다 [f[i]],
// 누락 배열은 진단 없이 생성자 값). 배열 필드는 필드별 maxElements를 걸지 않고 배열마다
// MaxArrayElements와 그래프 한도(maxValueBytes·maxTotalElements·maxTotalFields·work)로만
// 막는다(상한을 낮춰도 기존 데이터가 열린다). 압축·async IO·다중 root는 이 format에서 지원하지
// 않으며 암묵적으로 ObjectHandle byte를 보존하지 않는다.
Result<RootRef<Object>> LoadObjectGraph(Allocator&, ObjectRegistry&, TypeRegistry&,
                                        const ObjectAccessContext&, Span<const uint8>,
                                        ObjectGraphLoadOptions = {}) noexcept;

} // namespace rocks
