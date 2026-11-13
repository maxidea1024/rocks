#pragma once

#include <Object/ConstructionContext.h>
#include <Serialization/Record.h>

namespace rocks {

// byte 한도는 파일/필드와 migration 결과 누계다. peak RAM은 소유 index·decoded 문자열·output 복사도
// 포함한다. work는 codec의 반복/복사 비용이며 신뢰된 ctor/accessor/migration의 임의 코드를 중단하지
// 않는다. 이 콜백은 유한 작업만 하고 registry/input/현재 wrapper를 재진입 변경하지 않아야 한다.
struct ObjectGraphLimits {
    uint32 maxObjects = 1024;
    // 원본과 migration 결과를 검증한 TLV 수의 누계다.
    uint32 maxTotalFields = 4096;
    usize maxBytes = 16 * 1024 * 1024;
    usize maxValueBytes = 4 * 1024 * 1024;
    uint64 maxWork = 64 * 1024 * 1024;
    usize maxMigrationBytes = 16 * 1024 * 1024;
};

using ObjectGraphMigration = Result<Array<uint8>> (*)(Allocator&, TypeId type, uint32 sourceVersion,
                                                      uint32 targetVersion,
                                                      Span<const uint8> sourceRecord,
                                                      void* user) noexcept;

struct ObjectGraphLoadOptions {
    ObjectGraphLimits limits;
    // false이면 현재 schema의 모든 필드가 파일에 있어야 한다. true이면 누락 필드는 native
    // 기본값이다.
    bool allowMissingFields = false;
    // version 불일치에만 한 번 호출한다. callback은 전달 allocator로 소유 결과를 반환해야 한다.
    // 반환 record도 현재 schema/version/TLV/값 폭으로 다시 검증한다. callback 자체는 신뢰 경계다.
    ObjectGraphMigration migrate = nullptr;
    void* migrationUser = nullptr;
};

// owner thread에서 단일 strong root의 전체 그래프를 저장한다. root Borrow가 모든 GC slice를 막는다.
// caller/allocator/accessor callback도 실행 동안 그래프를 변경하지 않는다. 결과 buffer만 소유한다.
// allocator는 결과 Array보다 오래 살아야 한다. metadata 순서 BFS로 local ID를 부여하여 같은 입력은
// 같은 byte 순서다. 주소/registry domain/slot/generation은 파일에 저장하지 않는다.
// completeFieldLayout=true(상속 필드 포함)와 GUID별 1:1 typed accessors가 없으면 Unsupported다.
Result<Array<uint8>> SaveObjectGraph(Allocator&, ObjectRegistry&, const ObjectAccessContext&,
                                     ObjectHandle root, ObjectGraphLimits = {}) noexcept;

// 전체 구조/값/참조/도달성을 검증한 뒤 모든 객체를 비공개 생성하여 fixup하고 한번에 Commit한다.
// 실패는 부분 객체 공개 없이 역순 rollback한다. 할당기 통계/slot generation은 rollback 전 값으로
// 돌아간다는 뜻이 아니다. 원본 bytes는 호출 동안 유효하고 불변이어야 하며 결과는 원본을 빌리지
// 않는다. registry와 types는 같은 등록 저장소여야 한다. unknown field는 skip, orphan/dangling local
// ID는 거부다. nonnull 강한 참조는 value.type 등록의 native type과 정확히 일치해야 하며 TypeId
// 별명은 허용한다. value.type=0은 기반 Object다. nonnull의 명시 대상 타입 미등록은 NotFound다.
// 소프트 참조(SoftObjectRef)는 값(에셋 ID + 객체 경로)으로 저장·복원하며 대상을 그래프에 넣거나
// 찾지 않는다(기대 타입도 검사하지 않는다). 소프트 참조의 null 규칙·4 KiB 경로 상한과 digest의
// 32 byte 길이를 어기면 저장·복원 모두 InvalidArgument다. 압축·async IO·다중 root는 이 format에서
// 지원하지 않으며 암묵적으로 ObjectHandle byte를 보존하지 않는다.
Result<RootRef<Object>> LoadObjectGraph(Allocator&, ObjectRegistry&, TypeRegistry&,
                                        const ObjectAccessContext&, Span<const uint8>,
                                        ObjectGraphLoadOptions = {}) noexcept;

} // namespace rocks
