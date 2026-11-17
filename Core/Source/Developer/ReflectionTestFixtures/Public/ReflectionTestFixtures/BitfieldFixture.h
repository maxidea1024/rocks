#pragma once

#include <Core/Containers/Array.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9m bitfield bool fixture다(Docs/Design/ReflectionExtensions.ko.md 14절).
// BitfieldFlags는 부호 없는 정수 4종(uint8·16·32·64)의 폭 1 PROPERTY bitfield와 같은 바이트를
// 나누는 NATIVE bitfield(spare)·일반 멤버(plain)를 담는다(이웃 비트 보존 시험). BitfieldHolder는
// 클래스 bitfield 필드(ReadOnly 포함)·private bitfield·struct 멤버·Array<BitfieldFlags> 원소 자리를
// 담고, BitfieldChild는 base의 bitfield를 상속한다(BitfieldGraphTests·TextBitfieldTests).
ROCKS_STRUCT()
struct BitfieldFlags {
    ROCKS_PROPERTY() uint8 first : 1 = 0;
    ROCKS_NATIVE() uint8 spare : 6 = 0;
    ROCKS_PROPERTY() uint8 second : 1 = 0;
    ROCKS_PROPERTY() uint16 wide : 1 = 0;
    ROCKS_PROPERTY() uint32 wider : 1 = 0;
    ROCKS_PROPERTY() uint64 widest : 1 = 0;
    ROCKS_PROPERTY() uint8 plain = 0;
};

ROCKS_CLASS()
class BitfieldHolder : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit BitfieldHolder(Allocator& allocator) noexcept : list(allocator) {
    }

    bool Hidden() const noexcept {
        return hidden != 0;
    }

    void SetHidden(bool value) noexcept {
        hidden = value ? 1 : 0;
    }

    ROCKS_PROPERTY() uint8 on : 1 = 0;
    ROCKS_NATIVE() uint8 cache : 6 = 0;
    ROCKS_PROPERTY() uint8 off : 1 = 0;
    ROCKS_PROPERTY() uint32 big : 1 = 0;
    ROCKS_PROPERTY(ReadOnly) uint8 locked : 1 = 0;
    ROCKS_PROPERTY() BitfieldFlags flags;
    ROCKS_PROPERTY() Array<BitfieldFlags> list;

private:
    ROCKS_PROPERTY() uint8 hidden : 1 = 0;
};

ROCKS_CLASS()
struct BitfieldChild final : BitfieldHolder {
    explicit BitfieldChild(Allocator& allocator) noexcept : BitfieldHolder(allocator) {
    }

    ROCKS_PROPERTY() uint8 extra : 1 = 0;
};

} // namespace rocks::reflection_fixture
