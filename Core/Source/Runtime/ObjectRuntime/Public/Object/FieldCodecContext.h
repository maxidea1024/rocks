#pragma once

#include <Object/FieldCodecPolicy.h>

// 필드 codec 문맥 사슬의 둘째 조각으로 두 문맥의 공통 기반이다(Docs/Design/
// ReflectionFoundations.ko.md 6.1–6.4절). 위치 스택·진단·Soft 보고·예산·참조 wire를 가진
// FieldCodecContext와 위치 RAII(FieldScope, 배열 원소 index의 ElementIndexScope)가 있다.
// FieldCodecContext의 out-of-line inline 정의(BuildPath·Report·CollectSoftReference)는 사슬 끝에
// 있으므로 Object/FieldCodec.h로 받는다.
namespace rocks {

class FieldScope;
class ElementIndexScope;

// 두 문맥의 공통 기반이다: 위치(프레임) 스택, 진단·Soft 보고, 예산, 참조 wire. 직렬화기가
// 파생해 구현한다. 같은 문맥을 여러 스레드에서 쓰지 않는다.
class FieldCodecContext {
public:
    FieldCodecContext(const FieldCodecContext&) = delete;
    FieldCodecContext& operator=(const FieldCodecContext&) = delete;

    // 지금 위치다. FieldScope 밖에서 Field()를 부르면 Fatal이다.
    const FieldCodecFrame* Current() const noexcept {
        return current_;
    }

    const FieldSchema& Field() const noexcept {
        if (!current_) {
            platform::Fatal("FieldCodecContext::Field called outside a field scope");
        }

        return *current_->field;
    }

    uint32 EffectiveFlags() const noexcept {
        return current_ ? current_->effectiveFlags : 0;
    }

    // 지금 위치(필드·struct 멤버)가 포함 필터 includeFlags 밖인지다(리플렉션 B9h1 —
    // Docs/Design/ReflectionExtensions.ko.md 9.1절). includeFlags가 0이면 끔이다. 판정은 자기
    // 플래그(field.flags — 바깥에서 전파하지 않는다)로 하고, 조상 위치의 struct가 Atomic이면(통째
    // 단위 — Atomic struct 안의 멤버) 밖이 아니다. 위치 밖에서는 거짓이다.
    //
    // 사용 예(저장·복원 문맥의 ExcludesCurrent):
    //   return IsExcluded(EffectiveFlags()) || IsOutsideInclude(policy_.includeFlags);
    bool IsOutsideInclude(uint32 includeFlags) const noexcept {
        if (includeFlags == 0 || !current_) {
            return false;
        }

        for (const FieldCodecFrame* frame = current_->parent; frame; frame = frame->parent) {
            if (!frame->structure.IsEmpty() && frame->structure.IsAtomic()) {
                return false;
            }
        }

        return (current_->field->flags & includeFlags) == 0;
    }

    const EnumSchema* FieldEnum() const noexcept {
        return current_ ? current_->enumeration : nullptr;
    }

    // 지금 위치가 키가 Enum인 Map 필드면 그 키 enum 스키마다(리플렉션 B9f2 — 그 밖은 nullptr).
    const EnumSchema* FieldKeyEnum() const noexcept {
        return current_ ? current_->keyEnumeration : nullptr;
    }

    TypeId ObjectType() const noexcept {
        return objectType_;
    }

    uint32 ObjectIndex() const noexcept {
        return objectIndex_;
    }

    // 참조 필드(강한·weak)의 wire tag다(graph v2 = U32 — local ID와 외부 참조 비트, weak는 외부
    // 참조 비트를 쓰지 않는다). 참조 필드의 encode는 이 값을 돌려주고 decode는 저장 tag를 이것과
    // 비교한다.
    uint32 ReferenceWireType() const noexcept {
        return referenceWire_;
    }

    // 진단·수집을 기록하는 단계인지다(복원 2단계는 거짓 — 1단계가 이미 판정했다).
    bool IsRecording() const noexcept {
        return recording_;
    }

    // Name 필드 값(ValueKind::Name)의 토큰 pool이다(저장 encode가 토큰의 표시 문자열을 읽는다 —
    // 직렬화기가 대상 Registry의 RegistryOptions::names로 정한다, 없으면 nullptr). 복원 문맥은
    // pool에 닿지 않으므로(1단계는 표시 문자열을 기록하고 2a가 intern한다) 정하지 않는다.
    //
    // 사용 예(Name 값 encode):
    //   auto display = AcquireNameFieldText(context.Names(), value.name, context.Scratch());
    NamePool* Names() const noexcept {
        return names_;
    }

    // 진단을 받을 곳이 있는지다(기록하지 않는 단계는 참이다 — 1단계가 이미 판정했다). 값을 바꾸는
    // 정책(ReportAndDefault)은 이것이 거짓이면 InvalidArgument다(진단 없이 값을 바꾸지 않는다).
    bool CanReport() const noexcept {
        return !recording_ || hooks_ != nullptr;
    }

    // 지금 위치로 진단을 만든다. 기록하지 않는 단계(복원 2단계)에서는 아무것도 하지 않는다.
    Result<void> Report(FieldDiagnosticKind kind, int64 value = 0, TypeId valueType = {}) noexcept;

    // 스키마에 없는 필드를 보고한다. 최상위면 경로가 비어 있고, struct 안이면 경로는 감싼 struct
    // 필드까지다(같은 ID의 최상위 필드와 구별된다).
    Result<void> ReportUnknownField(Guid field) noexcept {
        if (!recording_ || !hooks_) {
            return Result<void>::Success();
        }

        PathStep steps[MaxPropertyPathSteps];
        FieldDiagnostic diagnostic;
        diagnostic.kind = FieldDiagnosticKind::UnknownField;
        diagnostic.objectType = objectType_;
        diagnostic.objectIndex = objectIndex_;
        diagnostic.path = BuildPath(steps);
        diagnostic.field = field;
        return hooks_->Report(diagnostic);
    }

    // 지금 위치의 non-null 소프트 참조를 보고한다(플래그·위치는 문맥이 채운다).
    Result<void> CollectSoftReference(AssetId asset, StringView objectPath,
                                      TypeId expectedType) noexcept;

    // 이 필드 처리의 작업량이다. 복원 1단계는 2단계 몫까지 두 배를 청구하고, 2단계는 청구하지
    // 않는다.
    virtual Result<void> Spend(uint64 work) noexcept = 0;

    // 복원 2단계에만 있는 작업량(staged 쓰기·문자열 복사·참조 연결)이다. 1단계가 미리 청구하고
    // 2단계와 저장은 청구하지 않는다.
    virtual Result<void> ReserveApply(uint64 work) noexcept = 0;

    // ClassRef 값(ValueKind::ClassReference — Docs/Design/ReflectionExtensions.ko.md 5.2절)의
    // 클래스 type을 대상 TypeRegistry에서 찾아 lease를 준다. base는 스키마의 Base 클래스
    // ID(value.type — 배열이면 element.type)이고, type이 base 자신이거나 그 파생(스키마 사슬
    // TypeLease::IsA)이어야 한다(base 0이면 Base ID를 보지 않는다 — 등록이 스키마의 0을 거부하므로
    // codec이 주는 base는 0이 아니다). 직렬화기가 구현한다: 복원 1단계 문맥은 TypeRegistry만
    // 조회하고(ObjectRegistry에 닿지 않는다 — 잠금으로 보호되는 TypeRegistry::Acquire라
    // worker에서도 부를 수 있다), 저장 문맥은 ObjectRegistry::AcquireRegisteredType(소유
    // 스레드)이다. 복원 2단계는 조회하지 않는다(1단계 판정 기록 — TakeClassReference). 판정 규칙은
    // detail::CheckClassReferenceLease(Object/ClassRef.h) 하나다.
    //
    // 반환: 그 클래스의 lease. 실패 — 미등록 NotFound, 클래스가 아닌 레코드(struct·enum)·base의
    // 파생이 아님 InvalidArgument, 종료 ShuttingDown, 문맥·스레드 오류는 그 오류. 기본 문맥은
    // Unsupported다(ClassRef를 지원하지 않는 문맥 — 시험 문맥 등).
    //
    // 사용 예(codec이 부르는 형태):
    //   auto found = context.AcquireClassReference(value.guid, type.type);
    //   if (!found) { return Result<void>::Failure(found.Error()); }
    virtual Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept {
        (void)type;
        (void)base;
        return Result<TypeLease>::Failure(ErrorCode::Unsupported);
    }

    // 값 임시 저장에 쓰는 할당기다(저장·1단계는 예산을 청구한다).
    virtual Allocator& Scratch() noexcept = 0;

    // 관찰 대상 struct 임시 값과 그 안 배열의 원소 버퍼를 할당하는 할당기다(복원 1단계의 struct
    // hook 관찰 — Docs/Design/ReflectionFoundations.ko.md 5.6절). 바이트를 작업량으로 청구하되
    // Scratch와 달리 단일 할당 한도(maxValueBytes)가 없다: 관찰 struct 안 배열의 원소 버퍼는 wire
    // 값 크기보다 클 수 있다(예: 빈 String 원소 20만 개 — wire 8 byte, native 수십 byte). work
    // 예산이 상한이다. 복원 1단계의 Map 키·Set 원소 중복 판정 색인(리플렉션 B9f2 — 1단계에만 있는
    // 임시 색인이고 wire보다 클 수 있다)도 이 할당기를 쓴다. 복원 2단계와 저장은 관찰하지 않으므로
    // 쓰지 않는다.
    virtual Allocator& ObservationScratch() noexcept = 0;

    // 중첩 Record(struct 값)의 TLV 수를 그래프 필드 누계(maxTotalFields)에 더한다. 복원 2단계는
    // 세지 않는다.
    virtual Result<void> AddFields(usize count) noexcept = 0;

    // 배열 원소 수를 그래프 원소 누계(maxTotalElements)에 더한다(넘으면 CapacityExceeded). 배열
    // codec이 header를 해석한 직후·원소를 읽거나 할당하기 전에 부른다(저장은 원소를 쓰기 전).
    // 복원 2단계는 세지 않는다.
    virtual Result<void> AddElements(usize count) noexcept = 0;

protected:
    FieldCodecContext(FieldCodecHooks* hooks, uint32 referenceWire) noexcept
        : hooks_(hooks), referenceWire_(referenceWire) {
    }

    ~FieldCodecContext() = default;

    // 직렬화기가 객체마다 정한다.
    void SetObject(TypeId type, uint32 index) noexcept {
        objectType_ = type;
        objectIndex_ = index;
    }

    // 복원 2단계는 false다(진단·수집을 버린다).
    void SetRecording(bool recording) noexcept {
        recording_ = recording;
    }

    // 저장 문맥이 Names()를 정한다(그 Registry의 RegistryOptions::names).
    void SetNames(NamePool* names) noexcept {
        names_ = names;
    }

    bool Recording() const noexcept {
        return recording_;
    }

    FieldCodecHooks* Hooks() const noexcept {
        return hooks_;
    }

    // 프레임 사슬로 경로를 만든다(바깥 → 안 순서). 반환 span은 steps를 가리킨다.
    Span<const PathStep> BuildPath(PathStep (&steps)[MaxPropertyPathSteps]) const noexcept;

private:
    friend class FieldScope;
    friend class ElementIndexScope;

    FieldCodecHooks* hooks_;
    NamePool* names_ = nullptr;
    // 맨 위 프레임이다. ElementIndexScope가 그 index 칸을 쓰므로 non-const다(Current()는 const를
    // 돌려준다).
    FieldCodecFrame* current_ = nullptr;
    TypeId objectType_{};
    uint32 objectIndex_ = 0;
    uint32 referenceWire_;
    bool recording_ = true;
};

// 위치 하나에 들어가고 나온다(RAII). 바깥 프레임의 유효 플래그를 PropagateFieldFlags로 전파한다.
// keyEnumeration은 키가 Enum인 Map 필드의 키 enum이다(리플렉션 B9f2 — TypeLease·StructView의
// FieldKeyEnum). scope는 문맥보다 먼저 끝나야 하고 들어간 순서의 역순으로 끝나야 한다.
//
// 사용 예(직렬화기):
//   FieldScope scope(context, schema.fields[index], schema.id, lease.FieldEnum(index),
//                    lease.FieldStruct(index), FieldCodecFrame::NoIndex,
//                    lease.FieldKeyEnum(index));
//   auto wire = ops->encode(*ops, registry, access, handle, payload, context);
class FieldScope {
public:
    [[nodiscard]] FieldScope(FieldCodecContext& context, const FieldSchema& field,
                             TypeId declaringType, const EnumSchema* enumeration = nullptr,
                             StructView structure = {}, uint32 index = FieldCodecFrame::NoIndex,
                             const EnumSchema* keyEnumeration = nullptr) noexcept
        : context_(context), previous_(context.current_) {
        frame_.field = &field;
        frame_.declaringType = declaringType;
        frame_.enumeration = enumeration;
        frame_.structure = structure;
        frame_.index = index;
        frame_.keyEnumeration = keyEnumeration;
        frame_.parent = context.current_;
        frame_.effectiveFlags =
            context.current_ ? PropagateFieldFlags(context.current_->effectiveFlags, field.flags)
                             : field.flags;
        context.current_ = &frame_;
    }

    ~FieldScope() {
        // scope는 들어간 순서의 역순으로 끝나야 한다.
        if (context_.current_ != &frame_) {
            platform::Fatal("FieldScope ended out of order");
        }

        // parent는 const 사슬이므로 바깥 프레임(non-const)은 따로 보관한 것으로 되돌린다.
        context_.current_ = previous_;
    }

    FieldScope(const FieldScope&) = delete;
    FieldScope& operator=(const FieldScope&) = delete;

private:
    FieldCodecContext& context_;
    FieldCodecFrame* previous_;
    FieldCodecFrame frame_;
};

// 배열 필드 프레임의 원소 index 칸을 쓴다(RAII, Docs/Design/ReflectionFoundations.ko.md 5.6절).
// 원소마다 새 프레임을 만들지 않으므로 프레임은 최대 MaxPropertyPathSteps(17)개(BuildPath 버퍼
// 크기)다. 만들 때 현재 프레임이 원소 열을 가진 컨테이너 필드(Array·Map·Set — 리플렉션 B9f2가
// Map 항목·Set 원소로 넓혔다, 진단 경로는 키를 복사하지 않는 삽입 순번이다 —
// Docs/Design/ReflectionExtensions.ko.md 1.6절)이고 그 index가 NoIndex여야 한다(아니면 Fatal —
// 컨테이너 codec 계약 위반). Set은 현재 프레임이 그 컨테이너 프레임일 때만 쓸 수 있고(원소 struct
// 멤버 scope 안에서 부르면 Fatal), 소멸 때 index를 NoIndex로 되돌린다. 컨테이너 프레임을 연
// FieldScope보다 먼저 끝나야 한다.
//
// 사용 예(배열 codec):
//   ElementIndexScope element(context);
//   for (usize index = 0; index < count; ++index) {
//       element.Set(static_cast<uint32>(index));   // 진단·hook 경로가 [f[index]]가 된다
//       ...
//   }
class ElementIndexScope {
public:
    [[nodiscard]] explicit ElementIndexScope(FieldCodecContext& context) noexcept
        : context_(context), frame_(context.current_) {
        const ValueKind kind = frame_ ? frame_->field->value.kind : ValueKind::Opaque;
        if ((kind != ValueKind::Array && kind != ValueKind::Map && kind != ValueKind::Set &&
             kind != ValueKind::StaticArray) ||
            frame_->index != FieldCodecFrame::NoIndex) {
            platform::Fatal(
                "ElementIndexScope requires an array, static array, map or set field frame "
                "without an index");
        }
    }

    ~ElementIndexScope() {
        frame_->index = FieldCodecFrame::NoIndex;
    }

    ElementIndexScope(const ElementIndexScope&) = delete;
    ElementIndexScope& operator=(const ElementIndexScope&) = delete;

    // 지금 처리하는 원소 index를 프레임에 쓴다.
    void Set(uint32 index) noexcept {
        if (context_.current_ != frame_) {
            platform::Fatal("ElementIndexScope::Set called outside its array frame");
        }

        frame_->index = index;
    }

private:
    FieldCodecContext& context_;
    FieldCodecFrame* frame_;
};

} // namespace rocks
