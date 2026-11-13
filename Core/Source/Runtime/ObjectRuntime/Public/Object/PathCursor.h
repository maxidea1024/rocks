#pragma once

#include <Object/AssetId.h>
#include <Object/PropertyPath.h>

// 경로 접근 cursor의 공개 detail 선언이다(Docs/Design/ReflectionFoundations.ko.md 7.1절). 생성 경로
// 템플릿(Object/GeneratedPathAccess.h)이 멤버 함수를 부르므로 클래스 선언은 여기에 두고 구현은
// Private/PropertyPath.cpp에 둔다. 사용자 코드는 쓰지 않는다 — 공개 API는 Object/PropertyPath.h의
// handle + PropertyPath 연산뿐이다.
namespace rocks::detail {

// 경로 연산 종류다.
enum class PathOperation : uint8 {
    Read,
    Write,
    Length,
    Insert,
    Remove,
    Clear
};

// 해석한 경로 단계 하나다: 필드(스키마 안 위치), 바깥 필드에서 누적한 유효 플래그, 대상
// 형식(배열이면 원소)의 등록 enum·struct, 원소 index.
struct PathFrame {
    const FieldSchema* field = nullptr;
    usize schemaIndex = 0;
    uint32 effectiveFlags = 0;
    const EnumSchema* enumeration = nullptr;
    StructView target;
    uint32 index = 0;
    bool indexed = false;
};

struct PathRunner;

// 경로 연산 하나의 상태다(스택 전용, 복사·이동 금지). 소유 타입 lease·참조 값 lease·Borrow·encode
// 고정을 연산 끝까지 쥐고(멤버 소멸 순서가 고정 → Borrow → lease다), 해석 프레임(최대
// MaxPropertyPathSteps)·연산 입출력·StructRefArray 원소 위치를 담는다. 생성 thunk는 자기 단계만
// 처리하고 다음 단계는 Descend로 넘긴다. 아래 멤버는 생성 템플릿용이며 오류를 돌려주는 함수는 실패
// 사유를 지금 단계에 기록한다(처음 기록한 것만 남는다).
class PathCursor {
public:
    PathCursor(const PathCursor&) = delete;
    PathCursor& operator=(const PathCursor&) = delete;
    PathCursor(PathCursor&&) = delete;
    PathCursor& operator=(PathCursor&&) = delete;
    ~PathCursor() = default;

    PathOperation Operation() const noexcept {
        return operation_;
    }

    // 지금 처리하는 단계다.
    const PathFrame& Frame() const noexcept {
        return frames_[current_];
    }

    // 지금 단계가 경로의 마지막 단계인지다.
    bool IsLast() const noexcept {
        return current_ + 1 == count_;
    }

    ObjectRegistry& Registry() const noexcept {
        return registry_;
    }

    const ObjectAccessContext& Access() const noexcept {
        return access_;
    }

    ObjectHandle OwnerHandle() const noexcept {
        return owner_;
    }

    // Write·Insert의 입력 값이다(Insert의 struct 원소는 nullptr). 검증을 마친 값이다.
    const FieldValue* Input() const noexcept {
        return input_;
    }

    // Read의 out, Write의 previous, Remove의 removed다(주지 않았으면 nullptr — Read는 항상 있다).
    FieldValue* Output() const noexcept {
        return output_;
    }

    // 클래스 필드 thunk가 소유 객체 주소를 얻는다. 실제 객체 타입이
    // ownerNative(NativeTypeKey<Owner>)의 타입이거나 파생이 아니면
    // TypeMismatch(StaticTypeMismatch)다.
    Result<Object*> Owner(const void* ownerNative) noexcept;

    // 정적 대조: 지금 단계의 대상 형식 종류가 target이고 배열 여부가 container와 같은지
    // (StaticTypeMismatch).
    Result<void> CheckTarget(ValueKind target, bool container) noexcept;

    // 정적 대조: 지금 단계의 대상 enum ID가 GeneratedType<E>::Id()인지(StaticTypeMismatch).
    Result<void> CheckEnum(TypeId enumeration) noexcept;

    // 정적 대조: 지금 단계의 대상 struct(배열이면 원소 struct)의 native 키가 nativeType
    // (NativeTypeKey<S>)인지(StaticTypeMismatch — MatchesStructView와 같은 형태).
    Result<void> CheckStruct(const void* nativeType) noexcept;

    // 정적 대조: struct 멤버 thunk가 받은 struct 값의 타입(바로 바깥 단계의 대상 struct)이
    // structNative(NativeTypeKey<S>)인지(StaticTypeMismatch).
    Result<void> CheckMemberOwner(const void* structNative) noexcept;

    // structValue(지금 단계의 대상 struct 값 — CheckStruct로 타입을 대조한 주소)에서 다음 단계
    // 멤버의 경로 thunk를 부른다. 다음 단계가 없거나 structValue가 null이면 InvalidArgument, 다음
    // 멤버의 thunk가 없으면 Unsupported(NoPathAccess)다.
    Result<void> Descend(void* structValue) noexcept;

    // StructRefArray 원소로 들어간다(그 아래 참조 멤버 쓰기는 이 위치의 SetElementReference).
    void EnterElement(const StructElementLocation& element) noexcept {
        element_ = element;
        inElement_ = true;
    }

    // 지나온 StructRefArray 원소 위치다(없으면 nullptr).
    const StructElementLocation* Element() const noexcept {
        return inElement_ ? &element_ : nullptr;
    }

    // 지금 단계의 원소 index를 길이 size로 검사한다: 마지막 단계의 Insert는 index ≤ size, 그 밖은
    // index < size(아니면 InvalidArgument(IndexOutOfRange)).
    Result<void> CheckIndex(usize size) noexcept;

    // 지금 단계의 배열 필드에 newLength개를 둘 수 있는지다(MaxElements — CapacityExceeded
    // (TooManyElements)).
    Result<void> CheckLength(usize newLength) noexcept;

    // PathArrayLength의 결과를 기록한다.
    void SetLength(usize length) noexcept {
        length_ = length;
    }

    // 할당하는 쓰기 끝 지점이 할당 전에 owner를 encode 고정한다(이미 고정했으면 그대로 성공 —
    // Read는 처음부터 고정한다). 고정 중 재진입 쓰기·Registry 컨테이너 변경·SetEdge는 Busy다.
    Result<void> Pin() noexcept;

    // 게시 직전에 owner의 논리 수명을 다시 확인한다(Borrow.Get — 고정은 RequestDestroy를 막지
    // 않는다). Destroyed 등이면 게시하지 않는다.
    Result<void> CheckOwner() noexcept;

    // 게시 뒤 고정을 푼다(옛 값·옛 버퍼는 그 뒤에 반환한다).
    void ReleasePin() noexcept {
        pin_.Reset();
    }

    // Read: owner를 다시 확인한 뒤 고정을 풀고 value를 out으로 옮긴다(out의 옛 버퍼는 고정 밖에서
    // 반환한다).
    Result<void> PublishRead(FieldValue& value) noexcept;

    // 성공한 쓰기·삭제의 옛 값(value)을 Output()으로 옮긴다. 고정을 푼 뒤에 부른다. Output()이
    // 없으면 아무것도 하지 않는다.
    void PublishOutput(FieldValue& value) noexcept;

    // 성공한 참조 쓰기·삭제의 옛 handle을 Output()에 담는다(없으면 아무것도 하지 않는다).
    void PublishReference(ObjectHandle previous) noexcept;

    // String·Soft 경로(text, Soft면 asset도) 끝 지점의 읽기·쓰기다(Read는 out의 allocator로 복사,
    // Write는 text의 allocator로 새 값을 만들어 고정 아래 owner 재확인 뒤 Swap으로 게시하고
    // previous를 담는다).
    Result<void> ReadText(const String& text, const AssetId* asset) noexcept;
    Result<void> WriteText(String& text, AssetId* asset) noexcept;

    // 삭제할 String·Soft 원소의 값을 captured(Output()의 allocator)에 담을 준비를 한다: allocator가
    // 같으면 moveText = true(게시 뒤 버퍼를 옮긴다), 다르면 고정 아래 복사하고 owner를 다시
    // 확인한다.
    Result<void> CaptureText(const String& text, const AssetId* asset, FieldValue& captured,
                             bool& moveText) noexcept;

    // 실패를 지금 단계에 reason으로 기록하고 code를 돌려준다. 실패를 기록하면 고정을 곧바로 푼다
    // (Fail·FailValue·Finish·cursor의 오류 반환 모두) — 호출한 thunk 지역의 임시 버퍼는 그 뒤에
    // 반환된다.
    Result<void> Fail(ErrorCode code, PropertyPathError reason = PropertyPathError::None) noexcept;

    // 값 쓰기(WriteTargetValue)의 실패를 기록한다: TypeMismatch는 StaticTypeMismatch(정적 enum ID),
    // InvalidArgument는 InvalidValue, 그 밖(Overflow·OutOfMemory)은 None이다.
    Result<void> FailValue(ErrorCode code) noexcept;

    // Registry 호출 결과를 그대로 돌려주되 실패면 사유 None으로 기록한다.
    Result<void> Finish(Result<void> result) noexcept;

private:
    friend struct PathRunner;

    PathCursor(ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle owner,
               PathOperation operation, const FieldValue* input, FieldValue* output,
               PropertyPathFailure* failure) noexcept;

    void Record(ErrorCode code, PropertyPathError reason) noexcept;

    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    ObjectHandle owner_;
    PathOperation operation_;
    const FieldValue* input_;
    FieldValue* output_;
    PropertyPathFailure* failure_;
    TypeLease ownerType_;
    TypeLease expectedType_;
    TypeLease referencedType_;
    Borrow<Object> borrow_;
    EncodePin pin_;
    Object* object_ = nullptr;
    const RuntimeFieldOps* topOps_ = nullptr;
    PathFrame frames_[MaxPropertyPathSteps];
    usize count_ = 0;
    usize current_ = 0;
    usize length_ = 0;
    StructElementLocation element_{};
    bool inElement_ = false;
    bool failed_ = false;
};

} // namespace rocks::detail
