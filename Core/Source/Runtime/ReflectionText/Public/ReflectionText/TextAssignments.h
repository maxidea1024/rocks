#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Object/DetachedPath.h>
#include <ReflectionText/TextValue.h>

// 텍스트 대입 묶음이다(리플렉션 B9i3c1 — Docs/Design/ReflectionExtensions.ko.md 10.2절). 명령행
// 대입(`<경로>=<값>`·`--unset=<경로>`·`--erase=<경로>`) 여러 개를 한 대상에 모아 해석·검증한 뒤
// (Prepare) 들여다보거나 덧붙이고(Touches·Append) 한 번에 게시한다(Publish).
//
// 동작:
//   - 대입마다 경로를 해석하고(ResolveTextPath) 값 텍스트를 읽는다(ParseTextValue — resolver도
//     여기서 부른다). 묶음 전체가 TextCodecContext 하나(입력 byte 누계·진단 한도)와 대상 타입의
//     TypeLease를 쥔다.
//   - 대입이 닿는 값 부분 트리(준비 단위 — 참조 없는 필드·멤버 하나)를 처음 닿을 때 사본으로
//     떼어 내고(PathDetach), 대입을 차례로 그 사본에 적용한다. 그래서 뒤 대입은 앞 대입이 만든
//     상태 위에서 해석된다(`anchor={}` 뒤 `anchor.x=1`).
//   - Publish는 바뀐 단위를 원래 자리와 무할당으로 맞바꾼다. 살아 있는 객체는 묶음 게시
//     구간(ObjectRegistry::BeginBatchPublish) 안에서 한 번에 게시하므로, 실패는 모두 구간에
//     들어가기 전에 나고 그때 객체는 바뀌지 않는다.
//   - 강한 참조(ObjectRef 필드·참조 멤버·Registry 컨테이너)에 닿는 대입은 아직 받지 않는다
//     (진단 Unsupported — 참조 단위는 B9i3c2).
namespace rocks {

class ConstructionContext;

} // namespace rocks

namespace rocks::reflection_text {

namespace detail {

struct AssignmentState;

} // namespace detail

// 대입 하나의 종류다.
enum class AssignmentKind : uint8 {
    // `<경로>=<값>` — 값 텍스트로 쓴다. Set 원소 자리(`tags[k]`)는 빈 값으로 원소를 넣는다.
    Set,
    // `--unset=<경로>` — Optional 필드를 unset으로 되돌린다.
    Unset,
    // `--erase=<경로>` — Map 항목·Set 원소(`m[k]`)나 Array 원소(`a[i]`)를 지운다.
    Erase
};

// 대입 하나다. path·value는 PrepareAssignments·Append 호출 동안만 빌린다(묶음은 해석한 결과만
// 담는다). sourceIndex는 진단의 위치(TextSource::Assignment)다.
struct TextAssignment {
    AssignmentKind kind = AssignmentKind::Set;
    // 도구 이름 경로(1.5절 — `SourceColorSettings.ColorSpace`·`scores["a=b"]`).
    StringView path;
    // 값 텍스트(Set만 — 10.4절 표기).
    StringView value;
    uint32 sourceIndex = 0;
};

// 대입 텍스트 `<경로>=<값>`을 Set 대입으로 나눠 out에 쓴다(SplitTextAssignment — `[...]`·key
// 따옴표 안의 `=`는 건너뛴다). `=`가 없으면 false다(out은 바뀌지 않는다). out은 text를 빌린다.
//
// 사용 예:
//   TextAssignment assignment;
//   if (!MakeTextAssignment("scores[\"a=b\"]=3", 0, assignment)) { /* 형식 오류 */ }
bool MakeTextAssignment(StringView text, uint32 sourceIndex, TextAssignment& out) noexcept;

// 대입 묶음의 대상이다. 값 타입이며 가리키는 Registry·트랜잭션·struct 값을 소유하지 않는다 —
// 그것들은 묶음(PreparedAssignments·PublishedAssignments)보다 오래 살아야 한다.
class AssignmentTarget {
public:
    enum class Kind : uint8 {
        Live,
        Staged,
        Value
    };

    // 게시된 생존 객체다(Publish가 묶음 게시 구간으로 한 번에 게시한다).
    static AssignmentTarget Live(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 ObjectHandle object) noexcept;

    // transaction의 공개 전 객체다(asset-new). ReadOnly 필드도 쓴다(초기값). 원자성은 호출자의
    // 트랜잭션 rollback이 준다.
    static AssignmentTarget Staged(ConstructionContext& transaction, ObjectHandle object) noexcept;

    // 등록 struct의 값 하나다: structType은 그 struct의 lease, storage는 그 C++ 값, allocator는 그
    // 값이 쓰는 allocator(사본·새 값이 이것으로 만들어져 값과 맞바뀐다)다. registry·access는 경로
    // 연산의 문맥(Name pool·ClassRef 등록 조회)이다. 강한 참조를 담은 struct 값은 대상이 될 수
    // 없다(참조 struct는 객체 안에서만 쓴다).
    static AssignmentTarget Value(ObjectRegistry& registry, const ObjectAccessContext& access,
                                  const TypeLease& structType, void* storage,
                                  Allocator& allocator) noexcept;

    Kind GetKind() const noexcept {
        return kind_;
    }

    ObjectRegistry& Registry() const noexcept {
        return *registry_;
    }

    const ObjectAccessContext& Access() const noexcept {
        return *access_;
    }

    ObjectHandle Handle() const noexcept {
        return object_;
    }

    ConstructionContext* Transaction() const noexcept {
        return transaction_;
    }

    const TypeLease* StructType() const noexcept {
        return structType_;
    }

    void* Storage() const noexcept {
        return storage_;
    }

    // 사본·새 값의 allocator다(Live·Staged는 ObjectRegistry::ObjectAllocator).
    Allocator& ValueAllocator() const noexcept;

private:
    AssignmentTarget() noexcept = default;

    Kind kind_ = Kind::Live;
    ObjectRegistry* registry_ = nullptr;
    const ObjectAccessContext* access_ = nullptr;
    ObjectHandle object_{};
    ConstructionContext* transaction_ = nullptr;
    const TypeLease* structType_ = nullptr;
    void* storage_ = nullptr;
    Allocator* allocator_ = nullptr;
};

// 게시를 마친 묶음이다(PreparedAssignments::Publish의 결과). override 경로와 게시한 단위의 옛 값을
// 담는다. 옛 값은 이 객체가 소멸할 때 반환한다(해제 콜백이 이 안에서 돈다). 이동만 된다.
class PublishedAssignments {
public:
    PublishedAssignments() noexcept;
    ~PublishedAssignments();
    PublishedAssignments(PublishedAssignments&&) noexcept;
    PublishedAssignments& operator=(PublishedAssignments&&) noexcept;
    PublishedAssignments(const PublishedAssignments&) = delete;
    PublishedAssignments& operator=(const PublishedAssignments&) = delete;

    // 게시한 override 경로다(PreparedAssignments::OverridePaths와 같다 — 값이 바뀌지 않은 대입
    // 포함, 값이 같은 ReadOnly 대입 제외).
    Span<const PropertyPath> OverridePaths() const noexcept;

    // 게시한(값을 맞바꾼) 단위 수다.
    usize UnitCount() const noexcept;

    // index번째 게시 단위의 경로(필드·멤버 하나 — 선택자 없음)다(index < UnitCount()).
    PropertyPath UnitPath(usize index) const noexcept;

    // index번째 게시 단위의 옛 값이다(Undo가 읽는다 — PathTarget::Value(box)로 경로 연산을 쓴다).
    DetachedValue& OldValue(usize index) noexcept;

    // 게시한 단일 강한 참조 위치(간선 변경 단위 — 리플렉션 B9i3c2) 수다. 값 단위(UnitCount)와 따로
    // 센다.
    usize ReferenceCount() const noexcept;

    // index번째 참조 위치의 경로(ObjectRef 필드·멤버, 정적 배열 원소면 그 index 선택자 포함)다
    // (index < ReferenceCount()).
    PropertyPath ReferencePath(usize index) const noexcept;

    // index번째 참조 위치가 게시 전에 가리키던 대상이다(null 가능 — Undo가 그 경로에 다시 쓴다).
    // 옛 대상을 고정하지 않으므로 Undo는 대상 생존을 다시 확인한다.
    ObjectHandle OldReference(usize index) const noexcept;

private:
    friend class PreparedAssignments;

    UniquePtr<detail::AssignmentState> state_;
};

// 해석·검증을 마치고 게시를 기다리는 대입 묶음이다(PrepareAssignments가 만든다). 이동만 된다. 소유
// 스레드(대상 Registry의 소유 스레드)에서 쓴다.
//
// 수명: PrepareAssignments에 준 TextDiagnostics·options.resolver·names, 대상(AssignmentTarget이
// 가리키는 것)은 이 객체(와 Publish 결과)보다 오래 살아야 한다. 묶음은 새로 가리키는 Weak 대상을
// RootRef로 고정하고 Publish가 끝나거나 소멸할 때 놓는다.
//
// 사용 예(A5 — 같은 명령에서 SRGB를 주지 않았으면 SRGB=off를 함께 저장):
//   const TextAssignment inputs[] = {{AssignmentKind::Set, "CompressionSettings", "Normal", 0}};
//   auto prepared = PrepareAssignments(AssignmentTarget::Live(registry, access, texture), inputs,
//                                      options, diagnostics);
//   if (!prepared) { /* diagnostics 보고 */ }
//
//   if (!prepared.Value().Touches(srgbPath)) {
//       if (auto added = prepared.Value().Append({AssignmentKind::Set, "SRGB", "off", 1}); !added)
//       {
//           ...
//       }
//   }
//
//   auto published = prepared.Value().Publish();
class PreparedAssignments {
public:
    PreparedAssignments() noexcept;
    ~PreparedAssignments();
    PreparedAssignments(PreparedAssignments&&) noexcept;
    PreparedAssignments& operator=(PreparedAssignments&&) noexcept;
    PreparedAssignments(const PreparedAssignments&) = delete;
    PreparedAssignments& operator=(const PreparedAssignments&) = delete;

    // 대입 하나를 덧붙인다(앞 대입이 만든 상태 위에서 해석 — PrepareAssignments의 대입과 같은
    // 규칙, resolver도 여기서 부른다). 진단은 PrepareAssignments에 준 TextDiagnostics로 간다.
    // 오류: 이미 게시했거나 앞 Append가 진단을 남긴 묶음은 InvalidArgument, 이 대입이 진단을
    // 남기면 InvalidArgument(그 뒤 묶음은 게시할 수 없다 — 대입이 사본에 일부만 적용됐을 수 있다),
    // 그 밖은 PrepareAssignments의 호출 오류.
    Result<void> Append(const TextAssignment& assignment) noexcept;

    // override 단위로 정규화한 경로다(중복 없음 — 선택자 없는 필드 경로): 경로를 위에서부터 보아
    // 처음 만나는 컨테이너 필드(Array·StaticArray·Map·Set·Optional)는 그 필드 경로, Atomic struct는
    // 그 struct 경로, 그 밖은 끝 경로다. 값이 바뀌지 않은 대입도 넣고, 값이 같아 무동작인 ReadOnly
    // 대입은 넣지 않는다. 겹치는 경로(`s`와 `s.x`)는 둘 다 남는다. 다음 Append까지 유효하다.
    Span<const PropertyPath> OverridePaths() const noexcept;

    // unit 경로가 override 경로 하나와 겹치는지(한쪽의 필드 ID 사슬이 다른 쪽의 앞부분인지)다.
    // unit의 선택자·key는 보지 않는다(`m[a]`는 override `m`과 겹친다).
    bool Touches(PropertyPath unit) const noexcept;

    // 묶음을 한 번 게시한다(성공·실패와 무관하게 다시 부르면 InvalidArgument). 순서:
    //   1. 바뀐 단위마다 게시 자리를 찾는다(PathPrepareAttach — 대상이 죽었으면 StaleHandle 등).
    //   2. Live: owner 읽기 Borrow → BeginBatchPublish(Prepare 때 읽은 변경 표지, 고정한 Weak
    //      대상) — 그 사이 owner가 Registry API로 바뀌었으면 Busy, Discard면 StaleHandle. 구간
    //      안에서 단위를 무할당으로 맞바꾸고 End가 표지를 올린다. Staged·Value: 단위를
    //      맞바꾼다(Staged는 트랜잭션이 끝났으면 그 오류).
    //   3. Weak 대상 고정을 놓는다.
    // 실패하면 대상은 바뀌지 않는다(Staged는 첫 단위 전에 실패한다). 성공하면 override 경로와 옛
    // 값을 돌려준다. 오류: 위 단계의 오류, 앞 Append가 진단을 남긴 묶음은 InvalidArgument.
    Result<PublishedAssignments> Publish() noexcept;

private:
    friend Result<PreparedAssignments> PrepareAssignments(const AssignmentTarget&,
                                                          Span<const TextAssignment>,
                                                          const TextCodecOptions&,
                                                          TextDiagnostics&) noexcept;

    UniquePtr<detail::AssignmentState> state_;
};

// assignments를 target에 차례로 해석·검증·준비한다(아무것도 게시하지 않는다).
//
// 대입마다: 경로 해석(ResolveTextPath — 입력 용도의 제외 판정 포함), 같은 경로 두 번이면
// DuplicateAssignment, 준비 단위 사본에 값을 적용한다:
//   - Set: 단일 값·원소·Map 값은 쓰기(Map에 없는 키는 넣기), 목록·Map 항목 목록은 비운 뒤 차례로
//     넣기(StaticArray는 원소마다 쓰기), `{}`는 struct 기본값(Optional<struct>는 값 만들기, Map 값
//     struct는 PathInsertKey, Array<struct> 원소는 그 자리 기본 원소 — StaticArray<struct> 원소는
//     Unsupported), Set 원소 자리는 빈 값이면 원소 넣기(값이 있으면 InvalidValue).
//   - Unset: Optional 필드만(아니면 InvalidValue). Erase: Map 항목·Set 원소·Array 원소(아니면
//     InvalidValue, StaticArray 원소는 Unsupported).
//   - 지금 값과 같은 단일 값 쓰기는 무동작이다(override 경로에는 넣는다). ReadOnly 자리(경로
//     어느 단계든)는 Staged면 쓰고, 아니면 값이 같은 단일 값 쓰기만 무동작으로 받고(override 경로에
//     넣지 않는다) 그 밖은 ReadOnly 진단이다. Deprecated 클래스 ClassRef도 같은 값은 무동작이고
//     다르면 InvalidValue다.
// 진단은 한도(options.maxDiagnostics) 안에서 모두 모은다(한 대입의 진단 뒤에도 다음 대입을 본다).
// options.names가 없으면 대상 Registry의 pool을 쓴다.
//
// 반환: 진단이 없으면 게시할 묶음. 오류: 진단을 남겼으면 InvalidArgument(내용은 diagnostics),
// 옵션 InvalidArgument·options.names가 대상 Registry의 pool과 다르면 InvalidArgument, 대상의
// 문맥·수명(AcquireType·MutationStamp — StaleHandle·WrongThread 등), 입력 한도 CapacityExceeded,
// 할당 OutOfMemory, 경로 연산의 Busy 등. 실패하면 대상은 바뀌지 않는다(Name intern은 남을 수 있다).
Result<PreparedAssignments> PrepareAssignments(const AssignmentTarget& target,
                                               Span<const TextAssignment> assignments,
                                               const TextCodecOptions& options,
                                               TextDiagnostics& diagnostics) noexcept;

} // namespace rocks::reflection_text
