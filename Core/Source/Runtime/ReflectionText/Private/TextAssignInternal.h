#pragma once

#include "TextInternal.h"

#include <Core/Containers/Array.h>
#include <Object/ObjectRefs.h>
#include <Object/PreparedReferences.h>
#include <Object/PreparedStructElements.h>
#include <ReflectionText/TextAssignments.h>

// 대입 묶음(리플렉션 B9i3c1 — ReflectionText/TextAssignments.h)의 내부 상태다.
// TextAssignments.cpp가 묶음의 수명·단위·override 경로·게시를, TextAssignApply.cpp가 대입 하나를
// 준비 단위 사본에 적용하는 일을 맡는다.
namespace rocks::reflection_text::detail {

// 값 부분 트리 준비 단위 하나다: 떼어 낸 사본(상자), 단위 경로(필드·멤버 하나 — 선택자 없음), 게시
// 자리, 대입이 실제로 사본을 바꿨는지.
struct AssignmentUnit {
    explicit AssignmentUnit(Allocator& valueAllocator, Allocator& allocator) noexcept
        : box(valueAllocator), steps(allocator) {
    }

    PropertyPath Path() const noexcept {
        return PropertyPath{steps.View(), {}};
    }

    DetachedValue box;
    Array<PathStep> steps;
    PreparedAttach prepared;
    bool dirty = false;
};

// 단일 강한 참조 위치의 간선 변경 단위다(리플렉션 B9i3c2): 위치 경로(인라인 struct 멤버·정적 배열과
// StructRefArray의 index 선택자 — key 없음), 새 대상, 게시 자리와 옛 대상(Publish가 찾는다).
struct ReferenceUnit {
    explicit ReferenceUnit(Allocator& allocator) noexcept : steps(allocator) {
    }

    PropertyPath Path() const noexcept {
        return PropertyPath{steps.View(), {}};
    }

    Array<PathStep> steps;
    ObjectHandle target{};
    // 게시한 뒤에는 게시 전 대상이다.
    ObjectHandle previous{};
    PreparedReference prepared;
};

// Registry 컨테이너(ObjectRefArray·ObjectRefMap) 단위다(리플렉션 B9i3c3): 컨테이너 필드·멤버
// 경로(선택자 없음 — 앞 단계는 인라인 struct), 준비 사본(묶을 때의 내용과 대입이 만든 새 내용),
// 대입이 사본을 바꿨는지.
struct ContainerUnit {
    explicit ContainerUnit(Allocator& allocator) noexcept : steps(allocator), stage(allocator) {
    }

    PropertyPath Path() const noexcept {
        return PropertyPath{steps.View(), {}};
    }

    Array<PathStep> steps;
    PreparedReferences stage;
    bool dirty = false;
};

// StructRefArray 원소 지우기·비우기 단위다(리플렉션 B9i3c4): 배열 필드·멤버 경로(선택자 없음 — 앞
// 단계는 인라인 struct), 준비물(지금 index → 묶을 때 index 표와 게시). 같은 묶음의 다른 단위는
// 이 배열의 원소를 묶을 때 index로 가리킨다.
struct ElementsUnit {
    explicit ElementsUnit(Allocator& allocator) noexcept : steps(allocator), stage(allocator) {
    }

    PropertyPath Path() const noexcept {
        return PropertyPath{steps.View(), {}};
    }

    Array<PathStep> steps;
    PreparedStructElements stage;
};

// 복사해 둔 경로 목록이다(단계·key 종류·key wire를 평평한 배열에 담는다 — 버퍼가 옮겨져도 key는
// offset으로 다시 만든다).
class PathCopies {
public:
    explicit PathCopies(Allocator& allocator) noexcept
        : steps_(allocator), keys_(allocator), bytes_(allocator), entries_(allocator) {
    }

    // path를 복사해 붙인다. 오류: 할당 실패(목록은 그대로).
    Result<void> Append(PropertyPath path) noexcept;

    usize Count() const noexcept {
        return entries_.Size();
    }

    // index번째 경로다. key는 keys(MaxPropertyPathSteps칸)에 다시 만들고 경로가 그것을 가리킨다 —
    // keys와 이 목록이 바뀌지 않는 동안 유효하다.
    PropertyPath View(usize index, PathKey* keys) const noexcept;

private:
    struct KeyRecord {
        ValueKind kind = ValueKind::Opaque;
        usize offset = 0;
        usize size = 0;
    };

    struct Entry {
        usize stepOffset = 0;
        usize stepCount = 0;
        usize keyOffset = 0;
        usize keyCount = 0;
    };

    Array<PathStep> steps_;
    Array<KeyRecord> keys_;
    Array<uint8> bytes_;
    Array<Entry> entries_;
};

// 묶음의 상태다(PreparedAssignments·PublishedAssignments가 소유한다). 옮길 수 없다(문맥이 options를
// 가리킨다).
struct AssignmentState {
    AssignmentState(const AssignmentTarget& target, const TextCodecOptions& options,
                    TextDiagnostics& diagnostics, Allocator& allocator) noexcept;

    AssignmentState(const AssignmentState&) = delete;
    AssignmentState& operator=(const AssignmentState&) = delete;

    // 경로 연산의 시작 위치다(대상 그 자체).
    PathTarget Root() const noexcept;

    AssignmentTarget target;
    TextCodecOptions options;
    TextCodecContext context;
    Allocator& allocator;
    TypeLease lease;
    // Live 대상의 변경 표지(Prepare 처음에 읽는다).
    uint64 stamp = 0;
    Array<UniquePtr<AssignmentUnit>> units;
    // 단일 강한 참조 위치 단위다(값이 실제로 바뀌는 것만).
    Array<UniquePtr<ReferenceUnit>> references;
    // Registry 컨테이너 단위다(리플렉션 B9i3c3).
    Array<UniquePtr<ContainerUnit>> containers;
    // StructRefArray 원소 지우기·비우기 단위다(리플렉션 B9i3c4).
    Array<UniquePtr<ElementsUnit>> elements;
    // 해석한 대입 경로(같은 경로 두 번 검사).
    PathCopies assigned;
    // override 경로(선택자·key 없음)의 단계와 범위, 그 view.
    Array<PathStep> overrideSteps;
    Array<usize> overrideEnds;
    Array<PropertyPath> overrideViews;
    // 새로 가리키는 Hard·Weak 대상의 고정이다(Publish가 끝나면 놓는다).
    Array<RootRef<Object>> pins;
    // 진단을 남긴 묶음이다(게시할 수 없다).
    bool failed = false;
    // Publish를 불렀다.
    bool consumed = false;
    // JSON import가 미리 읽은 지금 대입의 값이다(리플렉션 B9i4 — 있으면 값 텍스트 대신 쓴다.
    // ProcessResolvedAssignment 동안만 가리킨다).
    TextValue* preparsed = nullptr;
};

// 묶음 상태를 만들고 대상 타입 lease와 Live 대상의 변경 표지를
// 읽는다(PrepareAssignments·ImportJson의 공통 시작 — 리플렉션 B9i4). 오류: PrepareAssignments의
// 호출 오류(옵션·pool·대상 문맥·할당).
Result<UniquePtr<AssignmentState>> BeginAssignments(const AssignmentTarget& target,
                                                    const TextCodecOptions& options,
                                                    TextDiagnostics& diagnostics) noexcept;

// 준비를 마친 상태를 묶음으로 감싼다(PreparedAssignments의 friend).
struct AssignmentAccess {
    static PreparedAssignments Wrap(UniquePtr<AssignmentState> state) noexcept;
};

// 대입 하나를 해석하고 준비 단위에 적용한다. 진단을 남겼으면 state.failed를 세운다. 오류는 호출
// 오류(할당·입력 한도·대상 문맥·Busy 등)다.
Result<void> ProcessAssignment(AssignmentState& state, const TextAssignment& assignment) noexcept;

// 경로를 이미 해석한 대입 하나를 준비 단위에 적용한다(ProcessAssignment의 뒷부분 — JSON import가
// 쓴다, 리플렉션 B9i4). path·slot은 대상 기준 해석 결과(입력 용도 판정을 마친 것), value가 있으면
// 값 텍스트(assignment.value) 대신 그 값을 쓴다(Set만 — 옮겨 간다). assignment.path는 진단의 경로
// 텍스트다.
Result<void> ProcessResolvedAssignment(AssignmentState& state, const TextAssignment& assignment,
                                       TextSource source, PropertyPath path, const TextSlot& slot,
                                       TextValue* value) noexcept;

// 단위 경로(선택자 없음)의 준비 단위를 찾거나 대상에서 떼어 만든다. 진단을 남겼으면 nullptr이다.
Result<AssignmentUnit*> AcquireUnit(AssignmentState& state, PropertyPath unitPath,
                                    TextSource source, StringView pathText) noexcept;

// 컨테이너 경로(선택자 없음)의 Registry 컨테이너 단위를 찾거나 만들어 지금 내용으로 묶는다(리플렉션
// B9i3c3). 진단을 남겼으면 nullptr이다.
Result<ContainerUnit*> AcquireContainer(AssignmentState& state, PropertyPath unitPath,
                                        TextSource source, StringView pathText) noexcept;

// 배열 경로(선택자 없음)의 StructRefArray 원소 지우기 단위를 찾거나 만들어 배열에 묶는다(리플렉션
// B9i3c4). 진단을 남겼으면 nullptr이다.
Result<ElementsUnit*> AcquireElements(AssignmentState& state, PropertyPath unitPath,
                                      TextSource source, StringView pathText) noexcept;

// 끝 경로 path의 override 경로를 state에 넣는다(같은 경로가 있으면 무동작).
Result<void> AddOverride(AssignmentState& state, PropertyPath path) noexcept;

// 경로 연산 실패를 진단(값·경로 문제)이나 호출 오류(그 밖)로 옮긴다. 진단이면 Success다.
Result<void> ReportPathFailure(AssignmentState& state, ErrorCode code,
                               const PropertyPathFailure& failure, TextSource source,
                               StringView pathText) noexcept;

// 진단 하나를 남기고 묶음을 실패로 표시한다.
Result<void> Diagnose(AssignmentState& state, TextDiagnosticCode code, TextSource source,
                      StringView pathText, StringView message) noexcept;

} // namespace rocks::reflection_text::detail
