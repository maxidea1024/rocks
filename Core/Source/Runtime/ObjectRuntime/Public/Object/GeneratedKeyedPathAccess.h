#pragma once

#include <Base/TypeTraits.h>
#include <Object/GeneratedPathAccess.h>

// Map·Set 필드의 경로 key 단계(리플렉션 B9f3 — Docs/Design/ReflectionExtensions.ko.md 1.5·7.3절)
// 생성 thunk 템플릿이다. 선언과 공통 규칙(정적 대조·고정·게시)은 Object/GeneratedPathAccess.h에
// 있고 이 헤더는 그 끝에서 include된다(생성 헤더는 GeneratedPathAccess.h만 include한다). 키 값은
// 경로 실행기(PathRunner)가 해석·검증한 PathFrame::key(Name은 pool 토큰 keyName·keyFound)이고,
// 조회는 할당 없는 형식(StringView·SoftObjectRefView·토큰)으로 한다. 넣기는 키가 없을 때만 고정
// 아래 넣을 키를 준비하고(ClassRef 등록·Name intern) 새 키·값을 컨테이너의 allocator로 만든 뒤
// 저장소를 준비하고(PrepareReserve — 할당만) owner를 다시 확인한 다음 적용(CommitReserve)과 무할당
// Insert·Add로 게시한다. 지우기·비우기도 고정 아래 한다(키는 Erase 안에서 소멸한다 — 그 해제
// 콜백의 재진입 쓰기는 Busy다). 순회 중인 컨테이너의 쓰기·넣기·지우기·비우기는 Busy다(RF 결정 20 —
// 할당은 게시 전, 게시는 무할당, 옛 값을 돌려준다). ObjectRefMap(리플렉션 B9f4)은 고정 없이
// Registry API로 바꾼다(PathObjectRefMap — ObjectRefArray 갈래와 같은 규칙).
namespace rocks::detail {

// key 단계 키 값(PathFrame::key — 검증을 마친 정규형)의 C++ 단일 값이다(String·Name·Soft 제외 —
// 그 셋은 PathLookupKey·MakePathKey가 따로 만든다).
template <class K>
K PathScalarKey(const PathKeyValue& key) noexcept {
    if constexpr (IsSameV<K, bool>) {
        return key.boolean;
    } else if constexpr (ReflectedEnumField<K>::Supported) {
        return static_cast<K>(key.signedInteger);
    } else if constexpr (IsSameV<K, Guid>) {
        return key.guid;
    } else if constexpr (IsSameV<K, AssetId>) {
        return AssetId{key.guid};
    } else if constexpr (IsSameV<K, Sha256Digest>) {
        return key.digest;
    } else if constexpr (ClassReferenceField<K>::Supported) {
        return K(key.guid);
    } else if constexpr (IsSameV<K, int8> || IsSameV<K, int16> || IsSameV<K, int32> ||
                         IsSameV<K, int64>) {
        return static_cast<K>(key.signedInteger);
    } else {
        static_assert(IsSameV<K, uint8> || IsSameV<K, uint16> || IsSameV<K, uint32> ||
                          IsSameV<K, uint64>,
                      "reflected map key kind has no path key conversion");
        return static_cast<K>(key.unsignedInteger);
    }
}

// key 단계의 할당 없는 조회 값이다: String은 StringView, Soft는 SoftObjectRefView(입력 wire를
// 빌린다), Name은 pool 토큰(PathFrame::keyName), 그 밖은 PathScalarKey다.
template <class K>
auto PathLookupKey(const PathFrame& frame) noexcept {
    if constexpr (IsSameV<K, String>) {
        return frame.key.text;
    } else if constexpr (SoftReferenceField<K>::Supported) {
        return SoftObjectRefView{AssetId{frame.key.guid}, frame.key.text};
    } else if constexpr (IsSameV<K, Name>) {
        return frame.keyName;
    } else {
        return PathScalarKey<K>(frame.key);
    }
}

// 넣을 키 값이다: String·Soft는 allocator(컨테이너의 allocator)로 만들어 복사하고(OutOfMemory —
// 규칙 검사는 key 해석이 마쳤다), Name은 넣기 준비가 intern한 토큰, 그 밖은 PathScalarKey다.
template <class K>
Result<K> MakePathKey(Allocator& allocator, const PathFrame& frame) noexcept {
    if constexpr (IsSameV<K, String>) {
        String key(allocator);
        if (auto copied = key.TryAssign(frame.key.text); !copied) {
            return Result<K>::Failure(copied.Error());
        }

        return Result<K>::Success(Move(key));
    } else if constexpr (SoftReferenceField<K>::Supported) {
        K key(allocator);
        if (auto copied = key.TryAssign(AssetId{frame.key.guid}, frame.key.text); !copied) {
            return Result<K>::Failure(copied.Error());
        }

        return Result<K>::Success(Move(key));
    } else if constexpr (IsSameV<K, Name>) {
        return Result<K>::Success(frame.keyName);
    } else {
        return Result<K>::Success(PathScalarKey<K>(frame.key));
    }
}

// 새로 넣을 키의 정적 검사다(리플렉션 B9f3): ClassRef 키는 정적 Base의 파생이어야 한다(등록·
// 스키마 Base는 넣기 준비 — PathCursor::PrepareInsertedKey — 가 봤다).
template <class K>
Result<void> CheckInsertedKey(PathCursor& cursor) noexcept {
    if constexpr (ClassReferenceField<K>::Supported) {
        return cursor.CheckKeyClass(NativeTypeKey<typename ClassReferenceField<K>::Target>());
    } else {
        return Result<void>::Success();
    }
}

// 없던 키를 넣기 전의 공통 준비다: 길이(MaxElements) 검사 → 고정 → 넣을 키 준비(ClassRef 등록·
// Base 파생, Name 내용 규칙과 intern — 고정 아래라 그 할당 콜백의 재진입 쓰기는 Busy다) → 정적 키
// 검사.
template <class Key, class Container>
Result<void> BeginKeyInsertion(PathCursor& cursor, const Container& container) noexcept {
    if (auto fits = cursor.CheckLength(container.Count() + 1); !fits) {
        return fits;
    }

    if (auto pinned = cursor.Pin(); !pinned) {
        return pinned;
    }

    if (auto prepared = cursor.PrepareInsertedKey(); !prepared) {
        return prepared;
    }

    return CheckInsertedKey<Key>(cursor);
}

// 고정 아래 새 키·값을 만든 뒤의 저장소 준비와 게시 직전 확인이다: PrepareReserve(Count() + 1)는
// 새 저장소만 잡고 컨테이너는 바꾸지 않는다. owner가 그 사이 파괴됐으면(Destroyed 등) 준비한
// 저장소만 반환하고 컨테이너는 그대로다(주소 불변). 아니면 CommitReserve(할당·실패 없음 — 옛
// 저장소는 새 저장소를 게시한 뒤 반환한다)로 적용하므로 뒤의 Insert·Add는 할당하지 않는다.
template <class Container>
Result<void> ReserveForInsertion(PathCursor& cursor, Container& container) noexcept {
    auto reservation = container.PrepareReserve(container.Count() + 1);
    if (!reservation) {
        return cursor.Fail(reservation.Error());
    }

    if (auto alive = cursor.CheckOwner(); !alive) {
        return alive;
    }

    container.CommitReserve(reservation.Value());
    return Result<void>::Success();
}

// 단일 값 Map에 없던 키의 항목을 넣는다(PathWrite — 리플렉션 B9f3): 넣기 준비(BeginKeyInsertion)와
// 정적 값 대상(weak·ClassRef 값) 검사 뒤 고정 아래 키·값을 Map의 allocator로 만들고 저장소를
// 준비·확인·적용한 다음(ReserveForInsertion) 무할당 Insert로 끝에 넣는다. previous는 kind = Target,
// present = false(없던 항목 — enum이면 type에 그 enum ID)다.
template <ValueKind Target, class Map>
Result<void> InsertMapValue(PathCursor& cursor, Map& map) noexcept {
    using Key = typename MapField<Map>::Key;
    using Value = typename MapField<Map>::Value;
    if (auto begun = BeginKeyInsertion<Key>(cursor, map); !begun) {
        return begun;
    }

    if constexpr (Target == ValueKind::WeakObjectReference) {
        if (auto checked =
                cursor.CheckWeakTarget(NativeTypeKey<typename WeakReferenceField<Value>::Target>());
            !checked) {
            return checked;
        }
    }

    if constexpr (Target == ValueKind::ClassReference) {
        if (auto checked = cursor.CheckClassTarget(
                NativeTypeKey<typename ClassReferenceField<Value>::Target>());
            !checked) {
            return checked;
        }
    }

    Allocator& allocator = map.GetAllocator();
    auto key = MakePathKey<Key>(allocator, cursor.Frame());
    if (!key) {
        return cursor.Fail(key.Error());
    }

    Value value = MakeAllocatedValue<Value>(allocator);
    if (auto written = WriteTargetValue<Target>(value, *cursor.Input()); !written) {
        return cursor.FailValue(written.Error());
    }

    if (auto reserved = ReserveForInsertion(cursor, map); !reserved) {
        return reserved;
    }

    auto inserted = map.Insert(Move(key.Value()), Move(value));
    if (!inserted || inserted.Value() != InsertOutcome::Inserted) {
        platform::Fatal("path map insertion into a reserved map failed");
    }

    cursor.ReleasePin();
    if (FieldValue* previous = cursor.Output()) {
        FieldValue captured(previous->GetAllocator());
        captured.kind = Target;
        captured.present = false;
        if constexpr (Target == ValueKind::Enum) {
            captured.type = GeneratedType<Value>::Id();
        }

        cursor.PublishOutput(captured);
    }

    return Result<void>::Success();
}

// PathInsertKey의 본체다(리플렉션 B9f3): Set은 원소가 이미 있으면 결과 0(무동작 — 넣기 준비도 하지
// 않는다), 없으면 넣기 준비 뒤 고정 아래 키를 만들고 저장소를 준비·확인·적용한 다음 무할당 Add로
// 끝에 넣는다(결과 1). Map<K, 값 struct>는 키가 있으면 고정 아래 기본 값을 만들고 owner를 다시
// 확인한 뒤 옛 값을 지역으로 옮기고 새 값을 이동 대입한다(자리 유지 — 결과 0, 옛 값은 고정을 푼 뒤
// 소멸), 없으면 Set과 같은 순서로 키와 기본 값을 넣는다(결과 1). 새 키·값은 컨테이너의 allocator로
// 만든다.
template <ValueKind Target, class Container, class Lookup>
Result<void> InsertPathKey(PathCursor& cursor, Container& container, const Lookup& lookup,
                           bool known) noexcept {
    using Traits = MapField<Container>;
    using Key = typename Traits::Key;
    using Value = typename Traits::Value;
    Allocator& allocator = container.GetAllocator();
    if constexpr (Traits::IsSet) {
        if (known && container.Contains(lookup)) {
            cursor.SetLength(0);
            return Result<void>::Success();
        }
    } else {
        static_assert(Target == ValueKind::Struct, "only struct-valued maps insert default values");
        static_assert(IsNothrowAssignableV<Value&, Value&&>,
                      "map struct values must be nothrow move assignable");
        if (Value* existing = known ? container.Find(lookup) : nullptr) {
            if (auto pinned = cursor.Pin(); !pinned) {
                return pinned;
            }

            Value created = MakeAllocatedValue<Value>(allocator);
            if (auto alive = cursor.CheckOwner(); !alive) {
                return alive;
            }

            // 옛 값은 고정을 푼 뒤(이 함수를 나갈 때) 소멸한다.
            [[maybe_unused]] Value previous(Move(*existing));
            *existing = Move(created);
            cursor.ReleasePin();
            cursor.SetLength(0);
            return Result<void>::Success();
        }
    }

    if (auto begun = BeginKeyInsertion<Key>(cursor, container); !begun) {
        return begun;
    }

    auto key = MakePathKey<Key>(allocator, cursor.Frame());
    if (!key) {
        return cursor.Fail(key.Error());
    }

    if constexpr (Traits::IsSet) {
        if (auto reserved = ReserveForInsertion(cursor, container); !reserved) {
            return reserved;
        }

        auto added = container.Add(Move(key.Value()));
        if (!added || added.Value() != InsertOutcome::Inserted) {
            platform::Fatal("path set insertion into a reserved set failed");
        }
    } else {
        Value created = MakeAllocatedValue<Value>(allocator);
        if (auto reserved = ReserveForInsertion(cursor, container); !reserved) {
            return reserved;
        }

        auto inserted = container.Insert(Move(key.Value()), Move(created));
        if (!inserted || inserted.Value() != InsertOutcome::Inserted) {
            platform::Fatal("path map insertion into a reserved map failed");
        }
    }

    cursor.ReleasePin();
    cursor.SetLength(1);
    return Result<void>::Success();
}

// PathEraseKey의 본체다(리플렉션 B9f3): 키가 없으면 NotFound(KeyNotFound). 갈래와 removed 유무에
// 관계없이 Erase 전에 owner를 고정하고 게시(Erase) 뒤에 푼다 — 키는 Erase가 항목을 논리적으로 지운
// 뒤 그 안에서 소멸하므로(해시 색인에 있어 미리 옮길 수 없다) 그 해제 콜백의 재진입 쓰기는
// Busy이고, 재진입 읽기는 지운 뒤의 Map을 본다. removed가 있으면 지울 값을 담는다: 단일 값 Map의
// String·Soft 값은 removed의 allocator가 같으면 게시 뒤 버퍼를 옮기고 다르면 고정 아래
// 복사하며(CaptureText), Set 원소(키라 옮길 수 없다)는 고정 아래 복사한다. Map 항목의 값은 지역으로
// 옮긴 뒤 Erase하고 고정을 푼 뒤 소멸한다.
template <ValueKind Target, class Container, class Lookup>
Result<void> ErasePathKey(PathCursor& cursor, Container& container, const Lookup& lookup,
                          bool known) noexcept {
    using Traits = MapField<Container>;
    using Key = typename Traits::Key;
    using Value = typename Traits::Value;
    FieldValue* removed = cursor.Output();
    if constexpr (Traits::IsSet) {
        const Key* stored = known ? container.Find(lookup) : nullptr;
        if (!stored) {
            return cursor.FailMissingKey();
        }

        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }

        FieldValue captured(removed ? removed->GetAllocator() : container.GetAllocator());
        if (removed) {
            if (auto copied = CopyTargetValue<Target>(*stored, captured); !copied) {
                return cursor.Fail(copied.Error());
            }

            if (auto alive = cursor.CheckOwner(); !alive) {
                return alive;
            }
        }

        if (!container.Erase(lookup)) {
            platform::Fatal("path set erase of a found element failed");
        }

        cursor.ReleasePin();
        if (removed) {
            cursor.PublishOutput(captured);
        }

        return Result<void>::Success();
    } else {
        Value* value = known ? container.Find(lookup) : nullptr;
        if (!value) {
            return cursor.FailMissingKey();
        }

        if (auto pinned = cursor.Pin(); !pinned) {
            return pinned;
        }

        if constexpr (Target == ValueKind::Struct) {
            // 소멸을 게시(와 고정 해제) 뒤로 미루려고 붙잡는다.
            [[maybe_unused]] Value previous(Move(*value));
            if (!container.Erase(lookup)) {
                platform::Fatal("path map erase of a found entry failed");
            }

            cursor.ReleasePin();
            return Result<void>::Success();
        } else {
            FieldValue captured(removed ? removed->GetAllocator() : container.GetAllocator());
            bool moveText = false;
            if (removed) {
                Result<void> prepared = Result<void>::Success();
                if constexpr (Target == ValueKind::Utf8String) {
                    prepared = cursor.CaptureText(*value, nullptr, captured, moveText);
                } else if constexpr (Target == ValueKind::SoftObjectReference) {
                    prepared =
                        cursor.CaptureText(value->objectPath, &value->asset, captured, moveText);
                } else {
                    prepared = CopyTargetValue<Target>(*value, captured);
                    if (!prepared) {
                        return cursor.Fail(prepared.Error());
                    }
                }

                if (!prepared) {
                    return prepared;
                }
            }

            // String·Soft만 아래에서 쓴다(스칼라 값은 게시 뒤 소멸을 위해 붙잡을 뿐이다).
            [[maybe_unused]] Value previous(Move(*value));
            if (!container.Erase(lookup)) {
                platform::Fatal("path map erase of a found entry failed");
            }

            cursor.ReleasePin();
            if (moveText) {
                if constexpr (Target == ValueKind::Utf8String) {
                    captured.string.Swap(previous);
                } else if constexpr (Target == ValueKind::SoftObjectReference) {
                    captured.string.Swap(previous.objectPath);
                }
            }

            if (removed) {
                cursor.PublishOutput(captured);
            }

            return Result<void>::Success();
        }
    }
}

// ObjectRefMap 필드·멤버의 단계다(리플렉션 B9f4 — Docs/Design/ReflectionExtensions.ko.md 7.3절
// "B9f4 주의"·7.4절). ObjectRefArray 갈래처럼 고정을 쥐지 않고 Registry API(SetMapReference·
// EraseMapReference·ClearMapReferences)로 바꾼다 — Registry가 owner·대상·순회 중(Busy)·관찰자·삽입
// 장벽을 판정하고 할당 콜백 뒤 스스로 다시 판단한다. key 해석·검증 지점(대상 타입·생존 lease)·넣기
// 준비(ClassRef 키 lease·Name 키 intern)·문자열 경로는 다른 Map과 같다. 연산: 원소 수·비우기(key
// 없는 끝 단계), 조회, 값 읽기(없는 키 NotFound(KeyNotFound)), 쓰기(있으면 자리를 지키며 교체하고
// previous는 Registry가 게시 잠금 안에서 실제로 덮어쓴 옛 handle, 없으면 넣기 준비 뒤 끝에 넣고
// previous는 present = false — 새 키의 MaxElements는 Registry가 게시 직전에도 본다),
// 지우기(removed는 지운 handle). 값 struct가 없으므로 중간 단계와 InsertKey는 다른 단일 값 Map과
// 같이 거부한다.
template <ValueKind Target, class K, class T>
Result<void> PathObjectRefMap(PathCursor& cursor, ObjectRefMap<K, T>& map) noexcept {
    static_assert(Target == ValueKind::ObjectReference,
                  "ObjectRefMap values are object references");
    // 시작 위치에 준비 사본이 걸려 있으면 객체 대신 사본을 바꾼다(리플렉션 B9i3c3).
    if (cursor.StagedReferences()) {
        return PathStagedReferenceMap(cursor, map);
    }

    ObjectRegistry& registry = cursor.Registry();
    const ObjectAccessContext& access = cursor.Access();
    const ObjectHandle owner = cursor.OwnerHandle();
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    // 항목 쓰기·지우기·비우기는 게시된 객체의 Registry API다(staged는 트랜잭션의
    // AssignMapReferences — 리플렉션 B9i3c0).
    if (!cursor.IsReading()) {
        if (auto live = cursor.RequireLive(); !live) {
            return live;
        }
    }

    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(map.Count());
            return Result<void>::Success();
        }

        if (operation == PathOperation::Clear) {
            return cursor.Finish(registry.ClearMapReferences(access, owner, map));
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }

    if constexpr (ReflectedEnumField<K>::Supported) {
        if (!GeneratedType<K>::IsDeclared(frame.key.signedInteger)) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey);
        }
    }

    if (!cursor.IsLast()) {
        return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
    }

    const auto lookup = PathLookupKey<K>(frame);
    const ObjectHandle* found = frame.keyFound ? map.Find(lookup) : nullptr;
    switch (operation) {
    case PathOperation::Contains:
        cursor.SetLength(found ? 1 : 0);
        return Result<void>::Success();
    case PathOperation::InsertKey:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
    case PathOperation::EraseKey: {
        if (!found) {
            return cursor.FailMissingKey();
        }

        // removed는 Registry가 게시 잠금 안에서 실제로 지운 값이다.
        ObjectHandle previous;
        if (auto erased = registry.EraseMapReference(access, owner, map, lookup, &previous);
            !erased) {
            return cursor.Fail(erased.Error());
        }

        cursor.PublishReference(previous);
        return Result<void>::Success();
    }
    case PathOperation::Read: {
        if (!found) {
            return cursor.FailMissingKey();
        }

        FieldValue candidate(cursor.Output()->GetAllocator());
        candidate.kind = ValueKind::ObjectReference;
        candidate.reference = *found;
        return cursor.PublishRead(candidate);
    }
    case PathOperation::Write: {
        // Registry가 게시 잠금 안에서 실제로 덮어쓴 옛 값과 키가 있었는지(MapReferenceWrite)로
        // previous를 채운다 — 호출 전에 읽은 값은 넣기 준비·할당·관찰자 예약 콜백이 그 사이 바꿀 수
        // 있다(리플렉션 B9f4 리뷰 반영). 새 키의 MaxElements도 Registry가 게시 직전에 다시 본다.
        MapReferenceWrite write;
        write.maxCount = cursor.Frame().field->maxElements;
        Result<void> set = Result<void>::Success();
        if (found) {
            set = registry.SetMapReference(access, owner, map, lookup, cursor.Input()->reference,
                                           &write);
        } else {
            // 없던 키: 길이 검사 → 넣기 준비(ClassRef 등록·Base, Name 내용 규칙과 intern — 대상을
            // 바꾸지 않으므로 고정 없이 한다) → 정적 키 검사 → Registry 넣기(Name 키는 넣기 준비가
            // intern한 토큰(frame.keyName)으로 다시 만든다 — 그 사이 같은 키가 들어왔으면 교체다).
            if (auto fits = cursor.CheckLength(map.Count() + 1); !fits) {
                return fits;
            }

            if (auto prepared = cursor.PrepareInsertedKey(); !prepared) {
                return prepared;
            }

            if (auto checked = CheckInsertedKey<K>(cursor); !checked) {
                return checked;
            }

            const auto inserted = PathLookupKey<K>(cursor.Frame());
            set = registry.SetMapReference(access, owner, map, inserted, cursor.Input()->reference,
                                           &write);
        }

        if (!set) {
            return set.Error() == ErrorCode::CapacityExceeded && map.Count() >= write.maxCount
                       ? cursor.Fail(ErrorCode::CapacityExceeded,
                                     PropertyPathError::TooManyElements)
                       : cursor.Fail(set.Error());
        }

        if (write.replaced) {
            cursor.PublishReference(write.previous);
        } else if (FieldValue* previous = cursor.Output()) {
            FieldValue captured(previous->GetAllocator());
            captured.kind = ValueKind::ObjectReference;
            captured.present = false;
            cursor.PublishOutput(captured);
        }

        return Result<void>::Success();
    }
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::KeyRequired);
    }
}

template <ValueKind Target, class Container>
Result<void> PathKeyedValueContainer(PathCursor& cursor, Container& container) noexcept;

// Map·Set 필드·멤버의 단계다(리플렉션 B9f3 — Docs/Design/ReflectionExtensions.ko.md 7.3절). 연산과
// 무관하게 먼저 Map 키 종류(키 enum ID)와 값 struct를 정적 대조한다. key 없는 끝 단계는 원소 수·
// 비우기(옛 저장소는 게시 뒤 반환)이고, key 단계는 키 값(enum 키는 정적 선언도 본다 — 아니면
// InvalidArgument(InvalidKey))으로 항목을 찾아 중간 단계면 Map 값 struct로 내려가고(없으면
// NotFound(KeyNotFound)), 끝 단계면 조회·넣기·지우기·값 읽기·쓰기(단일 값 Map — 없는 키 쓰기는
// InsertMapValue)다. Name 키가 pool에 없으면(keyFound 거짓) 항목도 없다. ObjectRefMap(리플렉션
// B9f4)은 정적 대조 뒤 PathObjectRefMap(Registry API 갈래)이다.
template <ValueKind Target, class Container>
Result<void> PathKeyedContainer(PathCursor& cursor, Container& container) noexcept {
    using Traits = MapField<Container>;
    using Key = typename Traits::Key;
    using Value = typename Traits::Value;
    constexpr bool set = Traits::IsSet;
    constexpr bool structValue = !set && Target == ValueKind::Struct;
    if constexpr (!set) {
        if constexpr (ReflectedEnumField<Key>::Supported) {
            if (auto checked = cursor.CheckKey(ValueKind::Enum, GeneratedType<Key>::Id());
                !checked) {
                return checked;
            }
        } else {
            if (auto checked = cursor.CheckKey(FieldValueKind<Key>()); !checked) {
                return checked;
            }
        }
    }

    if constexpr (structValue) {
        if (auto checked = cursor.CheckStruct(NativeTypeKey<Value>()); !checked) {
            return checked;
        }
    }

    // ObjectRefMap(리플렉션 B9f4)은 Registry API 갈래다.
    if constexpr (Traits::Registry) {
        return PathObjectRefMap<Target>(cursor, container);
    } else {
        return PathKeyedValueContainer<Target>(cursor, container);
    }
}

// Map·Set(Registry 컨테이너가 아닌 OrderedMap·OrderedSet) 단계의 본문이다(PathKeyedContainer가 정적
// 대조 뒤 부른다).
template <ValueKind Target, class Container>
Result<void> PathKeyedValueContainer(PathCursor& cursor, Container& container) noexcept {
    using Traits = MapField<Container>;
    using Key = typename Traits::Key;
    using Value = typename Traits::Value;
    constexpr bool set = Traits::IsSet;
    constexpr bool structValue = !set && Target == ValueKind::Struct;
    const PathFrame& frame = cursor.Frame();
    const PathOperation operation = cursor.Operation();
    if (frame.selector == PathSelector::None) {
        if (operation == PathOperation::Length) {
            cursor.SetLength(container.Count());
            return Result<void>::Success();
        }

        if (operation == PathOperation::Clear) {
            if (container.IsIterating()) {
                return cursor.Fail(ErrorCode::Busy);
            }

            // 고정 아래 컨테이너를 지역으로 옮기고(빈 컨테이너 게시) 옛 원소·저장소는 고정을 푼 뒤
            // 반환한다.
            if (auto pinned = cursor.Pin(); !pinned) {
                return pinned;
            }

            Container old(Move(container));
            cursor.ReleasePin();
            return Result<void>::Success();
        }

        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
    }

    if constexpr (ReflectedEnumField<Key>::Supported) {
        if (!GeneratedType<Key>::IsDeclared(frame.key.signedInteger)) {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey);
        }
    }

    const auto lookup = PathLookupKey<Key>(frame);
    const bool known = frame.keyFound;
    if (!cursor.IsLast()) {
        if constexpr (structValue) {
            Value* value = known ? container.Find(lookup) : nullptr;
            if (!value) {
                return cursor.FailMissingKey();
            }

            return cursor.Descend(value);
        } else {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct);
        }
    }

    // 순회(ForEach 등) 중인 컨테이너는 구조를 바꾸지 않는다 — Core 컨테이너의 Fatal에 닿지 않게
    // Busy로 거절한다(값 교체도 같다).
    if (operation != PathOperation::Contains && operation != PathOperation::Read &&
        container.IsIterating()) {
        return cursor.Fail(ErrorCode::Busy);
    }

    switch (operation) {
    case PathOperation::Contains:
        cursor.SetLength(known && container.Contains(lookup) ? 1 : 0);
        return Result<void>::Success();
    case PathOperation::InsertKey:
        if constexpr (set || structValue) {
            return InsertPathKey<Target>(cursor, container, lookup, known);
        } else {
            return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
        }
    case PathOperation::EraseKey:
        return ErasePathKey<Target>(cursor, container, lookup, known);
    case PathOperation::Read:
    case PathOperation::Write:
        if constexpr (set) {
            return cursor.Fail(ErrorCode::TypeMismatch, PropertyPathError::NotMap);
        } else if constexpr (structValue) {
            return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::AggregateValue);
        } else {
            Value* value = known ? container.Find(lookup) : nullptr;
            if (value) {
                return PathValue<Target>(cursor, *value);
            }

            if (operation == PathOperation::Read) {
                return cursor.FailMissingKey();
            }

            return InsertMapValue<Target>(cursor, container);
        }
    default:
        return cursor.Fail(ErrorCode::InvalidArgument, PropertyPathError::KeyRequired);
    }
}

} // namespace rocks::detail
