#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Optional.h>
#include <Object/SoftObjectRef.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefMap.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>
#include <ReflectionTestFixtures/ValidateFixture.h>

// 실행 중 필드 변경 통지 시험 대상이다(Docs/Design/ChangeNotification.ko.md CN1 — CN-D1·CN-D2):
// Notify 필드(값·문자열·참조)와 통지하지 않는 필드, Notify 계산 값 함수, 그 클래스를 물려받고
// Notify 필드·함수를 더하는 파생이다. 통지 번호 배치(필드 → 기반부터의 함수)와 구독·통지·정리를
// 본다(FieldNotificationTests). NotifyHolder는 쓰기 경로의 통지(CN1b — CN-D10) 대상이다: 강한 참조·
// 참조 배열·참조 struct 배열·참조 Map·참조 struct·값 배열·실수 Notify 필드와 통지하지 않는 참조
// 배열(FieldNotificationWriteTests·TextAssignNotifyTests).
namespace rocks::reflection_fixture {

ROCKS_CLASS()
class NotifyModel : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyModel(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY(Notify) int32 health = 0;
    ROCKS_PROPERTY(Notify) String label;
    ROCKS_PROPERTY() int32 plain = 0;
    ROCKS_PROPERTY(Notify) WeakObjectRef<NotifyModel> partner;

    // 체력의 두 배다(계산 값 — health가 바뀌면 코드가 함께 알린다).
    ROCKS_FUNCTION(Pure, Notify) int32 Doubled() const noexcept {
        return health * 2;
    }

    // 통지 대상이 아닌 Pure 함수다.
    ROCKS_FUNCTION(Pure) int32 Plain() const noexcept {
        return plain;
    }
};

ROCKS_CLASS()
class NotifyModelChild : public NotifyModel {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyModelChild(Allocator& allocator) noexcept : NotifyModel(allocator) {
    }

    ROCKS_PROPERTY(Notify) float speed = 0.0f;

    ROCKS_FUNCTION(Pure, Notify) bool Fast() const noexcept {
        return speed > 1.0f;
    }
};

// enum 키 Map Notify 필드(NotifyHolder::byKind — 생성 함수표의 키 enum 칸 뒤 storage 칸)의 키다.
ROCKS_ENUM()
enum class NotifyKind : uint8 {
    First,
    Second
};

// 인라인 참조 struct다(NotifyHolder::point — 경로 쓰기가 안의 참조를 SetReference로 쓴다).
ROCKS_STRUCT()
struct NotifyPoint {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() ObjectRef<NotifyModel> pin;
};

// 참조 struct 원소다(StructRefArray<NotifySlot>).
ROCKS_STRUCT()
struct NotifySlot {
    ROCKS_GENERATED_BODY()

    explicit NotifySlot(Allocator&) noexcept {
    }

    ROCKS_PROPERTY() ObjectRef<NotifyModel> item;
    ROCKS_PROPERTY() int32 count = 0;
};

ROCKS_CLASS()
class NotifyHolder : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyHolder(Allocator& allocator) noexcept
        : members(allocator), quiet(allocator), slots(allocator), byKey(allocator),
          byKind(allocator), numbers(allocator) {
    }

    ROCKS_PROPERTY(Notify) ObjectRef<NotifyModel> target;
    ROCKS_PROPERTY(Notify) ObjectRefArray<NotifyModel> members;
    ROCKS_PROPERTY() ObjectRefArray<NotifyModel> quiet;
    ROCKS_PROPERTY(Notify) StructRefArray<NotifySlot> slots;
    ROCKS_PROPERTY(Notify) ObjectRefMap<uint32, NotifyModel> byKey;
    ROCKS_PROPERTY(Notify) ObjectRefMap<NotifyKind, NotifyModel> byKind;
    ROCKS_PROPERTY(Notify) NotifyPoint point;
    ROCKS_PROPERTY(Notify) Array<int32> numbers;
    ROCKS_PROPERTY(Notify) float ratio = 0.0f;
};

// 상속한 Notify 컨테이너를 파생 객체에서 Registry API로 바꾼다(storage thunk가 파생 객체에서 기반
// 멤버를 잰다).
ROCKS_CLASS()
class NotifyHolderChild : public NotifyHolder {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyHolderChild(Allocator& allocator) noexcept : NotifyHolder(allocator) {
    }
};

// 생성 setter의 값 struct다(사본 뒤 교환·멤버별 같음 — CN-D13·CN-D15).
ROCKS_STRUCT()
struct NotifyStyle {
    ROCKS_GENERATED_BODY()

    explicit NotifyStyle(Allocator& allocator) noexcept : font(allocator) {
    }

    ROCKS_PROPERTY() String font;
    ROCKS_PROPERTY() float size = 0.0f;
};

// NATIVE 멤버가 있는 struct다(값 같음이 늘 다르다 — CN-D15).
ROCKS_STRUCT()
struct NotifyCached {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_NATIVE() uint32 cache = 0;
};

// 생성 setter·getter와 코드 통지(CN2a — CN-D9·CN-D13)의 대상이다: Apply가 있는 문자열 setter, 실수·
// struct·Text·Soft·Optional<String>·Map setter, 통지하지 않는 필드의 setter, EditorOnly 필드의
// setter·getter(에디터 데이터 켬·끔 두 조합에서 컴파일), setter 없는 Notify 필드(SetAndNotify),
// virtual Notify 계산 값 함수. 생성 setter와 .cpp의 진입점을 모듈 밖(시험 실행 파일)에서 부르므로
// 클래스를 export한다(Modular 빌드 — 생성 setter는 모듈 생성 소스에 정의된다). 생성자가 setter를
// 불러 Registry 없는 경로를 남긴다(NATIVE 기록).
// NotifyWidget::grid의 칸 수다(상자 인라인 저장소 64 byte보다 크다 — Object/DetachedPath.h).
inline constexpr usize GridCells = 24;

ROCKS_CLASS()
class ROCKS_REFLECTIONTESTFIXTURES_API NotifyWidget : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyWidget(Allocator& allocator) noexcept
        : label(allocator), style(allocator), values(allocator), link(allocator), scores(allocator),
          stack(allocator), tags(allocator) {
        constructorScale = SetScale(2.0f);
        auto caption = Text::Invariant(allocator, "constructed");
        constructorCaption = caption ? SetCaption(caption.Value()) : Result<void>::Success();
    }

    ROCKS_PROPERTY(Notify, Apply = "Relayout") String label;
    ROCKS_PROPERTY(Notify) float scale = 1.0f;
    ROCKS_PROPERTY(Notify) NotifyStyle style;
    ROCKS_PROPERTY(Notify) Text caption;
    ROCKS_PROPERTY() int32 quiet = 0;
    ROCKS_PROPERTY(Notify) int32 count = 0;
    ROCKS_PROPERTY(Notify) Array<int32> values;
    ROCKS_PROPERTY(Notify) NotifyCached cached;
    ROCKS_PROPERTY(Notify) SoftObjectRef<NotifyModel> link;
    ROCKS_PROPERTY(Notify) Optional<String> nickname;
    ROCKS_PROPERTY(Notify) OrderedMap<int32, int32> scores;
    // ReadOnly이지만 setter가 있어 리플렉션 쓰기가 setter로 간다(CN-D14).
    ROCKS_PROPERTY(ReadOnly, Notify) int32 locked = 0;
    // 검증 hook struct(값 트리 검증 — StructValidation), 상자 인라인 저장소보다 큰 정적 배열(heap
    // 상자), 배열·Set(원소 연산)을 setter로 쓴다(CN-D14).
    ROCKS_PROPERTY(Notify) ValidRange span;
    ROCKS_PROPERTY(Notify) int32 grid[GridCells] = {};
    ROCKS_PROPERTY(Notify) Array<int32> stack;
    ROCKS_PROPERTY(Notify) OrderedSet<int32> tags;
#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly) int32 note = 0;
    ROCKS_SETTER(SetNote, note)
    ROCKS_GETTER(GetNote, note)
#endif

    ROCKS_NATIVE() uint32 relayouts = 0;
    ROCKS_NATIVE() Result<void> constructorScale = Result<void>::Success();
    ROCKS_NATIVE() Result<void> constructorCaption = Result<void>::Success();

    ROCKS_SETTER(SetLabel, label)
    ROCKS_GETTER(GetLabel, label)
    ROCKS_SETTER(SetScale, scale)
    ROCKS_SETTER(SetStyle, style)
    ROCKS_SETTER(SetCaption, caption)
    ROCKS_SETTER(SetQuiet, quiet)
    ROCKS_GETTER(GetCount, count)
    ROCKS_SETTER(SetLink, link)
    ROCKS_SETTER(SetNickname, nickname)
    ROCKS_SETTER(SetScores, scores)
    ROCKS_SETTER(SetLocked, locked)
    ROCKS_SETTER(SetSpan, span)
    ROCKS_SETTER(SetGrid, grid)
    ROCKS_SETTER(SetStack, stack)
    ROCKS_SETTER(SetTags, tags)

    // count + quiet이다(virtual 계산 값 — NotifyChanged가 템플릿 인자 동등성으로 ID를 찾는다).
    ROCKS_FUNCTION(Pure, Notify) virtual int32 Total() const noexcept {
        return count + quiet;
    }

    // 코드 통지 진입점을 시험에 연다(protected — 정의는 생성 헤더를 include한 NotifyFixture.cpp).
    Result<bool> PutCount(int32 next) noexcept;
    Result<bool> PutValues(const Array<int32>& next) noexcept;
    Result<bool> PutCached(const NotifyCached& next) noexcept;
    Result<void> TouchTotal() noexcept;
    Result<void> TouchScale() noexcept;

private:
    void Relayout() noexcept {
        ++relayouts;
    }
};

// 기반 멤버의 코드 통지(기반의 생성 NotifyId가 답한다)와 재정의한 virtual 계산 값 함수다.
ROCKS_CLASS()
class ROCKS_REFLECTIONTESTFIXTURES_API NotifyWidgetChild : public NotifyWidget {
    ROCKS_GENERATED_BODY()

public:
    explicit NotifyWidgetChild(Allocator& allocator) noexcept : NotifyWidget(allocator) {
    }

    ROCKS_PROPERTY(Notify) int32 level = 0;

    int32 Total() const noexcept override {
        return count + quiet + level;
    }

    Result<bool> PutBaseCount(int32 next) noexcept;
    Result<bool> PutLevel(int32 next) noexcept;
    // 재정의한 함수는 선언한 기반의 이름으로 알린다.
    Result<void> TouchBaseTotal() noexcept;
};

} // namespace rocks::reflection_fixture
