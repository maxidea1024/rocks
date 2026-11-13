#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <Object/GeneratedPathAccess.h>
#include <Object/PropertyPath.h>
#include <TestSupport/Test.h>

// 문자열 경로 앞단(ParsePropertyPath·FormatPropertyPath, B6a) 시험이다(Docs/Design/
// ReflectionFoundations.ko.md 7.1절 "문자열 앞단"·"시험"): 문법 표, ASCII 대소문자 무시, toolName이
// 있을 때 C++ 이름 거부, 상속 필드 이름, UnknownProperty의 단계·byte 위치, NotArray·NotStruct,
// 17단계 한도(18단계 TooDeep), Format 왕복, base lease로 해석한 경로를 파생 객체에 쓰기.
// GeneratedType 특수화는 B6b 생성 코드와 같은 모양으로 손으로 썼다.
using namespace rocks;

namespace path_parse {

// 참조 없는 struct다. weight는 도구 이름 "Mass"로 노출한다(C++ 이름은 받지 않는다).
struct Part {
    explicit Part(Allocator& allocator) noexcept : label(allocator) {
    }

    uint32 weight = 0;
    String label;
};

class Base : public Object {
public:
    explicit Base(Allocator& allocator) noexcept
        : part(allocator), parts(allocator), numbers(allocator) {
    }

    uint32 baseValue = 0;
    Part part;
    Array<Part> parts;
    Array<uint32> numbers;
};

class Derived final : public Base {
public:
    using Base::Base;

    uint32 extra = 0;
};

// 깊이 사슬이다: Deep<0>은 정수 하나, Deep<N>은 Deep<N − 1> 하나(깊이 N + 1).
template <uint32 N>
struct Deep {
    Deep<N - 1> inner;
};

template <>
struct Deep<0> {
    uint32 leaf = 0;
};

constexpr uint32 DeepTop = MaxStructDepth - 1;

struct DeepHolder final : Object {
    Deep<DeepTop> top;
};

constexpr TypeId PartId{0x505053, 1};
constexpr TypeId BaseId{0x505043, 1};
constexpr TypeId DerivedId{0x505043, 2};
constexpr TypeId DeepHolderId{0x505043, 3};
constexpr Guid WeightId{0x50504d, 1};
constexpr Guid LabelId{0x50504d, 2};
constexpr Guid BaseValueId{0x505046, 1};
constexpr Guid PartFieldId{0x505046, 2};
constexpr Guid PartsId{0x505046, 3};
constexpr Guid NumbersId{0x505046, 4};
constexpr Guid HiddenId{0x505046, 5};
constexpr Guid ExtraId{0x505046, 6};
constexpr Guid TopId{0x505046, 7};
constexpr Guid DeepInnerId{0x505044, 1};
constexpr Guid DeepLeafId{0x505044, 2};

constexpr TypeId DeepId(uint32 level) noexcept {
    return TypeId{0x505054, level + 1};
}

Span<const char> Text(const char* text) noexcept {
    usize bytes = 0;
    while (text[bytes] != '\0') {
        ++bytes;
    }

    return Span<const char>(text, bytes);
}

FieldSchema Scalar(Guid id, const char* name, ValueKind kind, TypeId type = {},
                   const char* toolName = "") noexcept {
    FieldSchema field;
    field.id = id;
    field.name = Text(name);
    field.value = ValueType{kind, type};
    field.toolName = Text(toolName);
    return field;
}

FieldSchema ArrayOf(Guid id, const char* name, ValueKind element, TypeId type = {}) noexcept {
    FieldSchema field;
    field.id = id;
    field.name = Text(name);
    field.value = ValueType{ValueKind::Array, {}};
    field.element = ValueType{element, type};
    field.maxElements = DefaultMaxElements;
    return field;
}

template <auto Member, ValueKind Target>
RuntimeStructFieldOps MemberOps(Guid id) noexcept {
    return detail::StructFieldOps<Member>(id, &detail::PathStructMember<Member, Target>);
}

// 단일 값 클래스 필드(read·write·writeStaged — path 없음). Owner는 함수표를 가진 실제 타입이다.
template <class Owner, auto Member>
RuntimeFieldOps ValueField(Guid id) noexcept {
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.read = +[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   FieldValue& out) noexcept {
        return detail::ReadGeneratedField<Owner>(registry, access, object, Member, out);
    };
    ops.write = +[](ObjectRegistry& registry, const ObjectAccessContext& access,
                    ObjectHandle object, const FieldValue& value) noexcept {
        return detail::WriteGeneratedField<Owner>(registry, access, object, Member, value);
    };
    ops.writeStaged = +[](ConstructionContext& construction, ObjectHandle object,
                          const FieldValue& value) noexcept {
        return detail::WriteGeneratedStagedField<Owner>(construction, object, Member, value);
    };
    return ops;
}

// struct·배열 클래스 필드(B6b 생성 코드 모양 — codec, valueNativeType, path 람다).
template <class Owner, auto Member, ValueKind Target>
RuntimeFieldOps PathField(Guid id, const void* valueNative = nullptr) noexcept {
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.encode = +[](const RuntimeFieldOps&, ObjectRegistry& registry,
                     const ObjectAccessContext& access, ObjectHandle object, ArchiveWriter& out,
                     FieldEncodeContext& context) noexcept {
        return detail::EncodeGeneratedField<Owner>(registry, access, object, Member, out, context);
    };
    ops.decode = +[](const RuntimeFieldOps&, ConstructionContext* construction, ObjectHandle object,
                     const FieldView& field, FieldDecodeContext& context) noexcept {
        return detail::DecodeGeneratedField<Owner>(construction, object, Member, field, context);
    };
    ops.valueNativeType = valueNative;
    ops.path = +[](const RuntimeFieldOps&, detail::PathCursor& cursor) noexcept -> Result<void> {
        return detail::PathClassField<Owner, Target>(cursor, Member);
    };
    return ops;
}

// 클래스 스키마(파생은 상속 필드를 같은 ID로 담는다).
const FieldSchema BaseFields[] = {Scalar(BaseValueId, "baseValue", ValueKind::UInt32),
                                  Scalar(PartFieldId, "part", ValueKind::Struct, PartId),
                                  ArrayOf(PartsId, "parts", ValueKind::Struct, PartId),
                                  ArrayOf(NumbersId, "numbers", ValueKind::UInt32),
                                  Scalar(HiddenId, "hidden", ValueKind::Opaque)};

const FieldSchema DerivedFields[] = {Scalar(BaseValueId, "baseValue", ValueKind::UInt32),
                                     Scalar(PartFieldId, "part", ValueKind::Struct, PartId),
                                     ArrayOf(PartsId, "parts", ValueKind::Struct, PartId),
                                     ArrayOf(NumbersId, "numbers", ValueKind::UInt32),
                                     Scalar(HiddenId, "hidden", ValueKind::Opaque),
                                     Scalar(ExtraId, "extra", ValueKind::UInt32)};

} // namespace path_parse

namespace rocks {

template <>
struct GeneratedType<path_parse::Part> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_parse::PartId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_parse;
        static const FieldSchema fields[] = {
            Scalar(WeightId, "weight", ValueKind::UInt32, {}, "Mass"),
            Scalar(LabelId, "label", ValueKind::Utf8String)};
        static const TypeSchema schema{Id(), 1, fields, {"Part", 4}};
        return schema;
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_parse;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Part::weight, ValueKind::UInt32>(WeightId),
            MemberOps<&Part::label, ValueKind::Utf8String>(LabelId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Part>();
        ops.fields = fields;
        return ops;
    }
};

template <uint32 N>
struct GeneratedType<path_parse::Deep<N>> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_parse::DeepId(N);
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_parse;
        static const FieldSchema fields[] = {
            Scalar(DeepInnerId, "inner", ValueKind::Struct, DeepId(N - 1))};
        static const TypeSchema schema{Id(), 1, fields, {"Deep", 4}};
        return schema;
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_parse;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Deep<N>::inner, ValueKind::Struct>(DeepInnerId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Deep<N>>();
        ops.fields = fields;
        return ops;
    }
};

template <>
struct GeneratedType<path_parse::Deep<0>> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_parse::DeepId(0);
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_parse;
        static const FieldSchema fields[] = {Scalar(DeepLeafId, "leaf", ValueKind::UInt32)};
        static const TypeSchema schema{Id(), 1, fields, {"Deep", 4}};
        return schema;
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_parse;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Deep<0>::leaf, ValueKind::UInt32>(DeepLeafId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Deep<0>>();
        ops.fields = fields;
        return ops;
    }
};

template <>
struct GeneratedType<path_parse::Base> {
    static constexpr TypeId Id() noexcept {
        return path_parse::BaseId;
    }

    static const TypeSchema& Schema() noexcept {
        static const TypeSchema schema{Id(), 1, path_parse::BaseFields, {"Base", 4}};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace path_parse;
        static const RuntimeFieldOps fields[] = {
            ValueField<Base, &Base::baseValue>(BaseValueId),
            PathField<Base, &Base::part, ValueKind::Struct>(PartFieldId, NativeTypeKey<Part>()),
            PathField<Base, &Base::parts, ValueKind::Struct>(PartsId, NativeTypeKey<Part>()),
            PathField<Base, &Base::numbers, ValueKind::UInt32>(NumbersId)};
        auto ops = MakeRuntimeTypeOps<Base>();
        ops.fields = fields;
        return ops;
    }
};

template <>
struct GeneratedType<path_parse::Derived> {
    static constexpr TypeId Id() noexcept {
        return path_parse::DerivedId;
    }

    static const TypeSchema& Schema() noexcept {
        static const TypeSchema schema{
            Id(), 1, path_parse::DerivedFields, {"Derived", 7}, path_parse::BaseId};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace path_parse;
        // 상속 필드도 파생 타입을 Owner로 낸다(생성 코드와 같다).
        static const RuntimeFieldOps fields[] = {
            ValueField<Derived, &Base::baseValue>(BaseValueId),
            PathField<Derived, &Base::part, ValueKind::Struct>(PartFieldId, NativeTypeKey<Part>()),
            PathField<Derived, &Base::parts, ValueKind::Struct>(PartsId, NativeTypeKey<Part>()),
            PathField<Derived, &Base::numbers, ValueKind::UInt32>(NumbersId),
            ValueField<Derived, &Derived::extra>(ExtraId)};
        auto ops = MakeRuntimeTypeOps<Derived>();
        ops.fields = fields;
        ops.nativeBaseType = NativeTypeKey<Base>();
        return ops;
    }
};

template <>
struct GeneratedType<path_parse::DeepHolder> {
    static constexpr TypeId Id() noexcept {
        return path_parse::DeepHolderId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_parse;
        static const FieldSchema fields[] = {
            Scalar(TopId, "top", ValueKind::Struct, DeepId(DeepTop))};
        static const TypeSchema schema{Id(), 1, fields, {"DeepHolder", 10}};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace path_parse;
        static const RuntimeFieldOps fields[] = {
            PathField<DeepHolder, &DeepHolder::top, ValueKind::Struct>(
                TopId, NativeTypeKey<Deep<DeepTop>>())};
        auto ops = MakeRuntimeTypeOps<DeepHolder>();
        ops.fields = fields;
        return ops;
    }
};

} // namespace rocks

namespace {

using namespace path_parse;

// 깊이 사슬을 얕은 것부터 등록한다(멤버 struct가 먼저 있어야 한다).
template <uint32 N>
struct DeepRegistrar {
    static bool Run(TypeRegistry& types) noexcept {
        return DeepRegistrar<N - 1>::Run(types) &&
               static_cast<bool>(types.RegisterStruct(GeneratedType<Deep<N>>::Schema(),
                                                      GeneratedType<Deep<N>>::Ops()));
    }
};

template <>
struct DeepRegistrar<0> {
    static bool Run(TypeRegistry& types) noexcept {
        return static_cast<bool>(
            types.RegisterStruct(GeneratedType<Deep<0>>::Schema(), GeneratedType<Deep<0>>::Ops()));
    }
};

bool RegisterAll(TypeRegistry& types) noexcept {
    return types.RegisterStruct(GeneratedType<Part>::Schema(), GeneratedType<Part>::Ops()) &&
           types.Register(GeneratedType<Base>::Schema(), GeneratedType<Base>::Ops()) &&
           types.Register(GeneratedType<Derived>::Schema(), GeneratedType<Derived>::Ops()) &&
           DeepRegistrar<DeepTop>::Run(types) &&
           types.Register(GeneratedType<DeepHolder>::Schema(), GeneratedType<DeepHolder>::Ops());
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

struct Expected {
    const char* text;
    ErrorCode error;
    PropertyPathError reason;
    usize step;
    usize offset;
};

} // namespace

TEST(PropertyPathParse_GrammarTable) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        auto lease = types.Acquire(BaseId);
        REQUIRE(lease);
        const Expected cases[] = {
            {"", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 0},
            {".", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 0},
            {"part.", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 1, 5},
            // 끝 `.`: struct 뒤면 이름이 없어 Syntax, struct가 아닌 값 뒤면 NotStruct가 먼저다.
            {"baseValue.", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 0, 9},
            {"numbers[0].", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 0, 10},
            {"[]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 0},
            {"[0]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 0},
            {"numbers[]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 8},
            {"numbers[01]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 8},
            {"numbers[00]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 8},
            {"numbers[4294967296]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 8},
            {"numbers[-1]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 8},
            {"numbers[+1]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 8},
            {"numbers[ 1]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 8},
            {"numbers[1 ]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 9},
            {"numbers[1", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 9},
            {" part", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 0},
            {"part ", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 1, 4},
            {"part .label", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 1, 4},
            {"part. label", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 1, 5},
            {"part..label", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 1, 5},
            {"1part", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 0},
            {"\xc3\xa4part", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 0, 0},
            {"part\xc3\xa4", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 1, 4},
            {"part/label", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 1, 4},
            {"bogus", ErrorCode::NotFound, PropertyPathError::UnknownProperty, 0, 0},
            {"part.bogus", ErrorCode::NotFound, PropertyPathError::UnknownProperty, 1, 5},
            {"parts[3].nope", ErrorCode::NotFound, PropertyPathError::UnknownProperty, 1, 9},
            {"hidden", ErrorCode::NotFound, PropertyPathError::UnknownProperty, 0, 0},
            {"baseValue[0]", ErrorCode::TypeMismatch, PropertyPathError::NotArray, 0, 9},
            {"part[0]", ErrorCode::TypeMismatch, PropertyPathError::NotArray, 0, 4},
            {"numbers[0][1]", ErrorCode::TypeMismatch, PropertyPathError::NotArray, 0, 10},
            {"part.label[0]", ErrorCode::TypeMismatch, PropertyPathError::NotArray, 1, 10},
            {"baseValue.x", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 0, 9},
            {"parts.label", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 0, 5},
            {"numbers[0].x", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 0, 10},
            {"part.label.x", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 1, 10}};
        for (const Expected& entry : cases) {
            PropertyPathBuffer out;
            out.count = 99;
            PropertyPathFailure failure;
            const auto result = ParsePropertyPath(lease.Value(), entry.text, out, &failure);
            CHECK(ErrorOf(result) == entry.error);
            CHECK(failure.reason == entry.reason);
            CHECK(failure.step == entry.step);
            CHECK(failure.offset == entry.offset);
            // 실패하면 out은 바뀌지 않는다.
            CHECK(out.count == 99);
        }

        // 정규 index의 경계: 0과 uint32 최댓값.
        PropertyPathBuffer out;
        REQUIRE(ParsePropertyPath(lease.Value(), "numbers[0]", out));
        CHECK(out.count == 1 && out.steps[0].indexed && out.steps[0].index == 0);
        REQUIRE(ParsePropertyPath(lease.Value(), "numbers[4294967295]", out));
        CHECK(out.steps[0].index == 4294967295u && out.steps[0].fieldId == NumbersId);
        REQUIRE(ParsePropertyPath(lease.Value(), "parts[10].label", out));
        CHECK(out.count == 2 && out.steps[0].fieldId == PartsId && out.steps[0].index == 10 &&
              out.steps[0].indexed && out.steps[1].fieldId == LabelId && !out.steps[1].indexed &&
              out.steps[1].index == 0);
        lease.Value().Reset();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathParse_ToolNamesCaseAndInheritance) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        auto base = types.Acquire(BaseId);
        auto derived = types.Acquire(DerivedId);
        auto part = types.AcquireValueType(PartId, TypeKind::Struct);
        REQUIRE(base && derived && part);
        PropertyPathBuffer out;
        PropertyPathFailure failure;
        // ASCII 대소문자 무시.
        REQUIRE(ParsePropertyPath(base.Value(), "PART.LABEL", out));
        CHECK(out.count == 2 && out.steps[0].fieldId == PartFieldId &&
              out.steps[1].fieldId == LabelId);
        REQUIRE(ParsePropertyPath(base.Value(), "Parts[2].mASS", out));
        CHECK(out.steps[1].fieldId == WeightId);
        // toolName이 있으면 C++ 이름은 받지 않는다.
        CHECK(ErrorOf(ParsePropertyPath(base.Value(), "part.weight", out, &failure)) ==
              ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::UnknownProperty && failure.step == 1 &&
              failure.offset == 5);
        // 상속 필드 이름(파생 lease)과 파생 전용 필드. base lease는 파생 필드를 모른다.
        REQUIRE(ParsePropertyPath(derived.Value(), "baseValue", out));
        CHECK(out.steps[0].fieldId == BaseValueId);
        REQUIRE(ParsePropertyPath(derived.Value(), "Extra", out));
        CHECK(out.steps[0].fieldId == ExtraId);
        CHECK(ErrorOf(ParsePropertyPath(base.Value(), "extra", out, &failure)) ==
              ErrorCode::NotFound);
        // struct lease로도 해석한다. enum lease·빈 lease는 InvalidArgument.
        REQUIRE(ParsePropertyPath(part.Value(), "mass", out));
        CHECK(out.count == 1 && out.steps[0].fieldId == WeightId);
        TypeLease empty;
        CHECK(ErrorOf(ParsePropertyPath(empty, "mass", out)) == ErrorCode::InvalidArgument);
        auto group = GeneratedType<reflection_fixture::TextureGroup>::Register(types);
        REQUIRE(group);
        auto enumeration =
            types.AcquireValueType(TypeIdOf<reflection_fixture::TextureGroup>(), TypeKind::Enum);
        REQUIRE(enumeration);
        CHECK(ErrorOf(ParsePropertyPath(enumeration.Value(), "World", out)) ==
              ErrorCode::InvalidArgument);
        String text(allocator);
        CHECK(ErrorOf(FormatPropertyPath(enumeration.Value(), out.View(), text)) ==
              ErrorCode::InvalidArgument);
        enumeration.Value().Reset();
        base.Value().Reset();
        derived.Value().Reset();
        part.Value().Reset();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathParse_FormatRoundTripAndBaseLeaseOnDerivedObject) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        auto base = types.Acquire(BaseId);
        REQUIRE(base);
        // Format은 실효 도구 이름으로 쓰고 Parse와 왕복한다.
        const char* inputs[] = {"PARTS[12].MASS", "part.Label", "numbers[0]", "BaseValue"};
        const char* canonical[] = {"parts[12].Mass", "part.label", "numbers[0]", "baseValue"};
        for (usize index = 0; index < 4; ++index) {
            PropertyPathBuffer path;
            REQUIRE(ParsePropertyPath(base.Value(), inputs[index], path));
            String text(allocator);
            REQUIRE(FormatPropertyPath(base.Value(), path.View(), text));
            CHECK(text.View() == StringView(canonical[index]));
            PropertyPathBuffer again;
            REQUIRE(ParsePropertyPath(base.Value(), text.View(), again));
            CHECK(SamePropertyPath(path.View(), again.View()));
        }

        // Format 오류: 없는 ID·정규형 위반·모양 불일치·Opaque, 실패하면 out은 그대로다.
        String text(allocator);
        REQUIRE(text.TryAssign("keep"));
        const PathStep unknown[] = {{Guid{0x123, 4}}};
        CHECK(ErrorOf(FormatPropertyPath(base.Value(), PropertyPath{unknown}, text)) ==
              ErrorCode::NotFound);
        const PathStep nonCanonical[] = {{NumbersId, 2, false}};
        CHECK(ErrorOf(FormatPropertyPath(base.Value(), PropertyPath{nonCanonical}, text)) ==
              ErrorCode::InvalidArgument);
        const PathStep shape[] = {{BaseValueId, 1, true}};
        CHECK(ErrorOf(FormatPropertyPath(base.Value(), PropertyPath{shape}, text)) ==
              ErrorCode::TypeMismatch);
        const PathStep afterArray[] = {{PartsId}, {LabelId}};
        CHECK(ErrorOf(FormatPropertyPath(base.Value(), PropertyPath{afterArray}, text)) ==
              ErrorCode::TypeMismatch);
        const PathStep hidden[] = {{HiddenId}};
        CHECK(ErrorOf(FormatPropertyPath(base.Value(), PropertyPath{hidden}, text)) ==
              ErrorCode::Unsupported);
        CHECK(ErrorOf(FormatPropertyPath(base.Value(), PropertyPath{}, text)) ==
              ErrorCode::InvalidArgument);
        CHECK(text.View() == StringView("keep"));

        // base lease로 해석한 경로를 파생 객체에 쓴다(파생 스키마가 상속 필드를 같은 ID로 담는다).
        ObjectRegistry objects(allocator, types, {0x5050, 1});
        const auto access = objects.OwnerAccess();
        auto derived = objects.NewObject<Derived>(access, DerivedId);
        REQUIRE(derived);
        PropertyPathBuffer label;
        REQUIRE(ParsePropertyPath(base.Value(), "part.label", label));
        FieldValue value(allocator);
        value.kind = ValueKind::Utf8String;
        REQUIRE(value.string.TryAssign("written through a base path"));
        REQUIRE(PathWrite(objects, access, derived.Value().Handle(), label.View(), value));
        PropertyPathBuffer mass;
        REQUIRE(ParsePropertyPath(base.Value(), "parts[0].mass", mass));
        PropertyPathBuffer element;
        REQUIRE(ParsePropertyPath(base.Value(), "parts[0]", element));
        REQUIRE(
            PathInsertElement(objects, access, derived.Value().Handle(), element.View(), nullptr));
        FieldValue weight(allocator);
        weight.kind = ValueKind::UInt32;
        weight.unsignedInteger = 12;
        REQUIRE(PathWrite(objects, access, derived.Value().Handle(), mass.View(), weight));
        {
            auto borrowed = objects.Resolve<Derived>(access, derived.Value().Handle());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            CHECK(pointer.Value()->part.label.View() == StringView("written through a base path"));
            CHECK(pointer.Value()->parts.Size() == 1 && pointer.Value()->parts[0].weight == 12);
        }

        // 최상위 단일 값(상속 필드)도 base lease 경로로 쓴다.
        PropertyPathBuffer baseValue;
        REQUIRE(ParsePropertyPath(base.Value(), "baseValue", baseValue));
        REQUIRE(PathWrite(objects, access, derived.Value().Handle(), baseValue.View(), weight));
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, access, derived.Value().Handle(), baseValue.View(), read));
        CHECK(read.unsignedInteger == 12);
        derived.Value().Reset();
        REQUIRE(objects.Collect(access));
        base.Value().Reset();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPathParse_SeventeenStepsAndTooDeep) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterAll(types));
        auto lease = types.Acquire(DeepHolderId);
        REQUIRE(lease);
        // top + inner 15 + leaf = 17단계(MaxPropertyPathSteps)는 받는다.
        char text[256];
        usize length = 0;
        const auto append = [&](const char* part) noexcept {
            for (usize index = 0; part[index] != '\0'; ++index) {
                text[length++] = part[index];
            }

            text[length] = '\0';
        };
        append("top");
        for (uint32 index = 0; index < DeepTop; ++index) {
            append(".inner");
        }

        append(".leaf");
        PropertyPathBuffer path;
        REQUIRE(ParsePropertyPath(lease.Value(), StringView(text, length), path));
        CHECK(path.count == MaxPropertyPathSteps);
        String formatted(allocator);
        REQUIRE(FormatPropertyPath(lease.Value(), path.View(), formatted));
        CHECK(formatted.View() == StringView(text, length));
        // 18단계는 TooDeep이다(17단계 뒤의 `.` 위치).
        const usize dot = length;
        append(".x");
        PropertyPathBuffer deeper;
        PropertyPathFailure failure;
        CHECK(ErrorOf(ParsePropertyPath(lease.Value(), StringView(text, length), deeper,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::TooDeep &&
              failure.step == MaxPropertyPathSteps && failure.offset == dot);

        // 17단계 경로로 가장 깊은 잎을 쓰고 읽는다(Descend 16번).
        ObjectRegistry objects(allocator, types, {0x5050, 2});
        const auto access = objects.OwnerAccess();
        auto holder = objects.NewObject<DeepHolder>(access, DeepHolderId);
        REQUIRE(holder);
        FieldValue value(allocator);
        value.kind = ValueKind::UInt32;
        value.unsignedInteger = 1717;
        REQUIRE(PathWrite(objects, access, holder.Value().Handle(), path.View(), value));
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, access, holder.Value().Handle(), path.View(), read));
        CHECK(read.unsignedInteger == 1717);
        PathStep tooMany[MaxPropertyPathSteps + 1];
        for (usize index = 0; index < MaxPropertyPathSteps; ++index) {
            tooMany[index] = path.steps[index];
        }

        tooMany[MaxPropertyPathSteps] = PathStep{DeepLeafId};
        CHECK(
            ErrorOf(PathRead(objects, access, holder.Value().Handle(),
                             PropertyPath{Span<const PathStep>(tooMany, MaxPropertyPathSteps + 1)},
                             read, &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::TooDeep);
        holder.Value().Reset();
        REQUIRE(objects.Collect(access));
        lease.Value().Reset();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
