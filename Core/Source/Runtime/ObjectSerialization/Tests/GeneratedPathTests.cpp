#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// MetaCompiler 생성 경로 thunk(B6b)로 등록한 리플렉션 타입의 경로 접근 시험이다(Docs/Design/
// ReflectionFoundations.ko.md 7.1절 "시험"의 생성 항목). PropertyPathTests가 손으로 쓴 특수화로
// 검증한 모양을 생성 특수화와 모듈 집계 등록으로 다시 확인한다: Struct·Array 필드와 모든 struct
// 멤버에만 path thunk가 있고, StructFixture·ArrayFixture·CoreValueFixture의 모든 종류를 문자열
// 경로로 읽고 쓰고 넣고 빼고 비우며(Binds 미러 멤버 placement.translation.x, 상속 배열 필드, base
// lease로 해석한 경로), 경로로 쓴 객체가 그래프 저장·로드에서 그대로 돌아오고 다시 저장한 byte가
// 같다. PathFixture로 도구 이름(Name 지정자)과 ReadOnly 전파를 본다.
using namespace rocks;
using reflection_fixture::ArrayChild;
using reflection_fixture::ArrayOwner;
using reflection_fixture::Channels;
using reflection_fixture::CoreValueFields;
using reflection_fixture::PathOwner;
using reflection_fixture::PropertyNode;
using reflection_fixture::StructOwner;
using reflection_fixture::TextureGroup;

namespace {

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

StringView NameOf(const FieldSchema& field) noexcept {
    return StringView(field.name.Data(), field.name.Size());
}

// field 철자 뒤에 "[index]"와 suffix를 붙인 경로 철자다(시험 철자는 짧다 — 넘치면 Fatal).
class Indexed {
public:
    Indexed(const char* field, uint32 index, const char* suffix = "") noexcept {
        Append(field);
        Append("[");
        char digits[10];
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + index % 10);
            index /= 10;
        } while (index != 0);

        while (count != 0) {
            Put(digits[--count]);
        }

        Append("]");
        Append(suffix);
        text_[size_] = '\0';
    }

    const char* Text() const noexcept {
        return text_;
    }

private:
    void Put(char value) noexcept {
        if (size_ + 1 >= sizeof(text_)) {
            platform::Fatal("test path spelling too long");
        }

        text_[size_++] = value;
    }

    void Append(const char* text) noexcept {
        for (; *text != '\0'; ++text) {
            Put(*text);
        }
    }

    char text_[96] = {};
    usize size_ = 0;
};

// 객체 하나에 문자열 경로로 연산한다: type(클래스 lease)의 스키마로 해석한 뒤 handle에 경로 연산을
// 부른다. 해석이 실패하면 그 오류를 돌려준다(failure도 해석 실패를 담는다).
class Paths {
public:
    Paths(ObjectRegistry& objects, const TypeLease& type, ObjectHandle handle) noexcept
        : objects_(objects), type_(type), handle_(handle) {
    }

    Result<void> Read(const char* text, FieldValue& out,
                      PropertyPathFailure* failure = nullptr) const noexcept {
        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(type_, StringView(text), path, failure); !parsed) {
            return parsed;
        }

        return PathRead(objects_, objects_.OwnerAccess(), handle_, path.View(), out, failure);
    }

    Result<void> Write(const char* text, const FieldValue& value, FieldValue* previous = nullptr,
                       PropertyPathFailure* failure = nullptr) const noexcept {
        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(type_, StringView(text), path, failure); !parsed) {
            return parsed;
        }

        return PathWrite(objects_, objects_.OwnerAccess(), handle_, path.View(), value, previous,
                         failure);
    }

    Result<usize> Length(const char* text, PropertyPathFailure* failure = nullptr) const noexcept {
        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(type_, StringView(text), path, failure); !parsed) {
            return Result<usize>::Failure(parsed.Error());
        }

        return PathArrayLength(objects_, objects_.OwnerAccess(), handle_, path.View(), failure);
    }

    Result<void> Insert(const char* text, const FieldValue* value,
                        PropertyPathFailure* failure = nullptr) const noexcept {
        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(type_, StringView(text), path, failure); !parsed) {
            return parsed;
        }

        return PathInsertElement(objects_, objects_.OwnerAccess(), handle_, path.View(), value,
                                 failure);
    }

    Result<void> Remove(const char* text, FieldValue* removed = nullptr,
                        PropertyPathFailure* failure = nullptr) const noexcept {
        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(type_, StringView(text), path, failure); !parsed) {
            return parsed;
        }

        return PathRemoveElement(objects_, objects_.OwnerAccess(), handle_, path.View(), removed,
                                 failure);
    }

    Result<void> Clear(const char* text, PropertyPathFailure* failure = nullptr) const noexcept {
        PropertyPathBuffer path;
        if (auto parsed = ParsePropertyPath(type_, StringView(text), path, failure); !parsed) {
            return parsed;
        }

        return PathClearArray(objects_, objects_.OwnerAccess(), handle_, path.View(), failure);
    }

private:
    ObjectRegistry& objects_;
    const TypeLease& type_;
    ObjectHandle handle_;
};

FieldValue Boolean(Allocator& allocator, bool value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Bool;
    result.boolean = value;
    return result;
}

FieldValue Unsigned(Allocator& allocator, ValueKind kind, uint64 value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.unsignedInteger = value;
    return result;
}

FieldValue Signed(Allocator& allocator, ValueKind kind, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.signedInteger = value;
    return result;
}

FieldValue Float32(Allocator& allocator, float value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Float32;
    result.float32 = value;
    return result;
}

FieldValue Float64(Allocator& allocator, double value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Float64;
    result.float64 = value;
    return result;
}

// Guid·AssetId 값이다(kind로 고른다).
FieldValue Identifier(Allocator& allocator, ValueKind kind, Guid value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.guid = value;
    return result;
}

// 시험 digest다(byte i = seed + 3i).
Sha256Digest TestDigest(uint8 seed) noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(seed + index * 3);
    }

    return digest;
}

FieldValue Digest(Allocator& allocator, uint8 seed) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Digest256;
    result.digest = TestDigest(seed);
    return result;
}

FieldValue Text(Allocator& allocator, const char* text) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Utf8String;
    if (!result.string.TryAssign(text)) {
        platform::Fatal("test text allocation failed");
    }

    return result;
}

FieldValue Soft(Allocator& allocator, Guid asset, const char* path) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::SoftObjectReference;
    result.guid = asset;
    if (!result.string.TryAssign(path)) {
        platform::Fatal("test soft path allocation failed");
    }

    return result;
}

FieldValue EnumValue(Allocator& allocator, TypeId type, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Enum;
    result.type = type;
    result.signedInteger = value;
    return result;
}

FieldValue Reference(Allocator& allocator, ObjectHandle handle) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::ObjectReference;
    result.reference = handle;
    return result;
}

// 종류에 해당하는 칸만 비교한다.
bool SameValue(const FieldValue& left, const FieldValue& right) noexcept {
    if (left.kind != right.kind) {
        return false;
    }

    switch (left.kind) {
    case ValueKind::Bool:
        return left.boolean == right.boolean;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
        return left.unsignedInteger == right.unsignedInteger;
    case ValueKind::Int32:
    case ValueKind::Int64:
        return left.signedInteger == right.signedInteger;
    case ValueKind::Float32:
        return left.float32 == right.float32;
    case ValueKind::Float64:
        return left.float64 == right.float64;
    case ValueKind::Guid:
    case ValueKind::AssetId:
        return left.guid == right.guid;
    case ValueKind::Utf8String:
        return left.string.View() == right.string.View();
    case ValueKind::SoftObjectReference:
        return left.guid == right.guid && left.string.View() == right.string.View();
    case ValueKind::Digest256:
        return left.digest == right.digest;
    case ValueKind::Enum:
        return left.signedInteger == right.signedInteger && left.type == right.type;
    case ValueKind::ObjectReference:
        return left.reference == right.reference;
    default:
        return false;
    }
}

// 경로와 그 값이다(FieldValue는 이동 전용이라 표의 원소로 둔다).
struct Entry {
    const char* path;
    FieldValue value;
};

// 단일 값 배열의 원소 연산 사례다(빈 배열에 first·second·third를 쓴다).
struct ArrayCase {
    const char* field;
    FieldValue first;
    FieldValue second;
    FieldValue third;
};

// view의 모든 멤버(중첩 struct·원소 struct의 멤버 포함)에 경로 thunk가 있는지 세며 확인한다.
void CheckMemberThunks(StructView view, usize& members, uint32 depth) noexcept {
    REQUIRE(depth <= MaxStructDepth);
    for (usize index = 0; index < view.Schema().fields.Size(); ++index) {
        CHECK(view.FieldOps(index).path != nullptr);
        ++members;
        StructView inner = view.FieldStruct(index);
        if (!inner.IsEmpty()) {
            CheckMemberThunks(inner, members, depth + 1);
        }
    }
}

// 클래스 type의 Struct·Array 필드에만 path thunk가 있고(단일 값 필드는 없다) 그 struct의 모든
// 멤버에 thunk가 있는지 세며 확인한다.
void CheckClassThunks(TypeRegistry& types, TypeId type, usize& paths, usize& scalars,
                      usize& members) noexcept {
    auto lease = types.Acquire(type);
    REQUIRE(lease);
    const TypeSchema& schema = lease.Value().Schema();
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const RuntimeFieldOps* ops = lease.Value().FieldOps(index);
        REQUIRE(ops);
        const ValueKind kind = schema.fields[index].value.kind;
        if (kind == ValueKind::Struct || kind == ValueKind::Array) {
            CHECK(ops->path != nullptr);
            ++paths;
        } else {
            CHECK(ops->path == nullptr);
            ++scalars;
        }

        StructView view = lease.Value().FieldStruct(index);
        if (!view.IsEmpty()) {
            CheckMemberThunks(view, members, 1);
        }
    }
}

// 빈 단일 값 배열의 원소 연산이다: 끝(index = 길이)·앞 삽입, 범위 밖 삽입·읽기, 읽기, 교체(옛 값),
// 삭제(removed — 뒤 원소를 당긴다), 길이, 비우기.
void ExerciseValueArray(const Paths& paths, Allocator& allocator, const ArrayCase& entry) noexcept {
    const Indexed at0(entry.field, 0);
    const Indexed at1(entry.field, 1);
    const Indexed at2(entry.field, 2);
    const Indexed at3(entry.field, 3);
    auto empty = paths.Length(entry.field);
    REQUIRE(empty && empty.Value() == 0);
    REQUIRE(paths.Insert(at0.Text(), &entry.first));
    REQUIRE(paths.Insert(at0.Text(), &entry.second));
    PropertyPathFailure failure;
    CHECK(ErrorOf(paths.Insert(at3.Text(), &entry.third, &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::IndexOutOfRange);
    FieldValue read(allocator);
    REQUIRE(paths.Read(at0.Text(), read));
    CHECK(SameValue(read, entry.second));
    REQUIRE(paths.Read(at1.Text(), read));
    CHECK(SameValue(read, entry.first));
    CHECK(ErrorOf(paths.Read(at2.Text(), read, &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::IndexOutOfRange);
    FieldValue previous(allocator);
    REQUIRE(paths.Write(at1.Text(), entry.third, &previous));
    CHECK(SameValue(previous, entry.first));
    FieldValue removed(allocator);
    REQUIRE(paths.Remove(at0.Text(), &removed));
    CHECK(SameValue(removed, entry.second));
    REQUIRE(paths.Read(at0.Text(), read));
    CHECK(SameValue(read, entry.third));
    auto one = paths.Length(entry.field);
    CHECK(one && one.Value() == 1);
    REQUIRE(paths.Clear(entry.field));
    auto cleared = paths.Length(entry.field);
    CHECK(cleared && cleared.Value() == 0);
}

// 빈 struct 원소 배열의 원소 연산이다: 값 없는 기본 원소 삽입(값을 주면 AggregateValue), 원소 멤버
// member 쓰기·읽기, 원소 통째 읽기(AggregateValue), removed를 준 삭제(AggregateValue), 삭제 뒤
// 당겨진 원소, 비우기.
void ExerciseStructArray(const Paths& paths, Allocator& allocator, const char* field,
                         const char* member, const FieldValue& first,
                         const FieldValue& second) noexcept {
    const Indexed at0(field, 0);
    const Indexed member0(field, 0, member);
    const Indexed member1(field, 1, member);
    REQUIRE(paths.Insert(at0.Text(), nullptr));
    REQUIRE(paths.Insert(at0.Text(), nullptr));
    PropertyPathFailure failure;
    CHECK(ErrorOf(paths.Insert(at0.Text(), &first, &failure)) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::AggregateValue);
    REQUIRE(paths.Write(member0.Text(), first));
    REQUIRE(paths.Write(member1.Text(), second));
    FieldValue read(allocator);
    CHECK(ErrorOf(paths.Read(at0.Text(), read, &failure)) == ErrorCode::Unsupported);
    CHECK(failure.reason == PropertyPathError::AggregateValue);
    FieldValue removed(allocator);
    CHECK(ErrorOf(paths.Remove(at0.Text(), &removed, &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::AggregateValue);
    REQUIRE(paths.Remove(at0.Text()));
    REQUIRE(paths.Read(member0.Text(), read));
    CHECK(SameValue(read, second));
    auto one = paths.Length(field);
    CHECK(one && one.Value() == 1);
    REQUIRE(paths.Clear(field));
    auto cleared = paths.Length(field);
    CHECK(cleared && cleared.Value() == 0);
}

// StructOwner의 모든 단일 값 멤버(참조·enum·String·EditorOnly 포함)와 최상위 단일 값을 경로로 쓴다.
void FillStructOwner(const Paths& paths, Allocator& allocator, ObjectHandle other) noexcept {
    const TypeId group = TypeIdOf<TextureGroup>();
    Entry entries[] = {{"holder.target", Reference(allocator, other)},
                       {"holder.middle.leaf.count", Unsigned(allocator, ValueKind::UInt32, 42)},
                       {"holder.middle.leaf.label", Text(allocator, "잎 경로")},
                       {"holder.middle.leaf.group", EnumValue(allocator, group, 7)},
                       {"holder.middle.offset", Signed(allocator, ValueKind::Int64, -5)},
                       {"holder.middle.note", Unsigned(allocator, ValueKind::UInt16, 65535)},
                       {"holder.flag", Unsigned(allocator, ValueKind::UInt8, 200)},
                       {"plain", Unsigned(allocator, ValueKind::UInt32, 9)},
                       {"extra.count", Unsigned(allocator, ValueKind::UInt32, 3)},
                       {"extra.label", Text(allocator, "extra")},
                       {"extra.group", EnumValue(allocator, group, 13)}};
    for (const Entry& entry : entries) {
        REQUIRE(paths.Write(entry.path, entry.value));
    }
}

// FillStructOwner가 쓴 값인지 본다(self는 그 객체 — holder.target은 다른 non-null 객체다).
void CheckStructOwner(const StructOwner& value, ObjectHandle self) noexcept {
    const ObjectHandle target = value.holder.target.Handle();
    CHECK(target != ObjectHandle{} && target != self);
    CHECK(value.holder.middle.leaf.count == 42);
    CHECK(value.holder.middle.leaf.label.View() == StringView("잎 경로"));
    CHECK(value.holder.middle.leaf.group == TextureGroup::EightBitData);
    CHECK(value.holder.middle.offset == -5 && value.holder.middle.note == 65535);
    CHECK(value.holder.flag == 200 && value.plain == 9);
    CHECK(value.extra.count == 3 && value.extra.label.View() == StringView("extra"));
    CHECK(value.extra.group == TextureGroup::Reserved13);
}

// ArrayChild의 모든 배열(상속 배열 필드·인라인 struct 안 컨테이너·원소 struct 안 배열 포함)을 경로
// 삽입과 원소 멤버 쓰기만으로 채운다. other는 다른 ArrayOwner다.
void FillArrayChild(const Paths& paths, Allocator& allocator, ObjectHandle self,
                    ObjectHandle other) noexcept {
    Entry elements[] = {{"flags[0]", Boolean(allocator, true)},
                        {"flags[1]", Boolean(allocator, false)},
                        {"longs[0]", Unsigned(allocator, ValueKind::UInt64, ~uint64{0})},
                        {"doubles[0]", Float64(allocator, -0.5)},
                        {"guids[0]", Identifier(allocator, ValueKind::Guid, Guid{1, 2})},
                        {"assets[0]", Identifier(allocator, ValueKind::AssetId, Guid{9, 9})},
                        {"digests[0]", Digest(allocator, 0)},
                        {"ints[0]", Signed(allocator, ValueKind::Int32, -7)},
                        {"ints[1]", Signed(allocator, ValueKind::Int32, 2147483647)},
                        {"names[0]", Text(allocator, "경로")},
                        {"names[1]", Text(allocator, "")},
                        {"groups[0]", EnumValue(allocator, TypeIdOf<TextureGroup>(), 7)},
                        {"channels[0]", EnumValue(allocator, TypeIdOf<Channels>(), 5)},
                        {"softs[0]", Soft(allocator, Guid{5, 5}, "Mesh.Socket")},
                        {"anySofts[0]", Soft(allocator, Guid{6, 6}, "")},
                        {"links[0]", Reference(allocator, other)},
                        {"links[1]", Reference(allocator, ObjectHandle{})},
                        {"links[2]", Reference(allocator, self)},
                        {"required[0]", Reference(allocator, other)},
                        {"bundle.refs[0]", Reference(allocator, ObjectHandle{})},
                        {"bundle.refs[1]", Reference(allocator, other)},
                        {"extra[0]", Unsigned(allocator, ValueKind::UInt8, 0)},
                        {"extra[1]", Unsigned(allocator, ValueKind::UInt8, 255)}};
    for (const Entry& entry : elements) {
        REQUIRE(paths.Insert(entry.path, &entry.value));
    }

    // struct 원소는 값 없이 기본 원소를 넣고 멤버를 쓴다.
    const char* const structElements[] = {"points[0]", "tags[0]", "slots[0]", "bundle.slots[0]"};
    for (const char* element : structElements) {
        REQUIRE(paths.Insert(element, nullptr));
    }

    Entry members[] = {{"points[0].x", Float32(allocator, 1.5f)},
                       {"points[0].y", Float32(allocator, -2.0f)},
                       {"points[0].z", Float32(allocator, 0.25f)},
                       {"tags[0].id", Unsigned(allocator, ValueKind::UInt32, 1)},
                       {"tags[0].label", Text(allocator, "첫째")},
                       {"slots[0].material", Reference(allocator, other)},
                       {"slots[0].name", Text(allocator, "s0")},
                       {"bundle.slots[0].material", Reference(allocator, other)},
                       {"bundle.slots[0].name", Text(allocator, "b0")}};
    for (const Entry& entry : members) {
        REQUIRE(paths.Write(entry.path, entry.value));
    }

    Entry nested[] = {{"tags[0].codes[0]", Unsigned(allocator, ValueKind::UInt16, 1)},
                      {"tags[0].codes[1]", Unsigned(allocator, ValueKind::UInt16, 65535)},
                      {"slots[0].weights[0]", Float32(allocator, 0.5f)},
                      {"bundle.slots[0].weights[0]", Float32(allocator, 1.0f)},
                      {"bundle.slots[0].weights[1]", Float32(allocator, 2.0f)}};
    for (const Entry& entry : nested) {
        REQUIRE(paths.Insert(entry.path, &entry.value));
    }
}

// FillArrayChild가 넣은 값인지 본다(self는 그 객체, 다른 객체 참조는 모두 links[0]과 같다).
void CheckArrayChild(const ArrayChild& value, ObjectHandle self) noexcept {
    REQUIRE(value.links.Size() == 3);
    const ObjectHandle other = value.links[0];
    CHECK(other != ObjectHandle{} && other != self);
    CHECK(value.links[1] == ObjectHandle{} && value.links[2] == self);
    CHECK(value.required.Size() == 1 && value.required[0] == other);
    CHECK(value.flags.Size() == 2 && value.flags[0] && !value.flags[1]);
    CHECK(value.longs.Size() == 1 && value.longs[0] == ~uint64{0});
    CHECK(value.doubles.Size() == 1 && value.doubles[0] == -0.5);
    CHECK(value.guids.Size() == 1 && value.guids[0] == (Guid{1, 2}));
    CHECK(value.assets.Size() == 1 && value.assets[0] == (AssetId{Guid{9, 9}}));
    CHECK(value.digests.Size() == 1 && value.digests[0] == TestDigest(0));
    CHECK(value.ints.Size() == 2 && value.ints[0] == -7 && value.ints[1] == 2147483647);
    CHECK(value.names.Size() == 2 && value.names[0].View() == StringView("경로") &&
          value.names[1].IsEmpty());
    CHECK(value.groups.Size() == 1 && value.groups[0] == TextureGroup::EightBitData);
    CHECK(value.channels.Size() == 1 && static_cast<uint32>(value.channels[0]) == 5);
    CHECK(value.softs.Size() == 1 && value.softs[0].asset == (AssetId{Guid{5, 5}}) &&
          value.softs[0].objectPath.View() == StringView("Mesh.Socket"));
    CHECK(value.anySofts.Size() == 1 && value.anySofts[0].asset == (AssetId{Guid{6, 6}}) &&
          value.anySofts[0].objectPath.IsEmpty());
    CHECK(value.points.Size() == 1 && value.points[0].x == 1.5f && value.points[0].y == -2.0f &&
          value.points[0].z == 0.25f);
    REQUIRE(value.tags.Size() == 1);
    CHECK(value.tags[0].id == 1 && value.tags[0].label.View() == StringView("첫째"));
    CHECK(value.tags[0].codes.Size() == 2 && value.tags[0].codes[0] == 1 &&
          value.tags[0].codes[1] == 65535);
    REQUIRE(value.slots.Size() == 1 && value.bundle.slots.Size() == 1);
    CHECK(value.slots[0].material.Handle() == other &&
          value.slots[0].name.View() == StringView("s0"));
    CHECK(value.slots[0].weights.Size() == 1 && value.slots[0].weights[0] == 0.5f);
    CHECK(value.bundle.refs.Size() == 2 && value.bundle.refs[0] == ObjectHandle{} &&
          value.bundle.refs[1] == other);
    CHECK(value.bundle.slots[0].material.Handle() == other &&
          value.bundle.slots[0].name.View() == StringView("b0"));
    CHECK(value.bundle.slots[0].weights.Size() == 2 && value.bundle.slots[0].weights[0] == 1.0f &&
          value.bundle.slots[0].weights[1] == 2.0f);
    CHECK(value.extra.Size() == 2 && value.extra[0] == 0 && value.extra[1] == 255);
}

// CoreValueFields의 Binds 미러 멤버(중첩 미러 Transform·사용자 struct 안 미러 포함)를 경로로 쓴다.
void FillCoreValues(const Paths& paths, Allocator& allocator) noexcept {
    Entry entries[] = {{"position.x", Float32(allocator, 1.5f)},
                       {"position.y", Float32(allocator, -2.25f)},
                       {"position.z", Float32(allocator, 3.0f)},
                       {"rotation.z", Float32(allocator, 0.75f)},
                       {"rotation.w", Float32(allocator, -0.5f)},
                       {"tint.r", Unsigned(allocator, ValueKind::UInt8, 255)},
                       {"tint.a", Unsigned(allocator, ValueKind::UInt8, 200)},
                       {"placement.translation.x", Float64(allocator, 1.0e9 + 0.25)},
                       {"placement.translation.z", Float64(allocator, 0.125)},
                       {"placement.rotation.x", Float32(allocator, 0.5f)},
                       {"placement.scale.y", Float32(allocator, 3.0f)},
                       {"cell.x", Signed(allocator, ValueKind::Int32, -1)},
                       {"cell.z", Signed(allocator, ValueKind::Int32, 2147483647)},
                       {"debugColor.a", Float32(allocator, -1.0f)},
                       {"bundle.thresholds.w", Float64(allocator, 1.0e-9)},
                       {"bundle.uv.y", Float32(allocator, -1.0f)},
                       {"bundle.uvPrecise.x", Float64(allocator, 1.0e12 + 0.5)},
                       {"bundle.weights.w", Float32(allocator, 4.0f)},
                       {"bundle.orientation.w", Float64(allocator, -0.5)},
                       {"bundle.heading.pitch", Float32(allocator, -0.5f)},
                       {"bundle.headingPrecise.roll", Float64(allocator, 3.0)},
                       {"bundle.size.y", Signed(allocator, ValueKind::Int32, 9)}};
    for (const Entry& entry : entries) {
        REQUIRE(paths.Write(entry.path, entry.value));
    }
}

// FillCoreValues가 쓴 값인지 본다(쓰지 않은 멤버는 생성자 기본값 — Quat w = 1, Transform scale =
// 1).
void CheckCoreValues(const CoreValueFields& value, ObjectHandle) noexcept {
    CHECK(value.position.x == 1.5f && value.position.y == -2.25f && value.position.z == 3.0f);
    CHECK(value.rotation.x == 0.0f && value.rotation.z == 0.75f && value.rotation.w == -0.5f);
    CHECK(value.tint.r == 255 && value.tint.g == 0 && value.tint.a == 200);
    CHECK(value.placement.translation.x == 1.0e9 + 0.25 && value.placement.translation.y == 0.0 &&
          value.placement.translation.z == 0.125);
    CHECK(value.placement.rotation.x == 0.5f && value.placement.rotation.w == 1.0f);
    CHECK(value.placement.scale.x == 1.0f && value.placement.scale.y == 3.0f);
    CHECK(value.cell.x == -1 && value.cell.y == 0 && value.cell.z == 2147483647);
    CHECK(value.debugColor.a == -1.0f);
    CHECK(value.bundle.thresholds.w == 1.0e-9 && value.bundle.uv.y == -1.0f);
    CHECK(value.bundle.uvPrecise.x == 1.0e12 + 0.5 && value.bundle.weights.w == 4.0f);
    CHECK(value.bundle.orientation.w == -0.5 && value.bundle.heading.pitch == -0.5f);
    CHECK(value.bundle.headingPrecise.roll == 3.0 && value.bundle.size.y == 9);
}

// root를 저장해 target에 복원하고 check(복원 객체, 그 handle)로 값을 본 뒤, target에서 다시 저장한
// byte가 처음과 같은지 본다.
template <class T>
void CheckRoundTrip(Allocator& allocator, TypeRegistry& types, ObjectRegistry& source,
                    ObjectRegistry& target, ObjectHandle root,
                    void (*check)(const T&, ObjectHandle) noexcept) noexcept {
    auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(), root);
    REQUIRE(bytes);
    auto loaded =
        LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes.Value().View());
    REQUIRE(loaded);
    {
        auto borrowed = target.Resolve<T>(target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        check(*value.Value(), loaded.Value().Handle());
    }

    auto resaved =
        SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
    REQUIRE(resaved);
    CHECK(SameBytes(bytes.Value().View(), resaved.Value().View()));
}

// 객체의 C++ 값을 본다(check가 const T&를 받는다).
template <class T>
void CheckObject(ObjectRegistry& objects, ObjectHandle handle,
                 void (*check)(const T&, ObjectHandle) noexcept) noexcept {
    auto borrowed = objects.Resolve<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    check(*value.Value(), handle);
}

} // namespace

// 모듈 집계 등록이 클래스 Struct·Array 필드(상속 필드 포함)의 함수표에 path thunk를 싣고 단일 값
// 필드에는 싣지 않으며(등록이 거부한다), 등록된 모든 struct 멤버(Binds 미러·원소 struct 포함)의
// 함수표에 path thunk가 있다.
TEST(GeneratedPath_ModuleRegistrationAttachesPathThunks) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        // 클래스마다 path가 있는 Struct·Array 필드 수, path가 없는 단일 값 필드 수(상속 포함), 그
        // struct 필드·struct 원소 배열이 닿는 struct 멤버 수(중첩 struct를 따라 센다)다.
        //   StructOwner: holder·extra / plain, 멤버 StructHolder 3 + StructMiddle 3 + StructLeaf 3
        //                + extra의 StructLeaf 3 = 12
        //   ArrayOwner: 배열 17 + bundle, 멤버 Vec3 3 + ArrayTag 3 + ArraySlot 3 + ArrayBundle 2
        //               + 그 안 ArraySlot 3 = 14 (ArrayChild는 상속 18 + extra)
        //   CoreValueFields: 7, 멤버 Vec3 3 + Quat 4 + Color 4 + Transform 3·Vec3d 3·Quat 4·Vec3 3
        //                    + IntVec3 3 + LinearColor 4 + CoreValueBundle 8·미러 24 = 63
        //   PathOwner: 6, 멤버 PathEntry 3 × 4(primary·frozen·entries·frozenEntries) = 12
        //   PropertyNode 4·Node 12·EnumFields 3·AssetFields 4는 단일 값 필드뿐이다.
        struct Expected {
            TypeId type;
            usize paths;
            usize scalars;
            usize members;
        };

        const Expected expected[] = {{TypeIdOf<StructOwner>(), 2, 1, 12},
                                     {TypeIdOf<ArrayOwner>(), 18, 0, 14},
                                     {TypeIdOf<ArrayChild>(), 19, 0, 14},
                                     {TypeIdOf<CoreValueFields>(), 7, 0, 63},
                                     {TypeIdOf<PathOwner>(), 6, 0, 12},
                                     {TypeIdOf<PropertyNode>(), 0, 4, 0},
                                     {TypeIdOf<reflection_fixture::Node>(), 0, 12, 0},
                                     {TypeIdOf<reflection_fixture::EnumFields>(), 0, 3, 0},
                                     {TypeIdOf<reflection_fixture::AssetFields>(), 0, 4, 0}};
        for (const Expected& entry : expected) {
            usize paths = 0;
            usize scalars = 0;
            usize members = 0;
            CheckClassThunks(types, entry.type, paths, scalars, members);
            CHECK(paths == entry.paths);
            CHECK(scalars == entry.scalars);
            CHECK(members == entry.members);
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// StructFixture: 3단 중첩 struct의 모든 단일 값 멤버(참조·enum·String·EditorOnly)를 경로로 쓰고
// 읽으며(옛 값 포함), 통째 struct 접근·값 오류·NATIVE 멤버 이름을 거부한다.
TEST(GeneratedPath_StructFixtureMembers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry objects(allocator, types, {0x4750, 1});
            const auto access = objects.OwnerAccess();
            {
                auto lease = types.Acquire(TypeIdOf<StructOwner>());
                auto owner = objects.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
                auto other = objects.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
                REQUIRE(lease && owner && other);
                const Paths paths(objects, lease.Value(), owner.Value().Handle());
                FillStructOwner(paths, allocator, other.Value().Handle());
                CheckObject<StructOwner>(objects, owner.Value().Handle(), &CheckStructOwner);
                {
                    auto borrowed = objects.Resolve<StructOwner>(access, owner.Value().Handle());
                    REQUIRE(borrowed);
                    CHECK(borrowed.Value().Get().Value()->holder.target.Handle() ==
                          other.Value().Handle());
                }

                // 읽기와 옛 값(String·enum·참조).
                FieldValue read(allocator);
                REQUIRE(paths.Read("holder.middle.leaf.label", read));
                CHECK(read.kind == ValueKind::Utf8String &&
                      read.string.View() == StringView("잎 경로"));
                REQUIRE(paths.Read("holder.middle.leaf.group", read));
                CHECK(read.kind == ValueKind::Enum && read.signedInteger == 7 &&
                      read.type == TypeIdOf<TextureGroup>());
                FieldValue previous(allocator);
                FieldValue label = Text(allocator, "바뀐 잎");
                REQUIRE(paths.Write("holder.middle.leaf.label", label, &previous));
                CHECK(previous.string.View() == StringView("잎 경로"));
                FieldValue cleared = Reference(allocator, ObjectHandle{});
                REQUIRE(paths.Write("holder.target", cleared, &previous));
                CHECK(previous.kind == ValueKind::ObjectReference &&
                      previous.reference == other.Value().Handle());
                REQUIRE(paths.Read("holder.target", read));
                CHECK(read.reference == ObjectHandle{});

                // 통째 struct는 읽지도 쓰지도 않는다.
                PropertyPathFailure failure;
                CHECK(ErrorOf(paths.Read("holder.middle.leaf", read, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::AggregateValue);
                CHECK(ErrorOf(paths.Write("holder", label, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::AggregateValue);

                // 값 오류: 정수 폭, 선언되지 않은 enum 값, 다른 enum, 대상 타입이 다른 참조.
                FieldValue wide = Unsigned(allocator, ValueKind::UInt16, 65536);
                CHECK(ErrorOf(paths.Write("holder.middle.note", wide)) == ErrorCode::Overflow);
                FieldValue undeclared = EnumValue(allocator, TypeIdOf<TextureGroup>(), 3);
                CHECK(ErrorOf(paths.Write("holder.middle.leaf.group", undeclared, nullptr,
                                          &failure)) == ErrorCode::InvalidArgument);
                CHECK(failure.reason == PropertyPathError::InvalidValue);
                FieldValue foreign = EnumValue(allocator, TypeIdOf<reflection_fixture::Bias>(), 0);
                CHECK(ErrorOf(paths.Write("holder.middle.leaf.group", foreign, nullptr,
                                          &failure)) == ErrorCode::TypeMismatch);
                CHECK(failure.reason == PropertyPathError::InvalidValue);
                auto node = objects.NewObject<reflection_fixture::Node>(
                    access, TypeIdOf<reflection_fixture::Node>());
                REQUIRE(node);
                FieldValue mistyped = Reference(allocator, node.Value().Handle());
                CHECK(ErrorOf(paths.Write("holder.target", mistyped)) == ErrorCode::TypeMismatch);
                REQUIRE(paths.Read("holder.middle.leaf.group", read));
                CHECK(read.signedInteger == 7);

                // NATIVE 멤버는 스키마에 없다.
                CHECK(ErrorOf(paths.Read("scratch.count", read, &failure)) == ErrorCode::NotFound);
                CHECK(failure.reason == PropertyPathError::UnknownProperty);
                CHECK(ErrorOf(paths.Read("holder.middle.cache", read, &failure)) ==
                      ErrorCode::NotFound);
                CHECK(failure.reason == PropertyPathError::UnknownProperty && failure.step == 2);
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// ArrayFixture: 모든 원소 종류의 Array<E>와 ObjectRefArray(상속 배열 필드·인라인 struct 안
// 컨테이너· 원소 struct 안 배열 멤버 포함)를 경로로 넣고 읽고 바꾸고 빼고 비우며, struct 원소
// 배열(Array<Binds Vec3>·Array<S>·StructRefArray·인라인 struct 안 StructRefArray)은 기본 원소와
// 원소 멤버로 다룬다. MaxElements·값 오류·모양 오류를 거부하고, base lease(ArrayOwner)로 해석한
// 경로(StructRefArray 원소 멤버 쓰기, Array<E> 원소 삽입·읽기)를 파생 객체(ArrayChild)에 쓴다.
TEST(GeneratedPath_ArrayFixtureElements) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry objects(allocator, types, {0x4750, 2});
            const auto access = objects.OwnerAccess();
            {
                auto lease = types.Acquire(TypeIdOf<ArrayChild>());
                auto baseLease = types.Acquire(TypeIdOf<ArrayOwner>());
                auto child = objects.NewObject<ArrayChild>(access, TypeIdOf<ArrayChild>());
                auto other = objects.NewObject<ArrayOwner>(access, TypeIdOf<ArrayOwner>());
                REQUIRE(lease && baseLease && child && other);
                const ObjectHandle self = child.Value().Handle();
                const ObjectHandle peer = other.Value().Handle();
                const Paths paths(objects, lease.Value(), self);
                const TypeId group = TypeIdOf<TextureGroup>();
                const TypeId channel = TypeIdOf<Channels>();
                ArrayCase cases[] = {
                    {"flags", Boolean(allocator, true), Boolean(allocator, false),
                     Boolean(allocator, true)},
                    {"longs", Unsigned(allocator, ValueKind::UInt64, 1),
                     Unsigned(allocator, ValueKind::UInt64, ~uint64{0}),
                     Unsigned(allocator, ValueKind::UInt64, 7)},
                    {"doubles", Float64(allocator, -0.5), Float64(allocator, 2.25),
                     Float64(allocator, 0.0)},
                    {"guids", Identifier(allocator, ValueKind::Guid, Guid{1, 2}),
                     Identifier(allocator, ValueKind::Guid, Guid{}),
                     Identifier(allocator, ValueKind::Guid, Guid{3, 4})},
                    {"assets", Identifier(allocator, ValueKind::AssetId, Guid{9, 9}),
                     Identifier(allocator, ValueKind::AssetId, Guid{8, 8}),
                     Identifier(allocator, ValueKind::AssetId, Guid{7, 7})},
                    {"digests", Digest(allocator, 1), Digest(allocator, 2), Digest(allocator, 3)},
                    {"ints", Signed(allocator, ValueKind::Int32, -7),
                     Signed(allocator, ValueKind::Int32, 2147483647),
                     Signed(allocator, ValueKind::Int32, 0)},
                    {"names", Text(allocator, "first"), Text(allocator, "둘째"),
                     Text(allocator, "a third name long enough to need a heap buffer")},
                    {"groups", EnumValue(allocator, group, 7), EnumValue(allocator, group, 0),
                     EnumValue(allocator, group, 13)},
                    {"channels", EnumValue(allocator, channel, 5), EnumValue(allocator, channel, 0),
                     EnumValue(allocator, channel, 15)},
                    {"softs", Soft(allocator, Guid{5, 5}, "Mesh.Socket"),
                     Soft(allocator, Guid{}, ""), Soft(allocator, Guid{6, 6}, "")},
                    {"anySofts", Soft(allocator, Guid{6, 6}, ""),
                     Soft(allocator, Guid{1, 1}, "Root"), Soft(allocator, Guid{}, "")},
                    {"links", Reference(allocator, peer), Reference(allocator, ObjectHandle{}),
                     Reference(allocator, self)},
                    {"required", Reference(allocator, self), Reference(allocator, peer),
                     Reference(allocator, ObjectHandle{})},
                    {"bundle.refs", Reference(allocator, ObjectHandle{}),
                     Reference(allocator, peer), Reference(allocator, self)},
                    {"extra", Unsigned(allocator, ValueKind::UInt8, 0),
                     Unsigned(allocator, ValueKind::UInt8, 255),
                     Unsigned(allocator, ValueKind::UInt8, 1)}};
                for (const ArrayCase& entry : cases) {
                    ExerciseValueArray(paths, allocator, entry);
                }

                FieldValue x0 = Float32(allocator, 1.5f);
                FieldValue x1 = Float32(allocator, -4.0f);
                ExerciseStructArray(paths, allocator, "points", ".x", x0, x1);
                FieldValue label0 = Text(allocator, "tag zero");
                FieldValue label1 = Text(allocator, "태그 하나");
                ExerciseStructArray(paths, allocator, "tags", ".label", label0, label1);
                FieldValue material0 = Reference(allocator, peer);
                FieldValue material1 = Reference(allocator, self);
                ExerciseStructArray(paths, allocator, "slots", ".material", material0, material1);
                FieldValue name0 = Text(allocator, "b0");
                FieldValue name1 = Text(allocator, "b1");
                ExerciseStructArray(paths, allocator, "bundle.slots", ".name", name0, name1);

                // 원소 struct 안 배열(Array<ArrayTag>의 codes, StructRefArray의 weights, 인라인
                // struct 안 StructRefArray 원소의 weights)도 멤버 thunk로 넣고 읽고 바꾸고(옛 값)
                // 빼고(removed) 비운다.
                REQUIRE(paths.Insert("tags[0]", nullptr));
                REQUIRE(paths.Insert("slots[0]", nullptr));
                REQUIRE(paths.Insert("bundle.slots[0]", nullptr));
                ArrayCase memberCases[] = {{"tags[0].codes",
                                            Unsigned(allocator, ValueKind::UInt16, 1),
                                            Unsigned(allocator, ValueKind::UInt16, 65535),
                                            Unsigned(allocator, ValueKind::UInt16, 0)},
                                           {"slots[0].weights", Float32(allocator, 0.5f),
                                            Float32(allocator, -1.0f), Float32(allocator, 8.0f)},
                                           {"bundle.slots[0].weights", Float32(allocator, 2.0f),
                                            Float32(allocator, 0.0f), Float32(allocator, -3.5f)}};
                for (const ArrayCase& entry : memberCases) {
                    ExerciseValueArray(paths, allocator, entry);
                }

                FieldValue code = Unsigned(allocator, ValueKind::UInt16, 65535);
                FieldValue weight = Float32(allocator, 0.5f);
                REQUIRE(paths.Insert("tags[0].codes[0]", &code));
                REQUIRE(paths.Insert("slots[0].weights[0]", &weight));
                FieldValue read(allocator);
                REQUIRE(paths.Read("tags[0].codes[0]", read));
                CHECK(read.kind == ValueKind::UInt16 && read.unsignedInteger == 65535);
                REQUIRE(paths.Read("slots[0].weights[0]", read));
                CHECK(read.kind == ValueKind::Float32 && read.float32 == 0.5f);
                FieldValue narrow = Unsigned(allocator, ValueKind::UInt16, 65536);
                CHECK(ErrorOf(paths.Insert("tags[0].codes[1]", &narrow)) == ErrorCode::Overflow);
                auto codes = paths.Length("tags[0].codes");
                CHECK(codes && codes.Value() == 1);

                // MaxElements = 4인 names: 상한까지 넣고 다섯째는 CapacityExceeded다.
                for (uint32 index = 0; index < 4; ++index) {
                    FieldValue name = Text(allocator, "n");
                    REQUIRE(paths.Insert(Indexed("names", index).Text(), &name));
                }

                PropertyPathFailure failure;
                FieldValue fifth = Text(allocator, "n");
                CHECK(ErrorOf(paths.Insert("names[4]", &fifth, &failure)) ==
                      ErrorCode::CapacityExceeded);
                CHECK(failure.reason == PropertyPathError::TooManyElements);
                auto names = paths.Length("names");
                CHECK(names && names.Value() == 4);

                // 값 오류: 선언되지 않은 enum 원소, 소프트 참조 null 규칙, 참조 대상 타입, 값 없는
                // 단일 값 원소 삽입.
                FieldValue undeclared = EnumValue(allocator, group, 3);
                CHECK(ErrorOf(paths.Insert("groups[0]", &undeclared, &failure)) ==
                      ErrorCode::InvalidArgument);
                CHECK(failure.reason == PropertyPathError::InvalidValue);
                FieldValue pathOnly = Soft(allocator, Guid{}, "Orphan.Path");
                CHECK(ErrorOf(paths.Insert("softs[0]", &pathOnly, &failure)) ==
                      ErrorCode::InvalidArgument);
                auto structOwner = objects.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
                REQUIRE(structOwner);
                FieldValue mistyped = Reference(allocator, structOwner.Value().Handle());
                CHECK(ErrorOf(paths.Insert("links[0]", &mistyped)) == ErrorCode::TypeMismatch);
                CHECK(ErrorOf(paths.Insert("ints[0]", nullptr, &failure)) ==
                      ErrorCode::InvalidArgument);
                CHECK(failure.reason == PropertyPathError::InvalidValue);

                // 모양 오류: 삽입에 index 없음, 길이에 index 있음, 배열이 아닌 struct 필드의 index,
                // 통째 배열 읽기.
                CHECK(ErrorOf(paths.Insert("ints", &code, &failure)) == ErrorCode::InvalidArgument);
                CHECK(failure.reason == PropertyPathError::IndexRequired);
                CHECK(!paths.Length("ints[0]", &failure));
                CHECK(failure.reason == PropertyPathError::IndexNotAllowed);
                CHECK(ErrorOf(paths.Read("bundle[0]", read, &failure)) == ErrorCode::TypeMismatch);
                CHECK(failure.reason == PropertyPathError::NotArray);
                CHECK(ErrorOf(paths.Read("ints", read, &failure)) == ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::AggregateValue);

                // base lease로 해석한 경로(상속 필드는 파생 스키마에 같은 ID로 있다)를 파생 객체에
                // 쓴다.
                PropertyPathBuffer basePath;
                REQUIRE(
                    ParsePropertyPath(baseLease.Value(), StringView("slots[0].name"), basePath));
                FieldValue slotName = Text(allocator, "base lease");
                REQUIRE(PathWrite(objects, access, self, basePath.View(), slotName));
                REQUIRE(paths.Read("slots[0].name", read));
                CHECK(read.string.View() == StringView("base lease"));
                // 상속 Array<E> 원소도 base lease 경로로 넣고 읽는다.
                REQUIRE(ParsePropertyPath(baseLease.Value(), StringView("ints[0]"), basePath));
                FieldValue baseInt = Signed(allocator, ValueKind::Int32, -11);
                REQUIRE(PathInsertElement(objects, access, self, basePath.View(), &baseInt));
                REQUIRE(PathRead(objects, access, self, basePath.View(), read));
                CHECK(read.kind == ValueKind::Int32 && read.signedInteger == -11);
                auto ints = paths.Length("ints");
                CHECK(ints && ints.Value() == 1);

                // 경로 삽입·쓰기만으로 모든 배열을 채운 값이 C++ 값과 같다.
                const char* const arrays[] = {"names", "tags", "slots", "bundle.slots", "ints"};
                for (const char* field : arrays) {
                    REQUIRE(paths.Clear(field));
                }

                FillArrayChild(paths, allocator, self, peer);
                CheckObject<ArrayChild>(objects, self, &CheckArrayChild);
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// CoreValueFixture: Binds 미러 멤버를 경로로 쓴다 — 미러 이름이 아니라 실제 Core 타입의 멤버
// thunk가 쓰고(placement.translation.x는 Transform → Vector3<double>), 사용자 struct 안의 다른 모듈
// 미러도 같다. Format이 같은 문자열로 돌아오고 통째 미러 struct는 Unsupported다.
TEST(GeneratedPath_CoreValueBindsMembers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry objects(allocator, types, {0x4750, 3});
            const auto access = objects.OwnerAccess();
            {
                auto lease = types.Acquire(TypeIdOf<CoreValueFields>());
                auto fields =
                    objects.NewObject<CoreValueFields>(access, TypeIdOf<CoreValueFields>());
                REQUIRE(lease && fields);
                const Paths paths(objects, lease.Value(), fields.Value().Handle());
                FillCoreValues(paths, allocator);
                CheckObject<CoreValueFields>(objects, fields.Value().Handle(), &CheckCoreValues);

                PropertyPathBuffer path;
                REQUIRE(
                    ParsePropertyPath(lease.Value(), StringView("Placement.Translation.X"), path));
                CHECK(path.count == 3);
                String text(allocator);
                REQUIRE(FormatPropertyPath(lease.Value(), path.View(), text));
                CHECK(text.View() == StringView("placement.translation.x"));
                FieldValue read(allocator);
                REQUIRE(PathRead(objects, access, fields.Value().Handle(), path.View(), read));
                CHECK(read.kind == ValueKind::Float64 && read.float64 == 1.0e9 + 0.25);
                FieldValue previous(allocator);
                FieldValue next = Float64(allocator, -8.0);
                REQUIRE(paths.Write("placement.translation.x", next, &previous));
                CHECK(previous.kind == ValueKind::Float64 && previous.float64 == 1.0e9 + 0.25);
                REQUIRE(paths.Read("placement.translation.x", read));
                CHECK(read.float64 == -8.0);

                PropertyPathFailure failure;
                CHECK(ErrorOf(paths.Read("placement.translation", read, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::AggregateValue);
                FieldValue wide = Unsigned(allocator, ValueKind::UInt8, 256);
                CHECK(ErrorOf(paths.Write("tint.g", wide)) == ErrorCode::Overflow);
                FieldValue wrongKind = Float32(allocator, 1.0f);
                CHECK(ErrorOf(paths.Write("placement.translation.y", wrongKind, nullptr,
                                          &failure)) == ErrorCode::TypeMismatch);
                CHECK(failure.reason == PropertyPathError::InvalidValue);
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 경로로만 쓴 객체(StructOwner·ArrayChild·CoreValueFields)를 그래프로 저장해 다른 Registry에
// 복원하면 필드 값이 같고, 다시 저장한 byte가 처음과 같다.
TEST(GeneratedPath_PathWritesRoundTripThroughGraph) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, types, {0x4750, 4});
            ObjectRegistry target(allocator, types, {0x4750, 5});
            const auto access = source.OwnerAccess();
            {
                auto structLease = types.Acquire(TypeIdOf<StructOwner>());
                auto arrayLease = types.Acquire(TypeIdOf<ArrayChild>());
                auto coreLease = types.Acquire(TypeIdOf<CoreValueFields>());
                REQUIRE(structLease && arrayLease && coreLease);

                auto owner = source.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
                auto ownerTarget = source.NewObject<StructOwner>(access, TypeIdOf<StructOwner>());
                REQUIRE(owner && ownerTarget);
                FillStructOwner(Paths(source, structLease.Value(), owner.Value().Handle()),
                                allocator, ownerTarget.Value().Handle());
                CheckRoundTrip<StructOwner>(allocator, types, source, target,
                                            owner.Value().Handle(), &CheckStructOwner);

                auto child = source.NewObject<ArrayChild>(access, TypeIdOf<ArrayChild>());
                auto peer = source.NewObject<ArrayOwner>(access, TypeIdOf<ArrayOwner>());
                REQUIRE(child && peer);
                FillArrayChild(Paths(source, arrayLease.Value(), child.Value().Handle()), allocator,
                               child.Value().Handle(), peer.Value().Handle());
                CheckRoundTrip<ArrayChild>(allocator, types, source, target, child.Value().Handle(),
                                           &CheckArrayChild);

                auto values =
                    source.NewObject<CoreValueFields>(access, TypeIdOf<CoreValueFields>());
                REQUIRE(values);
                FillCoreValues(Paths(source, coreLease.Value(), values.Value().Handle()),
                               allocator);
                CheckRoundTrip<CoreValueFields>(allocator, types, source, target,
                                                values.Value().Handle(), &CheckCoreValues);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// PathFixture: 문자열 경로는 실효 도구 이름(Name 지정자)을 대소문자 무시로 받고 C++ 이름은 받지
// 않으며(UnknownProperty의 단계·byte 위치), Format은 index가 낀 경로도 도구 이름으로 돌아온다.
// ReadOnly struct 필드는 멤버 쓰기를, ReadOnly 배열(Array<E>·Array<S>·ObjectRefArray)은 원소
// 교체·삽입·삭제·비우기와 struct 원소의 멤버 쓰기를, ReadOnly 멤버는 그 멤버 쓰기를
// 막고(읽기·길이는 된다 — 실패 단계는 전파된 ReadOnly면 쓰려던 멤버), 최상위 ReadOnly 단일 값도
// 같은 사유로 거부한다.
TEST(GeneratedPath_ToolNamesAndReadOnlyFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry objects(allocator, types, {0x4750, 6});
            const auto access = objects.OwnerAccess();
            {
                auto lease = types.Acquire(TypeIdOf<PathOwner>());
                auto owner = objects.NewObject<PathOwner>(access, TypeIdOf<PathOwner>());
                REQUIRE(lease && owner);
                const ObjectHandle handle = owner.Value().Handle();
                const Paths paths(objects, lease.Value(), handle);

                PropertyPathBuffer path;
                REQUIRE(ParsePropertyPath(lease.Value(), StringView("main.TITLE"), path));
                REQUIRE(path.count == 2);
                const usize primary = lease.Value().FieldIndex(path.steps[0].fieldId);
                REQUIRE(primary != TypeLease::NoField);
                CHECK(NameOf(lease.Value().Schema().fields[primary]) == StringView("primary"));
                String text(allocator);
                REQUIRE(FormatPropertyPath(lease.Value(), path.View(), text));
                CHECK(text.View() == StringView("Main.Title"));
                struct Rejected {
                    const char* text;
                    usize step;
                    usize offset;
                };

                const Rejected rejected[] = {{"primary.Title", 0, 0},
                                             {"Main.caption", 1, 5},
                                             {"entries[0].Title", 0, 0},
                                             {"Items[0].caption", 1, 9}};
                for (const Rejected& entry : rejected) {
                    PropertyPathFailure failure;
                    CHECK(ErrorOf(ParsePropertyPath(lease.Value(), StringView(entry.text), path,
                                                    &failure)) == ErrorCode::NotFound);
                    CHECK(failure.reason == PropertyPathError::UnknownProperty);
                    CHECK(failure.step == entry.step && failure.offset == entry.offset);
                }

                FieldValue title = Text(allocator, "제목");
                FieldValue weight = Signed(allocator, ValueKind::Int32, -3);
                FieldValue serial = Unsigned(allocator, ValueKind::UInt32, 1);
                REQUIRE(paths.Write("Main.Title", title));
                REQUIRE(paths.Write("Main.weight", weight));
                PropertyPathFailure failure;
                CHECK(ErrorOf(paths.Write("Main.serial", serial, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 1);
                FieldValue read(allocator);
                REQUIRE(paths.Read("Main.serial", read));
                CHECK(read.kind == ValueKind::UInt32 && read.unsignedInteger == 0);

                // 바깥 ReadOnly struct 필드는 모든 멤버 쓰기를 막는다(전파된 ReadOnly — 실패 단계는
                // 쓰려던 멤버다).
                CHECK(ErrorOf(paths.Write("frozen.weight", weight, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 1);
                CHECK(ErrorOf(paths.Write("frozen.Title", title, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 1);
                REQUIRE(paths.Read("frozen.weight", read));
                CHECK(read.kind == ValueKind::Int32 && read.signedInteger == 0);

                // ReadOnly 배열: 원소 교체·삽입·삭제·비우기를 막고 읽기·길이는 된다.
                {
                    auto borrowed = objects.Resolve<PathOwner>(access, handle);
                    REQUIRE(borrowed);
                    REQUIRE(borrowed.Value().Get().Value()->locked.TryPushBack(uint32{5}));
                }

                REQUIRE(paths.Read("locked[0]", read));
                CHECK(read.kind == ValueKind::UInt32 && read.unsignedInteger == 5);
                FieldValue element = Unsigned(allocator, ValueKind::UInt32, 6);
                CHECK(ErrorOf(paths.Write("locked[0]", element, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Insert("locked[1]", &element, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Remove("locked[0]", nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Clear("locked", &failure)) == ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                auto locked = paths.Length("locked");
                CHECK(locked && locked.Value() == 1);

                // ReadOnly struct 원소 배열과 ReadOnly 참조 배열: 원소 삽입·삭제·비우기·교체와 원소
                // 멤버 쓰기(전파된 ReadOnly — 실패 단계는 멤버)를 막고 읽기·길이는 된다.
                {
                    auto borrowed = objects.Resolve<PathOwner>(access, handle);
                    REQUIRE(borrowed);
                    PathOwner* value = borrowed.Value().Get().Value();
                    REQUIRE(
                        value->frozenEntries.TryEmplaceBack(value->frozenEntries.GetAllocator()));
                    REQUIRE(
                        objects.InsertReferenceAt(access, handle, value->frozenLinks, 0, handle));
                }

                CHECK(ErrorOf(paths.Insert("frozenEntries[1]", nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Remove("frozenEntries[0]", nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Clear("frozenEntries", &failure)) == ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Write("frozenEntries[0].weight", weight, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 1);
                CHECK(ErrorOf(paths.Write("frozenEntries[0].Title", title, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 1);
                REQUIRE(paths.Read("frozenEntries[0].weight", read));
                CHECK(read.kind == ValueKind::Int32 && read.signedInteger == 0);
                FieldValue cleared = Reference(allocator, ObjectHandle{});
                FieldValue self = Reference(allocator, handle);
                CHECK(ErrorOf(paths.Write("frozenLinks[0]", cleared, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Insert("frozenLinks[1]", &self, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Remove("frozenLinks[0]", nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                CHECK(ErrorOf(paths.Clear("frozenLinks", &failure)) == ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                REQUIRE(paths.Read("frozenLinks[0]", read));
                CHECK(read.kind == ValueKind::ObjectReference && read.reference == handle);
                auto frozenEntries = paths.Length("frozenEntries");
                auto frozenLinks = paths.Length("frozenLinks");
                CHECK(frozenEntries && frozenEntries.Value() == 1);
                CHECK(frozenLinks && frozenLinks.Value() == 1);

                // 도구 이름을 바꾼 struct 원소 배열과 그 원소의 ReadOnly 멤버. index가 낀 경로도
                // Format이 도구 이름으로 돌아온다.
                REQUIRE(paths.Insert("Items[0]", nullptr));
                FieldValue caption = Text(allocator, "원소 제목");
                REQUIRE(paths.Write("items[0].title", caption));
                CHECK(ErrorOf(paths.Write("Items[0].serial", serial, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 1);
                REQUIRE(ParsePropertyPath(lease.Value(), StringView("items[0].title"), path));
                REQUIRE(FormatPropertyPath(lease.Value(), path.View(), text));
                CHECK(text.View() == StringView("Items[0].Title"));
                {
                    auto borrowed = objects.Resolve<PathOwner>(access, handle);
                    REQUIRE(borrowed);
                    const PathOwner& value = *borrowed.Value().Get().Value();
                    CHECK(value.primary.caption.View() == StringView("제목"));
                    CHECK(value.primary.weight == -3 && value.primary.serial == 0);
                    CHECK(value.frozen.weight == 0 && value.frozen.caption.IsEmpty());
                    CHECK(value.locked.Size() == 1 && value.locked[0] == 5);
                    CHECK(value.frozenEntries.Size() == 1 && value.frozenEntries[0].weight == 0 &&
                          value.frozenEntries[0].caption.IsEmpty());
                    CHECK(value.frozenLinks.Size() == 1 && value.frozenLinks[0] == handle);
                    CHECK(value.entries.Size() == 1 &&
                          value.entries[0].caption.View() == StringView("원소 제목") &&
                          value.entries[0].serial == 0);
                }

                // 최상위 ReadOnly 단일 값(등록된 write)도 ReadOnly 사유로 거부한다.
                auto nodeLease = types.Acquire(TypeIdOf<PropertyNode>());
                auto node = objects.NewObject<PropertyNode>(access, TypeIdOf<PropertyNode>());
                REQUIRE(nodeLease && node);
                const Paths nodePaths(objects, nodeLease.Value(), node.Value().Handle());
                CHECK(ErrorOf(nodePaths.Write("label", title, nullptr, &failure)) ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 0);
                REQUIRE(nodePaths.Read("label", read));
                CHECK(read.kind == ValueKind::Utf8String && read.string.IsEmpty());
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}
