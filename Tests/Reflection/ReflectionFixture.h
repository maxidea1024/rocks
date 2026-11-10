#pragma once

#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>

namespace rocks::reflection_fixture {

struct ROCKS_REFLECT_TYPE("ad100000000000000000000000000001", 1) Base : Object {
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000011") Guid key {};
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000012") int32 count = 0;
};

struct ROCKS_REFLECT_TYPE("ad100000000000000000000000000002", 3) Node final : Base {
    explicit Node(Allocator& allocator) noexcept : label(allocator) {
    }

    // logical destroy 이후 필드를 다시 역참조하지 않고 destructor에서 commit 결과를 관찰한다.
    static inline bool destroyedWithOldLabel = false;
    static inline bool destroyedWithNewLabel = false;

    ~Node() noexcept {
        destroyedWithOldLabel = label.View() == StringView("old");
        destroyedWithNewLabel = label.View() == StringView("new");
    }

    ROCKS_REFLECT_FIELD("ad100000000000000000000000000021") String label;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000022") ObjectRef<Node> next;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000023") bool enabled = false;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000024") uint8 small = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000025") uint16 medium = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000026") uint32 flags = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000027") uint64 wide = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000028") int64 signedWide = 0;
    ROCKS_REFLECT_FIELD("ad100000000000000000000000000029") float weight = 0;
    ROCKS_REFLECT_FIELD("ad10000000000000000000000000002a") double precision = 0;
};

// CLASS/PROPERTY fixture는 private 상속 필드와 저장 정책을 실제 generated 접근기로 검증한다.
class ROCKS_CLASS("ad100000000000000000000000000003", 1, Abstract) AbstractBase : public Object {
    ROCKS_GENERATED_BODY()

public:
    static inline uint32 constructions = 0;

    AbstractBase() noexcept {
        ++constructions;
    }

    virtual ~AbstractBase() noexcept = default;
    virtual int32 ClassValue() const noexcept = 0;

private:
    ROCKS_PROPERTY("ad100000000000000000000000000031", ReadOnly, Category = "Stats",
                   DisplayName = "체력")
    int32 baseValue = 17;
};

class ROCKS_CLASS("ad100000000000000000000000000004", 1) PropertyNode final : public AbstractBase {
    ROCKS_GENERATED_BODY()

public:
    explicit PropertyNode(Allocator& allocator) noexcept : label(allocator) {
    }

    int32 ClassValue() const noexcept override {
        return 21;
    }

private:
    ROCKS_PROPERTY("ad100000000000000000000000000041", Transient)
    ObjectRef<PropertyNode> transientChild;
    ROCKS_PROPERTY("ad100000000000000000000000000042", ReadOnly, Category = "Text",
                   DisplayName = "표시 이름")
    String label;
    ROCKS_PROPERTY("ad100000000000000000000000000043", Transient)
    int32 runtimeCache = 73;
};

// C++ concrete 타입도 선언 정책 Abstract이면 생성할 수 없다.
struct ROCKS_CLASS("ad100000000000000000000000000005", 1, Abstract) AbstractByPolicy : Object {
    static inline uint32 constructions = 0;

    AbstractByPolicy() noexcept {
        ++constructions;
    }
};

} // namespace rocks::reflection_fixture
