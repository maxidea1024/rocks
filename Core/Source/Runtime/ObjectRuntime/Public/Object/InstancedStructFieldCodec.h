#pragma once

#include <Object/StructFieldCodec.h>
#include <Serialization/InstancedStructWire.h>

// 필드 codec 사슬의 넷째 조각으로 InstancedStruct 값 codec이다(리플렉션 B9j —
// Docs/Design/ReflectionExtensions.ko.md 1.2·11절). 값 하나의 쓰기(EncodeInstancedStructContent —
// 필드 자리 EncodeInstancedStructField, 원소·Map 값 자리 EncodeInstancedStructElement)와 읽기
// (DecodeInstancedStructContent — 필드 자리 DecodeInstancedStructField, 원소·Map 값 자리
// DecodeInstancedStructElement)가 있다. wire는 InstancedStruct(17)다: 필드 값은 빈 payload(비어
// 있음) 또는 담긴 struct의 Record 하나(schema = struct ID, version = 그 struct 버전), 원소·Map 값
// 자리는 u64 길이 + 같은 내용이다. 담긴 struct의 Record는 struct 필드와 같은 규칙(제외 멤버·버전
// 마이그레이션·newerVersion·그래프 필드 누계)으로 쓰고 읽는다. 배열·정적 배열·Map codec이 원소
// 자리를 쓰므로 그 앞에 둔다. Object/GeneratedFieldCodec.h로 받는다.
namespace rocks::detail {

// 지금 위치(대상 형식이 InstancedStruct인 필드·원소·Map 값)의 값 source의 내용(비어 있으면 아무것도
// 쓰지 않는다, 아니면 담긴 struct Record)을 out에 쓴다. 담긴 struct는 놓인 자리의 깊이 합 규칙
// (CheckInstancedStructDepth — 위치 깊이 PositionDepth + 담긴 struct 깊이 ≤ MaxStructDepth)과 담기
// 조건(CheckInstancedStructContent)을 다시 본다 — C++가 Emplace로 깊은 자리에 직접 넣은 값은 여기서
// Fatal 없이 InvalidArgument다. Record는 프레임 structure를 담긴 struct로 채운 동안
// EncodeStructRecord로 쓴다. 실패하면 out의 내용은 정해지지 않는다.
template <class Base>
Result<void> EncodeInstancedStructContent(const InstancedStruct<Base>& source, ArchiveWriter& out,
                                          FieldEncodeContext& context) noexcept {
    if (source.IsEmpty()) {
        return Result<void>::Success();
    }

    const StructView view = source.View();
    if (auto allowed = CheckInstancedStructContent(view); !allowed) {
        return allowed;
    }

    // InstancedStruct<Base>(리플렉션 B9k)의 담긴 struct는 스키마 Base의 파생이어야 한다(수동
    // 함수표·lease 판 Emplace로 넣은 값도 저장 때 다시 본다).
    const TypeId base = FieldTargetType(context.Field()).type;
    if (base != TypeId{} && !source.Type().IsA(base)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto fits = CheckInstancedStructDepth(context.PositionDepth(), view); !fits) {
        return fits;
    }

    InstancedContentScope content(context, view);
    auto wire = EncodeStructRecord(view, InstancedStructAccess::Value(source), out, context);
    if (!wire) {
        return Result<void>::Failure(wire.Error());
    }

    if (wire.Value() != static_cast<uint32>(WireType::Record)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<void>::Success();
}

// 지금 위치(InstancedStruct 필드·멤버)의 값 source를 out에 쓰고 wire InstancedStruct를 돌려준다
// (호출자가 MatchesSchema를 확인했다). 내용 규칙은 EncodeInstancedStructContent다.
template <class Base>
Result<uint32> EncodeInstancedStructField(const InstancedStruct<Base>& source, ArchiveWriter& out,
                                          FieldEncodeContext& context) noexcept {
    if (auto written = EncodeInstancedStructContent(source, out, context); !written) {
        return Result<uint32>::Failure(written.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(WireType::InstancedStruct));
}

// 원소·Map 값 자리의 값 source를 u64 길이 + 내용으로 out에 쓴다(PrefixedRecordWriter — 비어 있으면
// 길이 0). 배열·정적 배열·Map codec이 원소 하나마다 부른다(ElementIndexScope 안 — 진단 경로가
// [f[i]]다).
template <class Base>
Result<void> EncodeInstancedStructElement(const InstancedStruct<Base>& source, ArchiveWriter& out,
                                          FieldEncodeContext& context) noexcept {
    auto prefixed = PrefixedRecordWriter::Begin(out, WireType::InstancedStruct);
    if (!prefixed) {
        return Result<void>::Failure(prefixed.Error());
    }

    if (auto written = EncodeInstancedStructContent(source, out, context); !written) {
        return written;
    }

    return prefixed.Value().End();
}

// 지금 위치의 저장 InstancedStruct 내용 하나(content — 필드 payload 또는 원소 자리에서 읽은 범위)를
// 해석한다. target·construction이 방식을 정한다: 검증(target null — 1단계, 값에 손대지 않는다),
// 적용(둘 다 있음 — 2단계, owner 객체 안의 자리). 관찰 자리(construction null·target 있음)는 오지
// 않는다(관찰 struct는 InstancedStruct를 담지 않는다 — 직렬화기가 거부한다, InvalidArgument).
//
// 1단계 순서: 2단계 몫 1 청구 → 비어 있으면 끝 → AcquireInstancedStructType(담긴 struct ID — 미등록
// NotFound면 decode 정책 unknownStructType: Reject는 NotFound, ReportAndClear는 진단 받을 곳이
// 있어야 하고(없으면 InvalidArgument) UnknownStructType 진단(valueType = 저장 struct ID) 뒤
// 비움으로 기록, struct가 아닌 레코드는 InvalidArgument, 그 밖 오류 그대로) → 담기 조건·깊이
// 합(위치 깊이 + 담긴 struct 깊이 — InvalidArgument) → 판정 기록(CollectInstancedStruct) → 프레임
// structure를 담긴 struct로 채우고 DecodeStructValue(Record 내용 출처 — 버전
// 판정·마이그레이션·newerVersion·관찰 규칙이 struct 필드와 같다).
//
// 2단계는 TakeInstancedStruct로 1단계 판정을 받아(TypeRegistry를 다시 조회하지 않는다) 빈 lease면
// 자리를 비우고, 아니면 자리가 같은 struct를 담고 있으면 그 값 위에(Record에 없는 멤버는 지금 값),
// 다르면 그 struct 기본 값(객체 allocator)으로 바꾼 뒤 그 위에 해석한다. 2단계가 실패하는 것은
// 할당(값 블록·멤버 String 복사)뿐이다.
template <class Base>
Result<void> DecodeInstancedStructContent(ConstructionContext* construction, ObjectHandle owner,
                                          InstancedStruct<Base>* target,
                                          const InstancedStructWire& content,
                                          FieldDecodeContext& context) noexcept {
    if (target && !construction) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = context.ReserveApply(1); !reserved) {
        return reserved;
    }

    if (content.IsEmpty()) {
        if (target) {
            // 옛 값은 이 함수를 나갈 때 소멸한다(비운 상태를 먼저 게시한다).
            InstancedStruct<Base> previous(Move(*target));
        }

        return Result<void>::Success();
    }

    if (!construction) {
        auto acquired = context.AcquireInstancedStructType(content.schema);
        if (!acquired) {
            if (acquired.Error() != ErrorCode::NotFound) {
                // 저장된 schema가 담을 수 없는 struct(InvalidArgument)나 struct가 아님
                // (TypeMismatch)이면 손상이다(AssetSystem 19장).
                const ErrorCode error = acquired.Error();
                return Result<void>::Failure(error == ErrorCode::TypeMismatch ||
                                                     error == ErrorCode::InvalidArgument
                                                 ? ErrorCode::CorruptData
                                                 : error);
            }

            if (context.Policy().unknownStructType == UnknownStructTypePolicy::Reject) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }

            // 진단 없이 값을 비우지 않는다(직렬화기가 hook 없이 ReportAndClear를 주면 거부).
            if (!context.CanReport()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (auto reported =
                    context.Report(FieldDiagnosticKind::UnknownStructType, 0, content.schema);
                !reported) {
                return reported;
            }

            return context.CollectInstancedStruct(content.schema, TypeLease{});
        }

        const StructView view = acquired.Value().Struct();
        // 저장된 struct를 담을 수 없으면 손상이다(같은 검사가 Emplace에서는 InvalidArgument).
        if (auto allowed = CheckInstancedStructContent(view); !allowed) {
            return Result<void>::Failure(allowed.Error() == ErrorCode::InvalidArgument
                                             ? ErrorCode::CorruptData
                                             : allowed.Error());
        }

        // InstancedStruct<Base>(리플렉션 B9k)는 Base 자신이나 그 파생만 받는다(정책과 무관하게
        // CorruptData — 읽은 데이터의 규칙 위반).
        const TypeId base = FieldTargetType(context.Field()).type;
        if (base != TypeId{} && !acquired.Value().IsA(base)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        if (auto fits = CheckInstancedStructDepth(context.PositionDepth(), view); !fits) {
            return fits;
        }

        // 기록이 lease를 넘겨받아도 레코드는 그대로이므로(lease가 그 레코드를 붙잡는다) view는 이
        // 함수 동안 유효하다.
        if (auto collected = context.CollectInstancedStruct(content.schema, Move(acquired.Value()));
            !collected) {
            return collected;
        }

        InstancedContentScope scope(context, view);
        StructRecordSource source;
        source.record = content.record;
        return DecodeStructValue(view, nullptr, owner, nullptr, source, context);
    }

    auto taken = context.TakeInstancedStruct(content.schema);
    if (!taken) {
        return Result<void>::Failure(taken.Error());
    }

    // 2단계 계약: 1단계가 판정한 대로 비우거나 그 struct 값 위에 적용한다.
    if (!taken.Value().IsValid()) {
        if (target) {
            InstancedStruct<Base> previous(Move(*target));
        }

        return Result<void>::Success();
    }

    if (!target) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (target->IsEmpty() || target->Type().Id() != taken.Value().Id()) {
        if (auto made = target->Emplace(Move(taken.Value()), construction->ObjectAllocator());
            !made) {
            return made;
        }
    }

    const StructView view = target->View();
    InstancedContentScope scope(context, view);
    StructRecordSource source;
    source.record = content.record;
    return DecodeStructValue(view, construction, owner, InstancedStructAccess::Value(*target),
                             source, context);
}

// InstancedStruct 필드·멤버 하나(지금 위치 — context.Field(), 호출자가 MatchesSchema를 확인했다)를
// 해석한다: wire 종류(InstancedStruct가 아니면 TypeMismatch) → payload 전부를 내용으로 읽기
// (ReadInstancedStructField — 빈 payload = 비어 있음, 아니면 Record 하나) →
// DecodeInstancedStructContent. 방식은 DecodeInstancedStructContent와 같다.
//
// 사용 예(생성 decode thunk 2단계):
//   auto owner = construction->GetForWrite<Table>(handle);
//   return DecodeInstancedStructField(construction, handle, &owner.Value()->row, field, context);
template <class Base>
Result<void> DecodeInstancedStructField(ConstructionContext* construction, ObjectHandle owner,
                                        InstancedStruct<Base>* target, const FieldView& field,
                                        FieldDecodeContext& context) noexcept {
    if (field.wireType != static_cast<uint32>(WireType::InstancedStruct)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    auto opened = context.OpenValue(field);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    auto content = ReadInstancedStructField(opened.Value());
    if (!content) {
        return Result<void>::Failure(content.Error());
    }

    return DecodeInstancedStructContent(construction, owner, target, content.Value(), context);
}

// 원소·Map 값 자리의 InstancedStruct 하나(u64 길이 + 내용)를 elements에서 읽어 해석한다
// (ReadInstancedStructElement → DecodeInstancedStructContent). 배열·정적 배열·Map codec이 원소마다
// 부른다(ElementIndexScope 안).
template <class Base>
Result<void> DecodeInstancedStructElement(ConstructionContext* construction, ObjectHandle owner,
                                          InstancedStruct<Base>* target, ArchiveReader& elements,
                                          FieldDecodeContext& context) noexcept {
    auto content = ReadInstancedStructElement(elements);
    if (!content) {
        return Result<void>::Failure(content.Error());
    }

    return DecodeInstancedStructContent(construction, owner, target, content.Value(), context);
}

} // namespace rocks::detail
