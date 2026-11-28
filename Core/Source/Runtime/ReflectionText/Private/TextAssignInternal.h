#pragma once

#include "TextInternal.h"

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Hash/TypeHash.h>
#include <Object/ObjectRefs.h>
#include <Object/PreparedReferences.h>
#include <Object/PreparedStructElements.h>
#include <ReflectionText/TextAssignments.h>

// 대입 묶음(리플렉션 B9i3c1 — ReflectionText/TextAssignments.h)의 내부 상태다.
// TextAssignments.cpp가 묶음의 수명·단위·override 경로·게시를, TextAssignApply.cpp가 대입 하나를
// 준비 단위 사본에 적용하는 일을 맡는다.
namespace rocks::reflection_text::detail {

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

    // 목록을 비운다(용량은 남긴다).
    void Clear() noexcept;

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

// 서로 다른 경로의 집합이다(같은 경로 두 번 검사 — 리플렉션 B9i 리뷰 반영). 경로를 SipHash(seed)로
// 색인해 넣기 한 번이 경로 길이에 비례하고(대입 수에 선형), 해시가 같은 경로끼리만
// SamePropertyPath로 비교한다. 해시는 SamePropertyPath의 같음과 맞춘다(Name key는 Core Name 비교
// 규칙 — 대소문자 무시·번호 접미·None 동치).
class ROCKS_REFLECTIONTEXT_API PathSet {
public:
    explicit PathSet(Allocator& allocator, HashSeed seed = ProcessHashSeed()) noexcept
        : paths_(allocator), heads_(allocator), next_(allocator), seed_(seed) {
    }

    // path가 없으면 복사해 넣고 true, 같은 경로가 이미 있으면 false다. 오류: 할당 실패(집합은
    // 그대로).
    Result<bool> Add(PropertyPath path) noexcept;

    usize Count() const noexcept {
        return paths_.Count();
    }

    // index번째(넣은 순서) 경로다(PathCopies::View와 같은 수명).
    PropertyPath View(usize index, PathKey* keys) const noexcept {
        return paths_.View(index, keys);
    }

    // 집합을 비운다(비교 수는 남는다).
    void Clear() noexcept {
        paths_.Clear();
        heads_.Clear();
        next_.Clear();
    }

    // 지금까지 한 SamePropertyPath 비교 수다(시험이 작업량이 선형인지 본다).
    uint64 Comparisons() const noexcept {
        return comparisons_;
    }

private:
    uint64 HashOf(PropertyPath path) const noexcept;

    PathCopies paths_;
    // 해시 → 그 해시의 마지막 경로 index, 경로마다 같은 해시의 앞 경로 index(없으면 IndexNone).
    HashMap<uint64, usize> heads_;
    Array<usize> next_;
    HashSeed seed_;
    uint64 comparisons_ = 0;
};

// 값 부분 트리 준비 단위 하나다: 떼어 낸 사본(상자), 단위 경로(필드·멤버 하나 — 선택자 없음), 게시
// 자리, 대입이 실제로 사본을 바꿨는지, 호출 끝에 지금 값과 대조할 ReadOnly 자리(리플렉션 B9i 리뷰
// 반영 — 대상 기준 경로와 그 대입의 진단 위치).
struct AssignmentUnit {
    explicit AssignmentUnit(Allocator& valueAllocator, Allocator& allocator) noexcept
        : box(valueAllocator), steps(allocator), readOnlyPaths(allocator),
          readOnlySources(allocator) {
    }

    PropertyPath Path() const noexcept {
        return PropertyPath{steps.View(), {}};
    }

    DetachedValue box;
    Array<PathStep> steps;
    PreparedAttach prepared;
    bool dirty = false;
    // 대조할 ReadOnly 자리(서로 겹치지 않는다 — 경로마다 처음 ReadOnly가 붙는 단계)와 그 자리에
    // 처음 닿은 대입의 진단 위치다.
    PathSet readOnlyPaths;
    Array<TextSource> readOnlySources;
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

// StructRefArray 원소 지우기·비우기·추가 단위다(리플렉션 B9i3c4·B9i5): 배열 필드·멤버 경로(선택자
// 없음 — 앞 단계는 인라인 struct), 준비물(지금 index → 묶을 때 index 표, 추가 원소 블록과 게시).
// 같은 묶음의 다른 단위는 이 배열의 원소를 묶을 때 index로 가리키고, 추가 원소 안 대입은 그 블록에
// 바로 적용한다(단위를 따로 두지 않는다 — 고정의 owner는 블록 주소다).
struct ElementsUnit {
    explicit ElementsUnit(Allocator& allocator) noexcept : steps(allocator), stage(allocator) {
    }

    PropertyPath Path() const noexcept {
        return PropertyPath{steps.View(), {}};
    }

    Array<PathStep> steps;
    PreparedStructElements stage;
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
    PathSet assigned;
    // override 경로(선택자·key 없음)의 단계와 범위, 그 view.
    Array<PathStep> overrideSteps;
    Array<usize> overrideEnds;
    Array<PropertyPath> overrideViews;
    // 새로 가리키는 Hard·Weak 대상의 고정이다(Publish가 끝나면 놓는다). pinOwners는 같은 칸의
    // 고정을 만든 단위다(AssignmentUnit·ReferenceUnit·ContainerUnit 주소 — 단위를 버리면 그 고정도
    // 놓는다).
    Array<RootRef<Object>> pins;
    Array<const void*> pinOwners;
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

// 게시된 객체 대상의 묶음 게시 구간에 넘길 새 대상 재확인 목록과 간선 변경 목록을 만든다
// (TextAssignPublishPlan.cpp — 리플렉션 B9i5). 컨테이너 단위는 옛 내용과 새 내용의 handle 다중집합
// 차이만 넣는다(그대로 남는 대상은 이미 게시된 간선이라 재확인하지 않는다). 오류: 할당 실패.
Result<void> PlanBatchPublish(AssignmentState& state, Array<BatchTargetCheck>& targets,
                              Array<BatchEdgeChange>& edges) noexcept;

// 끝 경로 path의 override 경로를 state에 넣는다(같은 경로가 있으면 무동작).
Result<void> AddOverride(AssignmentState& state, PropertyPath path) noexcept;

// 경로 연산 실패를 진단(값·경로 문제)이나 호출 오류(그 밖)로 옮긴다. 진단이면 Success다.
Result<void> ReportPathFailure(AssignmentState& state, ErrorCode code,
                               const PropertyPathFailure& failure, TextSource source,
                               StringView pathText) noexcept;

// 진단 하나를 남기고 묶음을 실패로 표시한다.
Result<void> Diagnose(AssignmentState& state, TextDiagnosticCode code, TextSource source,
                      StringView pathText, StringView message) noexcept;

// owner(단위 주소)가 만든 고정을 모두 놓는다(남은 고정의 순서는 그대로 — 할당하지 않는다). 단위를
// 버리거나 참조 단위의 새 대상을 바꿀 때 부른다(리플렉션 B9i 리뷰 반영).
void ReleasePins(AssignmentState& state, const void* owner) noexcept;

// 경로 path(대상 기준 — Map 항목 key 단계로 끝난다)의 키가 준비 상태에 있는지다(리플렉션 B9i 리뷰
// 반영 — JSON import의 MergeByKey). 그 자리의 값 단위가 이미 있으면 상자를, 없으면 대상을 본다.
// 준비 상태에서 닿을 수 없는 자리(unset Optional·범위 밖 index·없는 바깥 키)는 거짓이다. 오류: 호출
// 오류(할당·대상 문맥).
Result<bool> PreparedContains(AssignmentState& state, PropertyPath path) noexcept;

// 경로 path(대상 기준 — InstancedStruct 값으로 끝난다)의 준비 상태에서 지금 담긴 struct lease를
// out에 둔다(리플렉션 B9j — 비어 있거나 닿을 수 없으면 빈 lease). 보는 곳은 PreparedContains와
// 같다(그 자리의 값 단위 상자, 더한 StructRefArray 원소 블록, 없으면 대상). 대입 텍스트의
// InstancedStruct 뒤 멤버 경로 해석이 쓴다. 오류: 호출 오류(할당·대상 문맥).
Result<void> PreparedInstancedType(AssignmentState& state, PropertyPath path,
                                   TypeLease& out) noexcept;

// 대입 묶음의 경로 path(대상 기준)가 가리키는 자리를 out에 쓴다(DescribeTextPath + 준비 상태
// resolver — InstancedStruct 뒤 멤버 단계를 그 자리의 준비된 담긴 struct로 푼다, 리플렉션 B9j).
// 오류: DescribeTextPath와 같다.
Result<void> DescribeAssignmentPath(AssignmentState& state, PropertyPath path,
                                    TextSlot& out) noexcept;

// 대입 묶음의 경로 path(대상 기준)의 정규 텍스트를 out에 쓴다(진단 경로 — FormatPropertyPath +
// 문맥 KeyHook + 준비 상태 resolver라 InstancedStruct 내용 멤버도 쓴다). 쓸 수 없는 경로(resolver가
// 풀지 못한 단계·쓰지 못한 key)는 빈 텍스트다. 오류: 할당 실패.
Result<void> FormatAssignmentPath(AssignmentState& state, PropertyPath path, String& out) noexcept;

// 값 단위의 ReadOnly 자리마다 상자의 값이 대상의 지금 값과 같은지 깊게 대조하고, 다르면 진단
// ReadOnly를 남긴다(리플렉션 B9i 리뷰 반영 — PrepareAssignments·Append·ImportJson이 호출 끝에
// 부른다). 대조한 자리 목록은 비운다. 오류: 호출 오류(할당·대상 문맥).
Result<void> CheckReadOnlyUnits(AssignmentState& state) noexcept;

} // namespace rocks::reflection_text::detail
