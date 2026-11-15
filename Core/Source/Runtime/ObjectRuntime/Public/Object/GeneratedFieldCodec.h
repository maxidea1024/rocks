#pragma once

#include <Object/ArrayFieldCodec.h>

// 필드 codec이다(Docs/Design/ReflectionFoundations.ko.md 4.6·5.6·6.4절). 생성 헤더와 직렬화기는
// 이 헤더 하나만 include한다. codec은 앞 조각을 include하는 사슬로 나뉜다: 단일 값 wire 쓰기
// (Object/FieldValueWire.h — WriteValueWire·EncodeFieldWire), 읽기·보고와 공용 판정
// (Object/FieldValueParse.h — ReadValueWire·ReportParsedValue·ParseFieldValue), struct 값
// (Object/StructFieldCodec.h — EncodeStructRecord·DecodeStructValue), 배열
// (Object/ArrayFieldCodec.h — EncodeArrayField·DecodeArrayField). 이 헤더에는 멤버 값 codec
// (EncodeMemberValue·EncodeStructMember·DecodeStructMember), 생성 thunk(EncodeGeneratedField·
// DecodeGeneratedField·StructFieldOps)와 수동 함수표용 범용 codec(EncodeThroughFieldValue·
// DecodeThroughFieldValue)이 있다.
namespace rocks::detail {

// 지금 위치(context.Field())의 멤버 값 source를 out에 쓰고 wire tag를 돌려준다(클래스 필드·struct
// 멤버 공용 — 호출자가 MatchesSchema를 확인했다). 배열은 컨테이너를 제자리로(ValueArray·
// RecordList), struct는 등록 struct(지금 위치의 structure — MatchesStructView가 아니면
// TypeMismatch)로 중첩 Record를, 단일 값은 EncodeFieldWire로 쓴다. String·경로·배열 원소는 복사하지
// 않는다(ArchiveWriter가 성장 전에 크기 한도를 검사하고, owner 밖 버퍼는 encode 고정 또는 Required
// 검사 중 표시가 지킨다).
template <class Member>
Result<uint32> EncodeMemberValue(const Member& source, ArchiveWriter& out,
                                 FieldEncodeContext& context) noexcept {
    if constexpr (ArrayField<Member>::Supported) {
        return EncodeArrayField(source, out, context);
    } else if constexpr (ReflectedStructField<Member>::Supported) {
        if (!MatchesStructView<Member>(context)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }

        return EncodeStructRecord(context.Current()->structure, &source, out, context);
    } else {
        FieldValue value(context.Scratch());
        StringView text;
        value.kind = FieldValueKind<Member>();
        if constexpr (IsSameV<Member, String>) {
            text = source.View();
        } else if constexpr (SoftReferenceField<Member>::Supported) {
            value.guid = source.asset.value;
            text = source.objectPath.View();
        } else {
            if (auto copied = CopyFieldValue(source, value); !copied) {
                return Result<uint32>::Failure(copied.Error());
            }
        }

        return EncodeFieldWire(out, context, value, text, StaticReferenceNative<Member>());
    }
}

// 생성(또는 수동) struct 멤버 encode thunk다. 멤버를 복사하지 않고 직접 쓴다.
template <auto MemberPointer>
Result<uint32> EncodeStructMember(const RuntimeStructFieldOps&, const void* structValue,
                                  ArchiveWriter& out, FieldEncodeContext& context) noexcept {
    using Traits = StructMemberPointer<decltype(MemberPointer)>;
    using S = typename Traits::Owner;
    using Member = typename Traits::Member;
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    return EncodeMemberValue(static_cast<const S*>(structValue)->*MemberPointer, out, context);
}

// 생성(또는 수동) struct 멤버 decode thunk다. 검증 방식(structValue null)은 값에 손대지 않고, 관찰·
// 적용 방식은 struct 값에 직접 쓴다(참조 멤버의 적용은 SetStructMemberReference, 배열 멤버는
// DecodeArrayField).
template <auto MemberPointer>
Result<void> DecodeStructMember(const RuntimeStructFieldOps&, ConstructionContext* construction,
                                ObjectHandle owner, void* structValue, const FieldView& field,
                                FieldDecodeContext& context) noexcept {
    using Traits = StructMemberPointer<decltype(MemberPointer)>;
    using S = typename Traits::Owner;
    using Member = typename Traits::Member;
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    S* target = static_cast<S*>(structValue);
    if constexpr (ArrayField<Member>::Supported) {
        return DecodeArrayField(construction, owner, target ? &(target->*MemberPointer) : nullptr,
                                field, context);
    } else if constexpr (ReflectedStructField<Member>::Supported) {
        if (!MatchesStructView<Member>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        return DecodeStructField(context.Current()->structure, construction, owner,
                                 target ? &(target->*MemberPointer) : nullptr, field, context);
    } else if constexpr (ObjectReferenceField<Member>::Supported) {
        // 관찰 대상 struct는 참조를 담지 않으므로 construction이 없으면 검증만 한다.
        if (!construction || !target) {
            return ParseFieldValue(field, context, StaticReferenceNative<Member>(), nullptr);
        }

        FieldValue value(context.Scratch());
        if (auto parsed = ParseFieldValue(field, context, StaticReferenceNative<Member>(), &value);
            !parsed) {
            return parsed;
        }

        return SetStructMemberReference(*construction, context, owner, target->*MemberPointer,
                                        value.reference);
    } else {
        // Name 멤버는 1단계에서 토큰이 없고 weak 멤버는 대상 handle이 없으므로 관찰할 수 없다(관찰
        // struct는 Name·weak 참조를 담지 않는다 — 직렬화기가 ObservedStructs에서 거부한다,
        // Docs/Design/ReflectionExtensions.ko.md 4.1·5.1절).
        if constexpr (IsSameV<Member, Name> || WeakReferenceField<Member>::Supported) {
            if (!construction && target) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        // 검증 방식은 값을 채우지 않는다(문자열을 scratch에 복사하지 않는다). enum은 정적 enum
        // 확인에 값이 필요하다. 관찰 방식(construction 없음·target 있음)의 문자열 사본은 1단계에만
        // 있는 일이라 ObservationScratch로 한 번만 청구한다(Scratch는 2단계 몫까지 두 번 센다).
        FieldValue value(!construction && target ? context.ObservationScratch()
                                                 : context.Scratch());
        FieldValue* apply = target || ReflectedEnumField<Member>::Supported ? &value : nullptr;
        if (auto parsed = ParseFieldValue(field, context, StaticReferenceNative<Member>(), apply);
            !parsed) {
            return parsed;
        }

        // 1단계는 등록 enum 스키마로 판정하므로 정적 enum(직접 쓰기의 기준)으로도 확인한다.
        if constexpr (ReflectedEnumField<Member>::Supported) {
            if (!GeneratedType<Member>::IsDeclared(value.signedInteger)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (!target) {
            return Result<void>::Success();
        }

        return WriteGeneratedValue<S>(MemberPointer, value,
                                      [target]() noexcept { return Result<S*>::Success(target); });
    }
}

// struct 멤버 하나의 함수표다(생성 코드·수동 시험 공용). valueNativeType은 Enum·Struct 멤버와
// Enum·Struct 원소 배열 멤버의 원소 native 키다. path는 경로 접근 thunk(B6 — 생성 코드는
// &detail::PathStructMember<&S::m, ValueKind::Target>, Object/GeneratedPathAccess.h)이며 주지
// 않으면 이 멤버로 들어가는 경로 연산은 Unsupported다.
//
// 사용 예:
//   const RuntimeStructFieldOps fields[] = {detail::StructFieldOps<&BulkDataRef::bulkId>(
//       BulkId, &detail::PathStructMember<&BulkDataRef::bulkId, ValueKind::UInt32>)};
template <auto MemberPointer>
RuntimeStructFieldOps StructFieldOps(Guid fieldId,
                                     StructFieldPathFunction path = nullptr) noexcept {
    using Member = typename StructMemberPointer<decltype(MemberPointer)>::Member;
    RuntimeStructFieldOps ops;
    ops.fieldId = fieldId;
    ops.encode = &EncodeStructMember<MemberPointer>;
    ops.decode = &DecodeStructMember<MemberPointer>;
    ops.path = path;
    if constexpr (ArrayField<Member>::Supported) {
        using Element = typename ArrayField<Member>::Element;
        if constexpr (ReflectedEnumField<Element>::Supported ||
                      ReflectedStructField<Element>::Supported) {
            ops.valueNativeType = NativeTypeKey<Element>();
        }
    } else if constexpr (ReflectedEnumField<Member>::Supported ||
                         ReflectedStructField<Member>::Supported) {
        ops.valueNativeType = NativeTypeKey<Member>();
    }

    return ops;
}

// 생성 encode thunk다. owner를 Borrow하고 멤버를 직접 읽어 쓴다(String·경로·배열 원소를 복사하지
// 않는다 — ArchiveWriter가 성장 전에 크기 한도를 검사하고, owner 밖 버퍼는 호출자가 쥔 encode
// 고정이 지킨다). 쓰기 뒤 owner의 논리 수명을 다시 확인한다. 문맥이 staged 객체를 읽는 Required
// 검사 모드면(FieldEncodeContext::StagedSource) Registry Resolve 대신 그 트랜잭션의
// ConstructionContext::Get으로 owner를 얻는다(Borrow·encode 고정 없음 — 검사 중 표시가 그
// 트랜잭션의 쓰기를 막고 Commit 전이라 회수되지 않는다).
template <class Owner, class MemberOwner, class Member>
Result<uint32> EncodeGeneratedField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    ObjectHandle handle, Member MemberOwner::* member,
                                    ArchiveWriter& out, FieldEncodeContext& context) noexcept {
    static_assert(__is_base_of(MemberOwner, Owner));
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    if (ConstructionContext* staged = context.StagedSource()) {
        auto owner = staged->Get<Owner>(handle);
        if (!owner) {
            return Result<uint32>::Failure(owner.Error());
        }

        const Owner& source = *owner.Value();
        return EncodeMemberValue(source.*member, out, context);
    }

    auto borrowed = registry.Resolve<Owner>(access, handle);
    if (!borrowed) {
        return Result<uint32>::Failure(borrowed.Error());
    }

    auto target = borrowed.Value().Get();
    if (!target) {
        return Result<uint32>::Failure(target.Error());
    }

    auto wire = EncodeMemberValue(target.Value()->*member, out, context);
    if (!wire) {
        return wire;
    }

    auto stillValid = borrowed.Value().Get();
    if (!stillValid) {
        return Result<uint32>::Failure(stillValid.Error());
    }

    return wire;
}

// 생성 decode thunk다. 1단계(construction == nullptr)는 해석·검증만, 2단계는 같은 해석 뒤 staged
// 쓰기로 게시한다(검사는 1단계 검증의 부분집합이다).
template <class Owner, class MemberOwner, class Member>
Result<void> DecodeGeneratedField(ConstructionContext* construction, ObjectHandle handle,
                                  Member MemberOwner::* member, const FieldView& field,
                                  FieldDecodeContext& context) noexcept {
    static_assert(__is_base_of(MemberOwner, Owner));
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ArrayField<Member>::Supported) {
        // 배열 필드: 1단계는 원소 없이 검증, 2단계는 새 객체 안의 컨테이너에 쓴다.
        if (!construction) {
            return DecodeArrayField<Member>(nullptr, handle, nullptr, field, context);
        }

        auto owner = construction->GetForWrite<Owner>(handle);
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        return DecodeArrayField(construction, handle, &(owner.Value()->*member), field, context);
    } else if constexpr (ReflectedStructField<Member>::Supported) {
        // struct 필드: 1단계는 값 없이 검증(관찰은 scratch), 2단계는 새 객체 안의 멤버 주소에 쓴다.
        if (!MatchesStructView<Member>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!construction) {
            return DecodeStructField(context.Current()->structure, nullptr, handle, nullptr, field,
                                     context);
        }

        auto owner = construction->GetForWrite<Owner>(handle);
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        return DecodeStructField(context.Current()->structure, construction, handle,
                                 &(owner.Value()->*member), field, context);
    } else if constexpr (ReflectedEnumField<Member>::Supported) {
        // 1단계는 등록된 enum 스키마로 판정하므로, 정적 enum(staged 쓰기의 기준)으로도 확인해
        // 2단계가 입력 때문에 실패하지 않게 한다(enum 값은 할당 없이 해석된다).
        FieldValue value(context.Scratch());
        auto parsed = ParseFieldValue(field, context, nullptr, &value);
        if (!parsed) {
            return parsed;
        }

        if (!GeneratedType<Member>::IsDeclared(value.signedInteger)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!construction) {
            return Result<void>::Success();
        }

        return WriteGeneratedStagedField<Owner>(*construction, handle, member, value);
    } else if constexpr (IsSameV<Member, Name>) {
        // 2단계 토큰은 2a가 이 Registry의 pool로 intern한 것이고 내용은 1단계가 검사했으므로 staged
        // 쓰기의 토큰 검사(pool·domain·내용)를 다시 하지 않는다 — 2단계는 입력 때문에 실패하지
        // 않는다.
        if (!construction) {
            return ParseFieldValue(field, context, nullptr, nullptr);
        }

        FieldValue value(context.Scratch());
        if (auto parsed = ParseFieldValue(field, context, nullptr, &value); !parsed) {
            return parsed;
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction->GetForWrite<Owner>(handle); });
    } else if constexpr (ClassReferenceField<Member>::Supported) {
        // ClassRef 값은 1단계가 TypeRegistry 조회로 판정하고 기록했으므로(2단계는 그 기록을 쓴다)
        // staged 쓰기의 등록 검사(ObjectRegistry 조회)를 다시 하지 않는다 — 2단계는 입력 때문에
        // 실패하지 않는다.
        if (!construction) {
            return ParseFieldValue(field, context, StaticReferenceNative<Member>(), nullptr);
        }

        FieldValue value(context.Scratch());
        if (auto parsed = ParseFieldValue(field, context, StaticReferenceNative<Member>(), &value);
            !parsed) {
            return parsed;
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction->GetForWrite<Owner>(handle); });
    } else if constexpr (WeakReferenceField<Member>::Supported) {
        // weak 대상은 1단계가 범위(같은 그래프 안 노드)·기대 형식을 검증한 노드 handle(2a가 이
        // 트랜잭션에 만든 객체)이므로 staged 쓰기의 대상 검사를 다시 하지 않는다 — 2단계는 입력
        // 때문에 실패하지 않는다. weak는 GC 간선이 아니므로 SetReference를 거치지 않고 대입한다.
        if (!construction) {
            return ParseFieldValue(field, context, StaticReferenceNative<Member>(), nullptr);
        }

        FieldValue value(context.Scratch());
        if (auto parsed = ParseFieldValue(field, context, StaticReferenceNative<Member>(), &value);
            !parsed) {
            return parsed;
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction->GetForWrite<Owner>(handle); });
    } else {
        if (!construction) {
            return ParseFieldValue(field, context, StaticReferenceNative<Member>(), nullptr);
        }

        FieldValue value(context.Scratch());
        auto parsed = ParseFieldValue(field, context, StaticReferenceNative<Member>(), &value);
        if (!parsed) {
            return parsed;
        }

        return WriteGeneratedStagedField<Owner>(*construction, handle, member, value);
    }
}

// 수동 함수표·실행 중 정의되는 필드용 범용 encode다(같은 표의 read로 값을 얻는다 — staged 객체를
// 읽는 Required 검사 모드에서는 readStaged, 없으면 Unsupported). 강한 참조의 정적 대상 타입을
// 모르므로 스키마 기대 타입만 검사한다. 단일 값 필드만 지원한다(struct·배열은 Unsupported).
//
// 사용 예:
//   RuntimeFieldOps ops{id, read, write, writeStaged, &detail::EncodeThroughFieldValue,
//                       &detail::DecodeThroughFieldValue};
//   ops.readStaged = readStaged;   // Required 필드면(그래프 로드의 Required 실효 값 검사)
inline Result<uint32> EncodeThroughFieldValue(const RuntimeFieldOps& self, ObjectRegistry& registry,
                                              const ObjectAccessContext& access,
                                              ObjectHandle handle, ArchiveWriter& out,
                                              FieldEncodeContext& context) noexcept {
    FieldValue value(context.Scratch());
    if (ConstructionContext* staged = context.StagedSource()) {
        if (!self.readStaged) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        if (auto read = self.readStaged(*staged, handle, value); !read) {
            return Result<uint32>::Failure(read.Error());
        }
    } else {
        if (!self.read) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        if (auto read = self.read(registry, access, handle, value); !read) {
            return Result<uint32>::Failure(read.Error());
        }
    }

    return EncodeFieldWire(out, context, value, value.string.View(), nullptr);
}

// 수동 함수표·실행 중 정의되는 필드용 범용 decode다(2단계는 같은 표의 writeStaged로 게시한다). 정적
// 대상 타입을 모르므로, writeStaged의 검사가 스키마보다 엄격하면(예: 스키마 기대 타입 Object인데
// 멤버가 ObjectRef<Derived>) 2단계에서 실패할 수 있다 — 그런 함수표는 계약 위반이며 rollback된다.
inline Result<void> DecodeThroughFieldValue(const RuntimeFieldOps& self,
                                            ConstructionContext* construction, ObjectHandle handle,
                                            const FieldView& field,
                                            FieldDecodeContext& context) noexcept {
    if (!construction) {
        return ParseFieldValue(field, context, nullptr, nullptr);
    }

    if (!self.writeStaged) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    FieldValue value(context.Scratch());
    if (auto parsed = ParseFieldValue(field, context, nullptr, &value); !parsed) {
        return parsed;
    }

    return self.writeStaged(*construction, handle, value);
}

} // namespace rocks::detail
