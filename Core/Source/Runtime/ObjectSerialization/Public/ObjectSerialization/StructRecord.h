#pragma once

#include <ObjectSerialization/ObjectGraph.h>

// 독립 struct Record다(Docs/Design/ReflectionExtensions.ko.md 26절). struct 값 하나를 객체 그래프
// 없이 Record 바이트(header schema = struct ID, version = struct 버전, 멤버 TLV)로 쓰고 읽는다.
// 바이트는 같은 값을 객체 그래프의 InstancedStruct 필드에 담아 저장했을 때의 내용 Record와 같다.
// 노드 그래프의 노드 속성·입력 기본값(Docs/Design/NodeGraph GD23)과 그
// 텍스트화(NodeGraphTools)가 쓴다.
//
// 담을 수 있는 struct만 받는다: InstancedStruct 담기 조건(detail::CheckInstancedStructContent —
// 강한 참조·Registry 컨테이너·InstancedStruct·Required·WorldOnly 소유자 요구 없음). weak 참조는
// null만 쓰고 읽는다(그래프 밖이라 대상을 쓸 곳이 없다).
//
// 사용 예:
//   auto bytes = SaveStructRecord(allocator, registry, access, lease, &value);
//   auto type = AcquireStructRecordType(registry.Types(), bytes.Value().View());
//   // type.Value()의 값 하나를 만든 뒤
//   auto loaded = LoadStructRecord(allocator, registry, access, bytes.Value().View(), type.Value(),
//                                  storage);
namespace rocks {

// 저장·읽기 옵션이다. limits는 그래프와 같은 한도 묶음이다(값 하나라 maxValueBytes가 Record 크기
// 상한이다). migrateStruct·migrationUser는 그래프 로드와 같은 struct 마이그레이션
// override(Unsupported면 타입이 등록한 마이그레이션을 쓴다)다. hooks는 진단·Soft 수집·struct
// 치환이다(없으면 진단이 필요한 정책은 그래프와 같이 InvalidArgument).
struct StructRecordOptions {
    ObjectGraphLimits limits;
    FieldEncodePolicy encodePolicy;
    FieldDecodePolicy decodePolicy;
    ObjectGraphMigration migrateStruct = nullptr;
    void* migrationUser = nullptr;
    FieldCodecHooks* hooks = nullptr;
};

// struct 값 value(type의 살아 있는 값)를 Record로 쓴다. weak 참조는 그래프 저장의 집합 밖 Reject
// 규칙과 같이 null로 쓴다(살아 있는 대상은 ExternalReference 진단 — hooks가 있을 때, 다른
// Registry의 handle은 WrongContext). 오류: type이 struct가 아니거나 담을 수 없는 struct면
// InvalidArgument, 크기· 작업량 한도는 CapacityExceeded, 그 밖은 멤버 codec의 오류다. Registry 소유
// 스레드에서 부른다(ClassRef 판정이 그 Registry를 조회한다).
//
// 사용 예:
//   auto record = SaveStructRecord(allocator, registry, registry.OwnerAccess(), lease, &point);
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<Array<uint8>>
SaveStructRecord(Allocator& allocator, ObjectRegistry& registry, const ObjectAccessContext& access,
                 const TypeLease& type, const void* value,
                 const StructRecordOptions& options = {}) noexcept;

// Record를 struct 값 value(type의 살아 있는 값)로 읽는다. 그래프 복원과 같은 두 단계다: 1단계는
// 값에 닿지 않고 버전·마이그레이션·멤버를 검증하며, 2단계는 객체를 만들지 않는 트랜잭션으로 type의
// 기본 생성 값에 적용한다. 성공하면 그 새 값을 value와 맞바꾼다(RuntimeStructOps::swap — 없으면
// Unsupported). Record에 없는 멤버는 기본 생성 값이다. 실패하면 value는 그대로다.
//
// 수명: 새 값은 construct(ObjectAllocator)로 만들고, decode가 만드는 버퍼(Text 노드·String 등)도
// Registry의 ObjectAllocator다(그래프 2단계와 같다). 그래서 value는 그 Registry보다 오래 쓰지
// 않는다. 2단계는 Registry 트랜잭션이므로 소유 스레드에서 부른다(아니면 트랜잭션의 오류 —
// WrongThread 등).
//
// 오류: 빈 입력·schema가 type이 아님·header·TLV 손상은 InvalidArgument(등 Record 오류), 담을 수
// 없는 struct·0이 아닌 weak 값은 InvalidArgument, 옛 버전인데 마이그레이션이 없으면 Unsupported,
// newer 버전은 decodePolicy를 따른다(기본 Unsupported), 진단이 필요한 읽기 정책인데 hooks가 없으면
// InvalidArgument, Name이 있는데 Registry에 NamePool이 없으면 Unsupported. 관찰(ObservedStructs)은
// 하지 않는다.
//
// 사용 예:
//   InstancedPoint point(registry.ObjectAllocator());
//   auto loaded = LoadStructRecord(allocator, registry, registry.OwnerAccess(), bytes, lease,
//   &point);
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<void>
LoadStructRecord(Allocator& allocator, ObjectRegistry& registry, const ObjectAccessContext& access,
                 Span<const uint8> record, const TypeLease& type, void* value,
                 const StructRecordOptions& options = {}) noexcept;

// Record header의 schema로 struct lease를 연다. 오류: header 오류는 그 오류, 미등록 NotFound,
// struct가 아니면 TypeMismatch다.
//
// 사용 예:
//   auto type = AcquireStructRecordType(registry.Types(), node.properties.bytes.View());
[[nodiscard]] ROCKS_OBJECTSERIALIZATION_API Result<TypeLease>
AcquireStructRecordType(TypeRegistry& types, Span<const uint8> record) noexcept;

} // namespace rocks
