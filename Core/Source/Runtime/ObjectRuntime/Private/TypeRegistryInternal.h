#pragma once

#include <Object/TypeRegistry.h>

// TypeRegistry 구현 파일(TypeRegistry*.cpp·TypeLease.cpp)이 함께 쓰는 내부 정의다. 타입
// 레코드(TypeRegistry::Record)와 등록 한 번의 색인 준비물(TypeRegistry::IndexGrowth)을 둔다.
namespace rocks {

// 클래스·enum·struct 레코드가 같은 목록·ID 공간을 쓴다. enum 레코드는 schema.id·name과
// ops.nativeType·module만 채우고 enumSchema에 열거자를 둔다. struct 레코드는 schema(id·version·
// name·fields)와 ops(size·alignment·nativeType·module)를 채우고 structOps에 멤버 함수표(스키마
// 순서)·construct·destroy·trace·migrate를 둔다. 클래스·struct 레코드는 fieldOrder에 필드 ID 순 정렬
// 색인을 두고(TypeLease·StructView::FieldIndex), 클래스 레코드는 opsIndex에 스키마 위치 →
// ops.fields 위치 색인(함수표가 없는 Opaque 필드는 NoOps)을 둔다(TypeLease::FieldOps — 클래스
// ops.fields는 입력 순서다). 클래스·struct 레코드의 fieldTypes는 schema.fields와 같은 길이이며
// 필드의 대상 형식(배열이면 원소)이 가리키는 Enum·Struct 레코드(그 밖은 nullptr)다 — 이 참조는
// lease가 아니라 dependents 계수로 표시하므로 Shutdown이 영구 Busy가 되지 않는다. fieldKeyTypes도
// 같은 길이이며 Map 필드의 키(FieldKeyType)가 Enum이면 그 enum 레코드다(리플렉션 B9f2 — 그 밖은
// nullptr, 같은 dependents 계수).
struct TypeRegistry::Record {
    static constexpr uint32 NoOps = NumericLimits<uint32>::Max();

    Record* next = nullptr;
    Record* previous = nullptr;
    TypeKind kind = TypeKind::Class;
    TypeSchema schema;
    // 저장소 시작에서 Object 기반까지의 거리다(TypeLease::ObjectOffset — ObjectRegistry가 첫 생성
    // 때 기록한다. 여러 Registry·스레드가 같은 값을 쓰므로 원자적으로 읽고 쓴다). ops.size 옆에 둬
    // 소유자 범위 검사가 줄 하나만 읽는다.
    usize objectOffset = TypeLease::NoObjectOffset;
    RuntimeTypeOps ops;
    EnumSchema enumSchema;
    RuntimeStructOps structOps;
    uint32* fieldOrder = nullptr;
    uint32* opsIndex = nullptr;
    uint32 depth = 0;
    bool containsStrongReference = false;
    bool containsRequired = false;
    bool requiresWorldOnlyOwner = false;
    bool containsReferenceArray = false;
    bool containsName = false;
    bool containsWeakReference = false;
    bool containsInstancedStruct = false;
    Record** fieldTypes = nullptr;
    Record** fieldKeyTypes = nullptr;
    uint32 generation = 0;
    uint32 leases = 0;
    uint32 dependents = 0;
    Record* base = nullptr;
};

// 등록 한 번의 색인 준비물이다(잠금 밖에서 할당한 새 색인과, 바꾼 뒤 잠금 밖에서 반환할 옛 색인).
struct TypeRegistry::IndexGrowth {
    explicit IndexGrowth(Allocator& allocator) noexcept
        : id(allocator), native(allocator), toolName(allocator), retiredId(allocator),
          retiredNative(allocator), retiredToolName(allocator) {
    }

    IdIndex id;
    NativeIndex native;
    ToolNameIndex toolName;
    IdIndex retiredId;
    NativeIndex retiredNative;
    ToolNameIndex retiredToolName;
    bool rebuildId = false;
    bool rebuildNative = false;
    bool rebuildToolName = false;
};

} // namespace rocks
