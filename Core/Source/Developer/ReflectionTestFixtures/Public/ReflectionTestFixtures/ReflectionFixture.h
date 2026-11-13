#pragma once

#include <Core/Encoding/Sha256.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

ROCKS_CLASS()
struct Base : Object {
    ROCKS_PROPERTY() Guid key {};
    ROCKS_PROPERTY() int32 count = 0;
};

ROCKS_CLASS()
struct Node final : Base {
    explicit Node(Allocator& allocator) noexcept : label(allocator) {
    }

    // logical destroy 이후 필드를 다시 역참조하지 않고 destructor에서 commit 결과를 관찰한다.
    static inline bool destroyedWithOldLabel = false;
    static inline bool destroyedWithNewLabel = false;

    ~Node() noexcept {
        destroyedWithOldLabel = label.View() == StringView("old");
        destroyedWithNewLabel = label.View() == StringView("new");
    }

    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() ObjectRef<Node> next;
    ROCKS_PROPERTY() bool enabled = false;
    ROCKS_PROPERTY() uint8 small = 0;
    ROCKS_PROPERTY() uint16 medium = 0;
    ROCKS_PROPERTY() uint32 flags = 0;
    ROCKS_PROPERTY() uint64 wide = 0;
    ROCKS_PROPERTY() int64 signedWide = 0;
    ROCKS_PROPERTY() float weight = 0;
    ROCKS_PROPERTY() double precision = 0;
};

// CLASS/PROPERTY fixture는 private 상속 필드와 저장 정책을 실제 generated 접근기로 검증한다.
ROCKS_CLASS()
class AbstractBase : public Object {
    ROCKS_GENERATED_BODY()

public:
    static inline uint32 constructions = 0;

    AbstractBase() noexcept {
        ++constructions;
    }

    virtual ~AbstractBase() noexcept = default;
    virtual int32 ClassValue() const noexcept = 0;

private:
    ROCKS_PROPERTY(ReadOnly, Category = "Stats", DisplayName = "체력")
    int32 baseValue = 17;
};

ROCKS_CLASS()
class PropertyNode final : public AbstractBase {
    ROCKS_GENERATED_BODY()

public:
    explicit PropertyNode(Allocator& allocator) noexcept : label(allocator) {
    }

    int32 ClassValue() const noexcept override {
        return 21;
    }

private:
    ROCKS_PROPERTY(Transient)
    ObjectRef<PropertyNode> transientChild;
    ROCKS_PROPERTY(ReadOnly, Category = "Text", DisplayName = "표시 이름")
    String label;
    // UTF-8·인용부호·역슬래시·개행·NUL 뒤 숫자를 등록 metadata에서 바이트 단위로 검증한다.
    ROCKS_PROPERTY(Transient, DisplayName = "한글\"\\\n\u00007")
    int32 runtimeCache = 73;
};

// C++ concrete 타입도 선언 정책 Abstract이면 생성할 수 없다.
ROCKS_CLASS(Abstract)
struct AbstractByPolicy : Object {
    static inline uint32 constructions = 0;

    AbstractByPolicy() noexcept {
        ++constructions;
    }
};

// 자동 ID 선언도 같은 전치 규칙과 private 필드 접근을 사용한다.
ROCKS_CLASS()
class AliasPrivate final : public Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 value = 41;

public:
    int32 Read() const noexcept {
        return value;
    }
};

// B2 지정자: 스키마 버전, WorldOnly(파생에 상속), EditorOnly·Required·도구 이름, 비영속 NATIVE
// 멤버.
ROCKS_CLASS(Version = 2, WorldOnly)
struct WorldFixture : Object {
    ROCKS_PROPERTY(EditorOnly, Name = "Label") int32 note = 0;
    ROCKS_PROPERTY(Required) ObjectRef<Node> target;
    ROCKS_NATIVE() int32 runtimeCache = 0;
};

ROCKS_CLASS()
struct WorldChild final : WorldFixture {
    ROCKS_PROPERTY() int32 depth = 0;
};

// B3a 값 종류: 에셋 ID, SHA-256 digest, 소프트 객체 참조(기대 타입 있음·없음).
ROCKS_CLASS()
struct AssetFields final : Object {
    explicit AssetFields(Allocator& allocator) noexcept : icon(allocator), anyAsset(allocator) {
    }

    // 시험이 파괴 시점의 icon 값을 확인한다(쓰기 도중 논리 파괴되어도 값이 바뀌지 않았는지).
    static inline AssetId destroyedIcon{};
    static inline char destroyedIconPath[32] = {};
    static inline usize destroyedIconPathBytes = 0;

    ~AssetFields() noexcept {
        destroyedIcon = icon.asset;
        const usize bytes = icon.objectPath.ByteCount();
        destroyedIconPathBytes =
            bytes < sizeof(destroyedIconPath) ? bytes : sizeof(destroyedIconPath);
        for (usize index = 0; index < destroyedIconPathBytes; ++index) {
            destroyedIconPath[index] = icon.objectPath.Data()[index];
        }
    }

    ROCKS_PROPERTY() AssetId source;
    ROCKS_PROPERTY() Sha256Digest hash;
    ROCKS_PROPERTY(Required) SoftObjectRef<Node> icon;
    ROCKS_PROPERTY() SoftObjectRef<> anyAsset;
};

// B3b 리플렉션 enum: 도구 접두·숫자로 시작하는 도구 이름·표시 이름·숨김 열거자(uint8), 음수·암묵
// 값(int32), 비트 플래그(uint16).
ROCKS_ENUM(ToolPrefix = "TEXTUREGROUP_")
enum class TextureGroup : uint8 {
    World = 0,
    EightBitData ROCKS_ENUMERATOR(Name = "8BitData", DisplayName = "8비트 데이터") = 7,
    Reserved13 ROCKS_ENUMERATOR(Hidden) = 13,
};

ROCKS_ENUM()
enum class Bias : int32 {
    Low = -2,
    Zero = 0,
    High,
};

ROCKS_ENUM(Flags)
enum class Channels : uint16 {
    None = 0,
    Red = 1,
    Green = 2,
    Blue = 4,
    Alpha = 8,
};

ROCKS_CLASS()
struct EnumFields final : Object {
    ROCKS_PROPERTY() TextureGroup group = TextureGroup::World;
    ROCKS_PROPERTY() Bias bias = Bias::Zero;
    ROCKS_PROPERTY() Channels channels = Channels::None;
};

} // namespace rocks::reflection_fixture
