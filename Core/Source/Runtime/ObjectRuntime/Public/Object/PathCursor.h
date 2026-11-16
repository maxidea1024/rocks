#pragma once

#include <Object/AssetId.h>
#include <Object/DetachedPath.h>
#include <Object/PathKey.h>
#include <Object/PropertyPath.h>

// 경로 접근 cursor의 공개 detail 선언이다(Docs/Design/ReflectionFoundations.ko.md 7.1절). 생성 경로
// 템플릿(Object/GeneratedPathAccess.h)이 멤버 함수를 부르므로 클래스 선언은 여기에 두고 구현은
// Private/PropertyPathCursor.cpp(연산 실행은 PropertyPathRunner.cpp)에 둔다. 사용자 코드는 쓰지
// 않는다 — 공개 API는 Object/PropertyPath.h의 handle + PropertyPath 연산뿐이다.
namespace rocks::detail {

// 경로 연산 종류다.
enum class PathOperation : uint8 {
    Read,
    Write,
    Length,
    Insert,
    Remove,
    Clear,
    // Optional<값 struct> 필드의 기본 값 만들기·비우기다(리플렉션 B9e).
    EmplaceOptional,
    ResetOptional,
    // Optional<값 struct> 필드의 set 여부 조회다(PathIsOptionalSet — 결과는 Length 칸에 0·1).
    QueryOptional,
    // Map 항목·Set 원소의 key 단계 연산이다(리플렉션 B9f3): 있는지 조회(결과는 Length 칸에 0·1),
    // Set 원소 넣기·Map<K, 값 struct> 기본 값 넣기·초기화(결과는 Length 칸에 새로 넣었으면 1),
    // 지우기.
    Contains,
    InsertKey,
    EraseKey,
    // 필드·멤버 값 하나를 상자로 깊게 복사하기(PathDetach)와 상자와 맞바꿀 자리 찾기
    // (PathPrepareAttach)다(리플렉션 B9i3c0).
    Detach,
    Attach
};

// 해석한 경로 단계 하나다: 필드(스키마 안 위치), 바깥 필드에서 누적한 유효 플래그, 대상
// 형식(배열이면 원소, Map이면 값, Set이면 원소)의 등록 enum·struct, 선택자와 원소 index, key
// 단계면 키 값(리플렉션 B9f3 — key는 PathKey wire를 빌린다, Name 키는 표시 문자열과 pool 조회
// 결과 — keyFound가 거짓이면 pool에 없는 이름이라 어느 컨테이너에도 없다, 새로 넣을 때는 넣기
// 준비가 intern한 토큰을 keyName에 둔다)와 키 enum 스키마.
struct PathFrame {
    const FieldSchema* field = nullptr;
    usize schemaIndex = 0;
    uint32 effectiveFlags = 0;
    const EnumSchema* enumeration = nullptr;
    const EnumSchema* keyEnumeration = nullptr;
    StructView target;
    uint32 index = 0;
    PathSelector selector = PathSelector::None;
    PathKeyValue key;
    Name keyName{};
    bool keyFound = true;
};

struct PathRunner;

// 경로 연산 하나의 상태다(스택 전용, 복사·이동 금지). 소유 타입 lease·참조 값 lease·Borrow·encode
// 고정을 연산 끝까지 쥐고(멤버 소멸 순서가 고정 → Borrow → lease다), 해석 프레임(최대
// MaxPropertyPathSteps)·연산 입출력·StructRefArray 원소 위치를 담는다. 생성 thunk는 자기 단계만
// 처리하고 다음 단계는 Descend로 넘긴다. 아래 멤버는 생성 템플릿용이며 오류를 돌려주는 함수는 실패
// 사유를 지금 단계에 기록한다(처음 기록한 것만 남는다). 크기는 약 3.4 KB(단계 프레임 17개 — key
// 단계의 키 값 포함)이고 경로 연산마다 스택에 하나 둔다(작은 스택 worker는 그 몫을 잡는다).
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

    // 시작 위치가 게시된 객체인지다(Registry 컨테이너 변경·live 참조 쓰기는 이때만 된다).
    bool IsLive() const noexcept {
        return root_ == PathTarget::Kind::Object;
    }

    // 쓰기 검증이 ReadOnly를 보지 않는 시작 위치인지다: staged 객체와 staged 객체에서 분리한 상자다
    // (리플렉션 B9i3c1 — Docs/Design/ReflectionExtensions.ko.md 10.2절).
    bool AcceptsReadOnlyWrites() const noexcept;

    // 시작 위치가 분리된 값(struct 값·상자)인지다(강한 참조 쓰기가 없다).
    bool IsDetached() const noexcept {
        return root_ == PathTarget::Kind::Struct || root_ == PathTarget::Kind::Detached;
    }

    // staged 시작 위치의 트랜잭션이다(아니면 nullptr).
    ConstructionContext* Construction() const noexcept {
        return construction_;
    }

    // 빈 자리(unset Optional 등)에 새로 만드는 값의 allocator다: 객체(게시·staged)면
    // ObjectRegistry::ObjectAllocator, 분리된 값이면 그 값의 allocator다.
    Allocator& ValueAllocator() const noexcept;

    // 게시된 객체에서만 되는 끝 지점(Registry 컨테이너의 변경)이 부른다: 아니면
    // Unsupported(NoPathAccess)를 기록한다.
    Result<void> RequireLive() noexcept;

    // 지금 연산이 대상을 바꾸지 않는 읽기(Read·Length·QueryOptional·Contains)인지다.
    bool IsReading() const noexcept {
        return operation_ == PathOperation::Read || operation_ == PathOperation::Length ||
               operation_ == PathOperation::QueryOptional || operation_ == PathOperation::Contains;
    }

    // Detach 끝 지점이다: value(ops 형식의 지금 값)를 출력 상자에 깊게 복사하고 상자를
    // 채운다(enter는 상자에서 경로를 시작하는 생성 함수). 게시된 객체면 고정 아래 복사하고 owner를
    // 다시 확인한다. copy·swap이 없으면 Unsupported(NotCopyable)다.
    Result<void> DetachValue(const RuntimeValueOps& ops, const void* value,
                             DetachedPathFunction enter) noexcept;

    // Attach 끝 지점이다: 상자 값의 형식이 ops와 같은지 대조하고(TypeMismatch(StaticTypeMismatch))
    // value 자리를 출력 PreparedAttach에 둔다(값은 바꾸지 않는다).
    Result<void> AttachValue(const RuntimeValueOps& ops, void* value) noexcept;

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

    // 정적 대조: 지금 단계의 대상 형식 종류(FieldTargetType — 컨테이너면 원소·값)가 target이고
    // 필드 값 종류가 wrapper(C++ 멤버의 컨테이너 종류 — Array·Optional·Map·Set, 단일 값·struct면
    // Opaque)와 맞는지다(StaticTypeMismatch — 스키마의 S 필드에 C++ Optional<S>·OrderedMap 멤버가
    // 오는 혼동을 막는다). wrapper가 Opaque면 필드 값 종류가 컨테이너가 아니어야 한다.
    Result<void> CheckTarget(ValueKind target, ValueKind wrapper = ValueKind::Opaque) noexcept;

    // 정적 대조: 지금 단계의 대상 enum ID가 GeneratedType<E>::Id()인지(StaticTypeMismatch).
    Result<void> CheckEnum(TypeId enumeration) noexcept;

    // 정적 대조: 지금 단계(Map 필드)의 키 종류가 key이고, 키가 enum이면 키 enum ID가
    // enumeration인지다(StaticTypeMismatch — 리플렉션 B9f3, Set은 원소를 CheckTarget이 본다).
    Result<void> CheckKey(ValueKind key, TypeId enumeration = TypeId{}) noexcept;

    // ClassRef 키를 넣을 때(key 단계 Write·InsertKey) 그 클래스가 정적 Base staticNative
    // (NativeTypeKey<Base>)의 C++ 파생인지다(리플렉션 B9f3). 등록·스키마 Base 파생은 넣기
    // 준비(PrepareInsertedKey)가 봤고 그 클래스 lease를 쥐었다. null 키면 성공, 아니면 TypeMismatch
    // (StaticTypeMismatch)다.
    Result<void> CheckKeyClass(const void* staticNative) noexcept;

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
    // index < size(아니면 InvalidArgument(IndexOutOfRange)). 선택자가 Index가 아니어도
    // InvalidArgument(IndexOutOfRange)다.
    Result<void> CheckIndex(usize size) noexcept;

    // 지금 단계의 컨테이너 필드(Array·Map·Set)에 newLength개를 둘 수 있는지다(MaxElements —
    // CapacityExceeded(TooManyElements)).
    Result<void> CheckLength(usize newLength) noexcept;

    // 지금 단계(마지막 key 단계)의 키를 컨테이너에 새로 넣기 전에 준비한다(리플렉션 B9f3 — 키가
    // 없을 때만 부른다, 있는 키의 교체·무동작 넣기는 부르지 않는다). ClassRef 키는 이 Registry에
    // 등록된 클래스이고 스키마 Base의 파생이어야 하고(lease를 쥔다 — CheckKeyClass가 정적 Base를
    // 대조한다, 미등록 NotFound·그 밖 InvalidArgument — 사유 InvalidKey), Name 키는 내용 규칙
    // (ValidateNameFieldText — InvalidArgument(InvalidKey)) 뒤 그 표시 문자열을 Registry pool에
    // intern한다(pool 없는 Registry의 None이 아닌 키는 Unsupported(InvalidKey), intern 오류는
    // 그대로). 할당 콜백의 재진입 쓰기를 막도록 고정을 쥔 뒤에 부른다.
    Result<void> PrepareInsertedKey() noexcept;

    // key 단계에서 키가 없을 때의 실패를 기록한다(NotFound(KeyNotFound)).
    Result<void> FailMissingKey() noexcept {
        return Fail(ErrorCode::NotFound, PropertyPathError::KeyNotFound);
    }

    // PathCount·PathContains·PathInsertKey·PathIsOptionalSet의 결과를 기록한다.
    void SetLength(usize length) noexcept {
        length_ = length;
    }

    // weak 참조 쓰기·삽입(Input()의 reference)의 대상이 정적 대상 타입 staticNative
    // (NativeTypeKey<T>)의 native 파생인지다(리플렉션 B9d1). 대상의 생존·스키마 기대 타입은 검증
    // 지점(PathRunner — ValidateFieldWrite)이 이미 봤다. null이면 성공, 아니면 TypeMismatch
    // (StaticTypeMismatch)다.
    Result<void> CheckWeakTarget(const void* staticNative) noexcept;

    // ClassRef 쓰기·삽입(Input()의 guid)의 클래스가 정적 Base staticNative(NativeTypeKey<Base>)의
    // C++ 파생인지다(리플렉션 B9d2). 등록·스키마 Base 파생은 검증 지점(PathRunner —
    // ValidateClassReference)이 이미 봤고 그 클래스 lease를 쥐었다. null이면 성공, 아니면
    // TypeMismatch(StaticTypeMismatch)다. 클래스가 Deprecated면(리플렉션 B9h2) 그 자리의 지금 값
    // current와 같을 때만 받는다(같은 값의 재기록 — 새 원소·Map 항목은 current 0이라 거부,
    // InvalidArgument(InvalidValue)).
    Result<void> CheckClassTarget(const void* staticNative, TypeId current = TypeId{}) noexcept;

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

    PathCursor(ObjectRegistry& registry, const ObjectAccessContext& access,
               const PathTarget& target, PathOperation operation, const FieldValue* input,
               FieldValue* output, PropertyPathFailure* failure) noexcept;

    void Record(ErrorCode code, PropertyPathError reason) noexcept;

    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    ObjectHandle owner_;
    // 시작 위치(PathTarget — 리플렉션 B9i3c0): staged 트랜잭션, struct 값의 lease·view·저장소·
    // allocator, 상자.
    PathTarget::Kind root_;
    ConstructionContext* construction_;
    const TypeLease* rootLease_;
    StructView rootView_;
    void* rootValue_;
    Allocator* rootAllocator_;
    DetachedValue* rootBox_;
    // Detach의 출력 상자·Attach의 입력 상자와 Attach의 출력 자리다.
    DetachedValue* box_ = nullptr;
    PreparedAttach* attach_ = nullptr;
    PathOperation operation_;
    const FieldValue* input_;
    FieldValue* output_;
    PropertyPathFailure* failure_;
    TypeLease ownerType_;
    TypeLease expectedType_;
    TypeLease referencedType_;
    TypeLease keyClass_;
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
