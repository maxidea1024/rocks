#pragma once

#include <Object/FieldEncodeContext.h>

// 필드 codec의 문맥·정책이다(Docs/Design/ReflectionFoundations.ko.md 6.1–6.4절 — hook 묶음과 진단·
// 외부 참조 정보는 Object/FieldCodecHooks.h). 생성 codec
// thunk(RuntimeFieldOps::encode·decode)는 이 문맥으로만 참조 wire·진단·Soft 수집·예산에 닿으므로,
// 그래프 형식(graph v2의 U32 local ID·외부 참조 비트)이나 저장 집합이 바뀌어도 생성 코드를 다시
// 만들지 않는다. 직렬화기(ObjectSerialization)가 문맥을 구현하고 필드마다 FieldScope로 위치를
// 정한다. 이 헤더는 생성 헤더와 직렬화기만 include한다.
//
// 문맥은 앞 조각을 include하는 사슬로 나뉜다: 정책·위치 프레임·참조 형식
// (Object/FieldCodecPolicy.h), 공통 기반 FieldCodecContext와 위치 scope
// (Object/FieldCodecContext.h), 저장 문맥(Object/FieldEncodeContext.h). 이 헤더에는 복원 문맥
// (FieldDecodeContext), Record 스택 scope(RecordScope), StructRefArray 원소 scope
// (StructElementScope)와 사슬의 out-of-line inline 정의(OpenRecordBytes·PushRecord·BuildPath·
// Report·CollectSoftReference)가 있다.
namespace rocks {

// 복원 문맥이다(그래프 복원이 1단계·2단계로 구현한다).
class FieldDecodeContext : public FieldCodecContext {
public:
    const FieldDecodePolicy& Policy() const noexcept {
        return policy_;
    }

    // 이 유효 플래그의 필드·멤버가 이 데이터에 없어야 하는지다(policy.excludedFlags — 쿠킹 로드의
    // EditorOnly). 있으면 직렬화기·struct codec이 InvalidArgument로 거부한다.
    bool IsExcluded(uint32 effectiveFlags) const noexcept {
        return (effectiveFlags & policy_.excludedFlags) != 0;
    }

    // 지금 위치(Current())의 필드·멤버가 저장 데이터에 있음을 알린다. 복원 1단계가 필드·멤버마다 그
    // 값을 검증한 뒤 한 번 부른다(객체 Record는 직렬화기, struct 멤버는 DecodeStructRecord).
    // 기록하지 않는 단계(복원 2단계)나 위치 밖에서는 아무것도 하지 않는다. A5 override 집합("파일에
    // 있는 값 경로 = 집합" — Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 결정')의
    // 자리이며 지금은 NotePresent의 기본 구현이 아무것도 하지 않는다. 실패를 돌려주면 그 복원이
    // 실패한다.
    Result<void> MarkPresent() noexcept {
        if (!Recording() || !Current()) {
            return Result<void>::Success();
        }

        return NotePresent(*Current());
    }

    // 필드 payload를 읽는 reader를 연다. Record 스택(struct 값)이 있으면 맨 위 Record로, 없으면
    // 직렬화기의 최상위 경로(OpenTopLevelValue)로 연다. 1단계는 그래프 공유 예산, 2단계는 같은 크기
    // 한도와 무제한 operation·work다.
    Result<ArchiveReader> OpenValue(const FieldView& field) noexcept {
        if (depth_ != 0) {
            return Top().OpenValue(field);
        }

        return OpenTopLevelValue(field);
    }

    // 맨 위 Record reader다(struct 값을 해석하는 동안 — 없으면 nullptr). Next()로 순회한다.
    RecordReader* CurrentRecord() noexcept {
        return depth_ != 0 ? &Top() : nullptr;
    }

    // struct 값의 중첩 Record를 열어 Record 스택에 넣는다(wire가 Record가 아니면 TypeMismatch).
    // 1단계는 맨 위(없으면 객체) Record의 OpenRecordField(공유 예산 — 헤더·색인만 청구, 필드 수
    // 한도는 남은 그래프 누계)와 AddFields, 2단계는 같은 버전 범위·크기 한도·무제한
    // operation·work다.
    virtual Result<RecordScope> OpenRecord(const FieldView& field, TypeId expected,
                                           uint32 minVersion, uint32 maxVersion) noexcept = 0;

    // 마이그레이션 결과 Record를 정확히 version으로 열어 Record 스택에 넣는다(아니면 Unsupported).
    virtual Result<RecordScope> OpenMigratedRecord(Span<const uint8> record, TypeId expected,
                                                   uint32 version) noexcept = 0;

    // 필드 값 안에 놓인 Record 내용(Optional<값 struct>의 내부 — OptionalWireReader::RecordBytes,
    // 리플렉션 B9e. 뒤 단계의 Map 값·InstancedStruct도 같다)을 Record 스택에 넣는다(Docs/Design/
    // ReflectionExtensions.ko.md 1.2절 "구현(B9a3)에서 정한 점"). record는 지금 위치의 필드 payload
    // 안을 빌린 범위다. 직렬화기 구현 규칙:
    // - 1단계: 맨 위(없으면 객체) Record의 RecordReader::OpenNestedRecord(공유 예산 — 색인만 청구,
    //   필드 수 한도는 남은 그래프 누계)와 AddFields. 맨 위 Record가 없으면(최상위 필드) 2단계가
    //   단독 reader로 다시 열며 청구할 몫(Optional 내용 넘기기와 단독 Record Open — record byte의
    //   두 배)을 미리 청구한다("1단계 청구가 2단계 몫을 덮는다").
    // - 2단계: 스택이 비면 같은 버전 범위·크기 한도·무제한 operation·work의 단독 Record Open
    //   (RecordReader::Open), 있으면 맨 위의 OpenNestedRecord.
    // 오류는 OpenRecord와 같다(범위 밖·빈 범위·버전 0 InvalidArgument, 범위 밖 버전 Unsupported,
    // schema 불일치 TypeMismatch, 필드 수 초과 CapacityExceeded). 기본은 Unsupported다(Record
    // 내용을 지원하지 않는 문맥 — 정의는 RecordScope 뒤).
    //
    // 사용 예(Optional<struct> codec):
    //   auto scope = context.OpenRecordBytes(optional.RecordBytes(), structId, 1,
    //                                        NumericLimits<uint32>::Max());
    virtual Result<RecordScope> OpenRecordBytes(Span<const uint8> record, TypeId expected,
                                                uint32 minVersion, uint32 maxVersion) noexcept;

    // 단일 값 원소 배열 필드(wire ValueArray)를 연다(Docs/Design/ReflectionFoundations.ko.md
    // 5.6절): wire가 ValueArray가 아니면 TypeMismatch, 그 뒤 OpenValue로 연 payload를
    // ValueArrayReader::Open(elementWire, maxCount, shape)에 넘긴다(그 검사 순서·청구 — 1단계는
    // 그래프 공유 예산, 2단계는 무제한). codec은 maxCount에 MaxArrayElements를 넘긴다(로드는 필드별
    // maxElements를 걸지 않는다).
    //
    // 사용 예:
    //   auto array = context.OpenValueArray(field, static_cast<uint32>(WireType::U32),
    //                                       MaxArrayElements);
    //   if (!array) { return Result<void>::Failure(array.Error()); }
    Result<ValueArrayReader> OpenValueArray(const FieldView& field, uint32 elementWire,
                                            uint32 maxCount, WireValueShape shape = {}) noexcept {
        if (field.wireType != static_cast<uint32>(WireType::ValueArray)) {
            return Result<ValueArrayReader>::Failure(ErrorCode::TypeMismatch);
        }

        auto opened = OpenValue(field);
        if (!opened) {
            return Result<ValueArrayReader>::Failure(opened.Error());
        }

        return ValueArrayReader::Open(Move(opened.Value()), elementWire, maxCount, shape);
    }

    // struct 원소 배열 필드(wire RecordList)를 연다. wire가 RecordList가 아니면 TypeMismatch다.
    // 항목 schema는 itemSchema, 항목 버전 범위는 (1, UINT32_MAX)이며(버전 판정은 codec) 목록 한도는
    // 직렬화기가 정한다(그래프는 {MaxArrayElements, maxValueBytes, maxValueBytes − 12} — 저장
    // 한도와 같다). 1단계는 맨 위(없으면 객체) Record의 RecordListReader::OpenField(공유 예산 —
    // 색인만 청구)이고 맨 위 Record가 없으면 2단계 단독 Open 몫(payload byte)을 미리 청구한다.
    // 2단계는 스택이 비면 같은 한도의 단독 Open(무제한 operation·work), 있으면 맨 위의 OpenField다.
    // 반환 reader는 이 필드 payload를 빌리며 호출자가 OpenListItem을 다 쓸 때까지 쥔다.
    virtual Result<RecordListReader> OpenRecordList(const FieldView& field,
                                                    TypeId itemSchema) noexcept = 0;

    // list의 index번째 항목 Record를 Record 스택에 넣는다. 1단계는 OpenItem(index, 남은 그래프 필드
    // 누계)과 AddFields(항목 TLV 수), 2단계는 OpenItem(index, maxTotalFields)다. 오류는
    // OpenRecord와 같다(범위 밖 index·버전 0 InvalidArgument, schema 불일치 TypeMismatch, 필드 수
    // 초과 CapacityExceeded).
    virtual Result<RecordScope> OpenListItem(RecordListReader& list, usize index) noexcept = 0;

    // 2단계 codec이 지금 적용하는 StructRefArray 원소의 위치다(StructElementScope 안 — 없으면
    // nullptr). 원소 안 참조 멤버 쓰기(detail::SetStructMemberReference)가 이것이 있으면
    // ConstructionContext::SetElementReference(컨테이너 기준 범위 검사)로 보낸다.
    const detail::StructElementLocation* StructElement() const noexcept {
        return element_;
    }

    // struct의 옛 버전 Record를 현재 버전으로 올린다. 1단계는 로드 옵션 override(Unsupported면
    // 다음)·타입이 등록한 마이그레이션 순으로 부르고(둘 다 없으면 Unsupported) 결과를 발생 순서대로
    // 보관해 view를 돌려준다. 2단계는 같은 순서의 보관본을 대조해 돌려준다(어긋나면 InvalidArgument
    // — 계약 위반·rollback).
    virtual Result<Span<const uint8>> MigrateStruct(TypeId type, uint32 fromVersion,
                                                    uint32 toVersion,
                                                    Span<const uint8> record) noexcept = 0;

    // 이 struct ID를 1단계에서 관찰하는지다(로드 시작 때 직렬화기가 정한 목록).
    bool IsObservedStruct(TypeId type) const noexcept {
        if (!Recording()) {
            return false;
        }

        usize low = 0;
        usize high = observed_.Size();
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            const TypeId id = observed_[middle];
            if (id == type) {
                return true;
            }

            if (id.high < type.high || (id.high == type.high && id.low < type.low)) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return false;
    }

    // 지금 위치의 관찰 대상 struct 값을 hook에 넘긴다.
    Result<void> ObserveStruct(TypeId type, const void* nativeKey, const void* value) noexcept {
        FieldCodecHooks* hooks = Hooks();
        if (!Recording() || !hooks || !Current()) {
            return Result<void>::Success();
        }

        return hooks->ObserveStruct(*Current(), type, nativeKey, value);
    }

    // 강한 참조 하나를 읽는다(ReferenceWireType() 형식). 1단계는 token 범위를 검사하고 기대 형식과
    // 함께 기록한다.
    virtual Result<DecodedReference>
    ReadReference(ArchiveReader& reader, const ReferenceExpectation& expectation) noexcept = 0;

    // weak 참조 하나를 읽는다(리플렉션 B9d1 — Docs/Design/ReflectionExtensions.ko.md 5.1절,
    // ReferenceWireType() 형식). 같은 그래프 안 노드 번호만 받는다: 외부 참조 번호(graph v2의
    // 0x80000000 | i)는 writer가 쓰지 않으므로 InvalidArgument이고, 노드 번호의 대상은 강한 참조와
    // 같은 기대 형식 규칙으로 판정한다(1단계가 기록해 모든 노드를 해석한 뒤 TypeMismatch). 1단계는
    // isNull만 정확하고, 2단계는 만든 노드 객체 handle이다. weak 간선은 도달 검사·GC·관찰자의
    // 대상이 아니다. 기본은 Unsupported다(weak 복원을 지원하지 않는 문맥).
    //
    // 사용 예(생성 codec이 부르는 형태 — ReadValueWire의 WeakObjectReference 갈래):
    //   auto decoded = context.ReadWeakReference(reader, {type.type, staticNative});
    //   if (!decoded) {
    //       return Result<void>::Failure(decoded.Error());
    //   }
    //
    //   out.reference = decoded.Value();   // 2단계면 handle로 WeakRef<T>를 만든다
    virtual Result<DecodedReference>
    ReadWeakReference(ArchiveReader& reader, const ReferenceExpectation& expectation) noexcept {
        (void)reader;
        (void)expectation;
        return Result<DecodedReference>::Failure(ErrorCode::Unsupported);
    }

    // 복원 1단계(기록하는 단계)가 내용 규칙(ValidateNameFieldText)을 지난 Name 표시 문자열 하나를
    // 방문 순서대로 기록한다(Docs/Design/ReflectionExtensions.ko.md 4.1절). 1단계는 ObjectRegistry·
    // pool에 닿지 않으므로 intern은 2a(CreateObjects 사전 확인)가 대상 Registry의 pool로 일괄 하고,
    // 1단계가 그 몫(byte + 상수)을 여기서 미리 청구한다. text는 복원 입력(또는 1단계가 보관한
    // 마이그레이션 결과)을 빌린 view이며 Stage 수명 동안 유효하다. 값 하나에 한 번이다(None
    // 문자열도 기록한다 — 2a가 기본 None으로 둔다). 기본은 Unsupported다(Name 복원을 지원하지 않는
    // 문맥).
    virtual Result<void> CollectName(StringView text) noexcept {
        (void)text;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 복원 2단계가 1단계와 같은 방문 순서로 Name 토큰 하나를 받는다(2a가 intern한 토큰 — 대상
    // Registry pool의 것이고 내용 검사를 지났다). text는 2단계가 다시 읽은 표시 문자열이며 1단계
    // 기록과 byte가 다르거나 기록이 남지 않았으면 두 단계의 방문 순서가 어긋난 계약 위반이라
    // InvalidArgument다. 기본은 Unsupported다.
    virtual Result<Name> TakeName(StringView text) noexcept {
        (void)text;
        return Result<Name>::Failure(ErrorCode::Unsupported);
    }

    // 복원 1단계가 null이 아닌 저장 ClassRef 값 하나(stored)의 판정을 방문 순서대로 기록한다(nulled
    // = 미등록 클래스를 decode 정책 ReportAndNull로 null로 바꿨다 — Docs/Design/
    // ReflectionExtensions.ko.md 5.2절). 2단계는 TypeRegistry를 다시 조회하지 않고 이 기록을 쓰므로
    // 두 단계 사이에 등록이 바뀌어도 결과는 1단계 판정·진단과 같다. 기록 몫은 구현이 1단계에
    // 청구한다. 기본은 Unsupported다(ClassRef 복원을 지원하지 않는 문맥).
    virtual Result<void> CollectClassReference(TypeId stored, bool nulled) noexcept {
        (void)stored;
        (void)nulled;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 복원 2단계가 1단계와 같은 방문 순서로 ClassRef 판정 하나를 받는다: 1단계 판정 뒤의 값(stored
    // 그대로이거나 null). stored가 기록과 다르거나 기록이 남지 않았으면 두 단계의 방문 순서가
    // 어긋난 계약 위반이라 InvalidArgument다. 기본은 Unsupported다.
    virtual Result<TypeId> TakeClassReference(TypeId stored) noexcept {
        (void)stored;
        return Result<TypeId>::Failure(ErrorCode::Unsupported);
    }

protected:
    FieldDecodeContext(const FieldDecodePolicy& policy, FieldCodecHooks* hooks,
                       uint32 referenceWire) noexcept
        : FieldCodecContext(hooks, referenceWire), policy_(policy) {
    }

    ~FieldDecodeContext() {
        if (depth_ != 0) {
            platform::Fatal("FieldDecodeContext destroyed with open record scopes");
        }
    }

    // Record 스택이 비었을 때의 값 reader다(1단계는 객체 Record, 2단계는 보관한 FieldView).
    virtual Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept = 0;

    // 직렬화기가 연 reader를 Record 스택에 넣는다(B5 RecordList 항목 reader도 같은 경로).
    Result<RecordScope> PushRecord(RecordReader&& reader) noexcept;

    // 관찰할 struct ID다(ID 순으로 정렬된 span — 로드 동안 유효해야 한다).
    void SetObservedStructs(Span<const TypeId> sorted) noexcept {
        observed_ = sorted;
    }

    // MarkPresent가 부르는 present 판정 hook이다(frame = 파일에 있는 필드·멤버의 위치, 호출 동안만
    // 유효). 기본은 아무것도 하지 않는다 — A5가 override 집합을 채울 때 직렬화기 문맥이 구현한다
    // (Docs/Design/ObjectFoundations.ko.md 4절, 공개 hook 여부는 A5가 정한다).
    virtual Result<void> NotePresent(const FieldCodecFrame& frame) noexcept {
        (void)frame;
        return Result<void>::Success();
    }

private:
    friend class RecordScope;
    friend class StructElementScope;

    static constexpr uint32 MaxRecordDepth = MaxStructDepth + 1;

    RecordReader& Top() noexcept {
        return *slots_[depth_ - 1];
    }

    FieldDecodePolicy policy_;
    Span<const TypeId> observed_{};
    const detail::StructElementLocation* element_ = nullptr;
    alignas(RecordReader) uint8 storage_[MaxRecordDepth * sizeof(RecordReader)];
    // placement new가 돌려준 포인터다(저장소 주소를 다시 해석하지 않는다).
    RecordReader* slots_[MaxRecordDepth] = {};
    uint32 depth_ = 0;
};

// FieldDecodeContext의 Record 스택 한 칸을 쥔다(RAII). 이동할 수 있고(이동된 원본은 아무것도 하지
// 않는다) 들어간 순서의 역순으로 끝나야 한다(아니면 Fatal). Close로 일찍 닫을 수 있다(마이그레이션
// 전에 원본 Record를 닫는다).
//
// 사용 예(struct codec):
//   auto scope = context.OpenRecord(field, structId, 1, NumericLimits<uint32>::Max());
//   if (!scope) { return Result<void>::Failure(scope.Error()); }
//   RecordReader& record = *context.CurrentRecord();
class [[nodiscard]] RecordScope {
public:
    RecordScope(RecordScope&& other) noexcept : context_(other.context_), depth_(other.depth_) {
        other.context_ = nullptr;
    }

    RecordScope& operator=(RecordScope&&) = delete;
    RecordScope(const RecordScope&) = delete;
    RecordScope& operator=(const RecordScope&) = delete;

    ~RecordScope() {
        Close();
    }

    void Close() noexcept {
        if (!context_) {
            return;
        }

        if (context_->depth_ != depth_) {
            platform::Fatal("RecordScope ended out of order");
        }

        context_->Top().~RecordReader();
        --context_->depth_;
        context_ = nullptr;
    }

private:
    friend class FieldDecodeContext;

    RecordScope(FieldDecodeContext* context, uint32 depth) noexcept
        : context_(context), depth_(depth) {
    }

    FieldDecodeContext* context_;
    uint32 depth_;
};

// 2단계 codec이 StructRefArray 원소 하나를 적용하는 동안 그 위치를 문맥에 세운다(RAII). 그 안의
// 참조 멤버 쓰기는 원소 위치로 범위 검사된다(FieldDecodeContext::StructElement). 이미 원소 위치가
// 있으면 Fatal이다(StructRefArray 원소는 Registry 컨테이너를 담지 않으므로 겹치지 않는다). 위치는
// 이 scope가 복사해 쥐며 scope는 문맥보다 먼저 끝나야 한다.
//
// 사용 예(배열 codec 2단계):
//   StructElementScope element(context, detail::StructRefArrayAccess::Location(array, index));
//   // 원소 struct 해석 — 참조 멤버는 SetElementReference로 간다
class StructElementScope {
public:
    [[nodiscard]] StructElementScope(FieldDecodeContext& context,
                                     const detail::StructElementLocation& location) noexcept
        : context_(context), location_(location) {
        if (context.element_ != nullptr) {
            platform::Fatal("StructElementScope nested inside another struct element");
        }

        context.element_ = &location_;
    }

    ~StructElementScope() {
        if (context_.element_ != &location_) {
            platform::Fatal("StructElementScope ended out of order");
        }

        context_.element_ = nullptr;
    }

    StructElementScope(const StructElementScope&) = delete;
    StructElementScope& operator=(const StructElementScope&) = delete;

private:
    FieldDecodeContext& context_;
    detail::StructElementLocation location_;
};

inline Result<RecordScope> FieldDecodeContext::OpenRecordBytes(Span<const uint8> record,
                                                               TypeId expected, uint32 minVersion,
                                                               uint32 maxVersion) noexcept {
    (void)record;
    (void)expected;
    (void)minVersion;
    (void)maxVersion;
    return Result<RecordScope>::Failure(ErrorCode::Unsupported);
}

inline Result<RecordScope> FieldDecodeContext::PushRecord(RecordReader&& reader) noexcept {
    if (depth_ == MaxRecordDepth) {
        return Result<RecordScope>::Failure(ErrorCode::CapacityExceeded);
    }

    slots_[depth_] = ::new (storage_ + depth_ * sizeof(RecordReader)) RecordReader(Move(reader));
    ++depth_;
    return Result<RecordScope>::Success(RecordScope(this, depth_));
}

inline Span<const PathStep>
FieldCodecContext::BuildPath(PathStep (&steps)[MaxPropertyPathSteps]) const noexcept {
    usize depth = 0;
    for (const FieldCodecFrame* frame = current_; frame; frame = frame->parent) {
        ++depth;
    }

    if (depth > MaxPropertyPathSteps) {
        platform::Fatal("FieldCodecContext path deeper than MaxPropertyPathSteps");
    }

    usize position = depth;
    for (const FieldCodecFrame* frame = current_; frame; frame = frame->parent) {
        --position;
        steps[position].fieldId = frame->field->id;
        // 원소·Map 항목·Set 원소는 삽입 순번 Index다(진단 경로 — key를 복사하지 않는다).
        const bool indexed = frame->index != FieldCodecFrame::NoIndex;
        steps[position].selector = indexed ? PathSelector::Index : PathSelector::None;
        steps[position].index = indexed ? frame->index : 0;
    }

    return Span<const PathStep>(steps, depth);
}

inline Result<void> FieldCodecContext::Report(FieldDiagnosticKind kind, int64 value,
                                              TypeId valueType) noexcept {
    if (!recording_ || !hooks_) {
        return Result<void>::Success();
    }

    PathStep steps[MaxPropertyPathSteps];
    FieldDiagnostic diagnostic;
    diagnostic.kind = kind;
    diagnostic.objectType = objectType_;
    diagnostic.objectIndex = objectIndex_;
    diagnostic.path = BuildPath(steps);
    diagnostic.field = current_ ? current_->field->id : Guid{};
    diagnostic.valueType = valueType;
    diagnostic.value = value;
    return hooks_->Report(diagnostic);
}

inline Result<void> FieldCodecContext::CollectSoftReference(AssetId asset, StringView objectPath,
                                                            TypeId expectedType) noexcept {
    if (!recording_ || !hooks_ || (asset.IsNull() && objectPath.ByteCount() == 0)) {
        return Result<void>::Success();
    }

    PathStep steps[MaxPropertyPathSteps];
    SoftReferenceInfo info;
    info.asset = asset;
    info.objectPath = objectPath;
    info.expectedType = expectedType;
    info.effectiveFlags = EffectiveFlags();
    info.objectIndex = objectIndex_;
    info.path = BuildPath(steps);
    return hooks_->CollectSoftReference(info);
}

} // namespace rocks
