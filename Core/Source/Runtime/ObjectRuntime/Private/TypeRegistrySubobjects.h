#pragma once

#include <Object/RuntimeTypeOps.h>

// 클래스 등록의 선언 subobject 표 검사·블록 배치·복사다(Docs/Design/Prefab 10.2절 — 정의는
// TypeRegistrySubobjects.cpp).
namespace rocks::detail {

// 레코드 블록 안 subobject 배열의 배치다. ValidateSubobjectList가 수를 세고, Place가 주어진 위치
// 뒤에 두 배열(SubobjectSchema·RuntimeSubobjectOps)의 위치를 정해 끝 위치를 돌려준다.
struct SubobjectBlockLayout {
    usize count = 0;
    usize schemaOffset = 0;
    usize opsOffset = 0;

    usize Place(usize offset) noexcept;
};

// 선언 subobject 표(펼친 표)와 소유 구조 클래스 지정자를 검사한다. 표가 MaxClassSubobjects를
// 넘으면 CapacityExceeded, 항목 ID가 겹치면 AlreadyExists, 그 밖의 위반(지정자 필드가 없거나 종류가
// 다름, 소유 필드가 ReadOnly ObjectRef가 아님, 부착 부모 index가 앞쪽이 아님, Root 둘, 연산 표
// 불일치)은 InvalidArgument다.
Result<SubobjectBlockLayout> ValidateSubobjectList(const TypeSchema& schema,
                                                   const RuntimeTypeOps& ops) noexcept;

// derived의 subobject 표가 base 표를 앞부분으로 그대로 싣고, base가 정한 소유 구조 클래스 지정자를
// 같은 값으로 이어받는지다(파생은 base 지정을 바꿀 수 없다).
bool InheritsSubobjects(const TypeSchema& base, const TypeSchema& derived) noexcept;

// 검사를 마친 subobject 표·연산을 block(layout으로 배치한 레코드 블록)에 복사하고 복사본의 span을
// subobjects·subobjectOps에 둔다.
void CopySubobjects(const SubobjectBlockLayout& layout, uint8* block, const TypeSchema& schema,
                    const RuntimeTypeOps& ops, Span<const SubobjectSchema>& subobjects,
                    Span<const RuntimeSubobjectOps>& subobjectOps) noexcept;

} // namespace rocks::detail
