#include "ReflectionFixture.generated.h"
#include <Tests/Support/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>

using namespace rocks;
using reflection_fixture::Base;
using reflection_fixture::Node;

namespace {
constexpr Guid NextId{0xad10000000000000ull, 0x22};

Result<TypeRegistrationToken> RegisterNodeAlias(TypeRegistry& types, TypeId id,
                                                TypeId reference) noexcept {
    auto schema = GeneratedType<Node>::Schema();
    if (schema.fields.Size() > 32) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::CapacityExceeded);
    }
    FieldSchema fields[32];
    bool found = false;
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        fields[index] = schema.fields[index];
        if (fields[index].id == NextId) {
            fields[index].referencedType = reference;
            found = true;
        }
    }
    if (!found) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::NotFound);
    }
    schema.fields = {fields, schema.fields.Size()};
    schema.id = id;
    return types.Register(schema, GeneratedType<Node>::Ops());
}

const RuntimeFieldOps* Next(const TypeLease& lease) noexcept {
    for (usize index = 0; index < lease.Ops().fields.Size(); ++index) {
        if (lease.Ops().fields[index].fieldId == NextId) {
            return &lease.Ops().fields[index];
        }
    }
    return nullptr;
}

struct CallbackUnregistrationProbe {
    TypeRegistry* source;
    TypeRegistry* target;
    TypeRegistrationToken sourceToken;
    TypeRegistrationToken targetToken;
    ErrorCode sourceResult = ErrorCode::None;
    ErrorCode targetResult = ErrorCode::None;
};

CallbackUnregistrationProbe* activeProbe = nullptr;

Result<void> ProbeReferenceTypeLifetimes(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                                         const FieldValue&) noexcept {
    if (!activeProbe) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    activeProbe->sourceResult = activeProbe->source->Unregister(activeProbe->sourceToken).Error();
    activeProbe->targetResult = activeProbe->target->Unregister(activeProbe->targetToken).Error();
    return Result<void>::Success();
}
} // namespace

TEST(ReferenceWriteChecksBothSchemasAcrossRegistriesAndNativeAliases) {
    SystemAllocator allocator;
    {
        TypeRegistry sourceTypes(allocator);
        TypeRegistry targetTypes(allocator);
        REQUIRE(GeneratedType<Base>::Register(sourceTypes));
        REQUIRE(GeneratedType<Node>::Register(sourceTypes));
        REQUIRE(GeneratedType<Base>::Register(targetTypes));
        REQUIRE(GeneratedType<Node>::Register(targetTypes));
        const TypeId nodeAlias{0xc0dec0de, 0x101};
        const TypeId badAlias{0xc0dec0de, 0x102};
        const TypeId wildcardAlias{0xc0dec0de, 0x103};
        const TypeId sourceWildcard{0xc0dec0de, 0x104};
        REQUIRE(RegisterNodeAlias(targetTypes, nodeAlias, nodeAlias));
        REQUIRE(RegisterNodeAlias(targetTypes, badAlias, GeneratedType<Base>::Id()));
        REQUIRE(RegisterNodeAlias(targetTypes, wildcardAlias, {}));
        REQUIRE(RegisterNodeAlias(sourceTypes, sourceWildcard, {}));
        ObjectRegistry target(allocator, targetTypes, {721, 1});
        auto compatible = target.NewObject<Node>(target.OwnerAccess(), nodeAlias);
        auto incompatible = target.NewObject<Node>(target.OwnerAccess(), badAlias);
        auto generic = target.NewObject<Node>(target.OwnerAccess(), wildcardAlias);
        auto child = target.NewObject<Node>(target.OwnerAccess(), GeneratedType<Node>::Id());
        auto base = target.NewObject<Base>(target.OwnerAccess(), GeneratedType<Base>::Id());
        REQUIRE(compatible && incompatible && generic && child && base);
        auto sourceLease = sourceTypes.Acquire(GeneratedType<Node>::Id());
        auto genericLease = sourceTypes.Acquire(sourceWildcard);
        REQUIRE(sourceLease && genericLease);
        const auto* writer = Next(sourceLease.Value());
        const auto* genericWriter = Next(genericLease.Value());
        REQUIRE(writer && genericWriter);
        FieldValue value(allocator);
        value.kind = ValueKind::ObjectReference;
        value.reference = child.Value().Handle();
        // 각 레지스트리의 서로 다른 type ID가 같은 native Node를 가리키는 별칭이다.
        REQUIRE(writer->write(target, target.OwnerAccess(), compatible.Value().Handle(), value));
        REQUIRE(writer->write(target, target.OwnerAccess(), generic.Value().Handle(), value));
        REQUIRE(
            genericWriter->write(target, target.OwnerAccess(), compatible.Value().Handle(), value));
        CHECK(writer->write(target, target.OwnerAccess(), incompatible.Value().Handle(), value)
                  .Error() == ErrorCode::TypeMismatch);
        CHECK(
            genericWriter->write(target, target.OwnerAccess(), incompatible.Value().Handle(), value)
                .Error() == ErrorCode::TypeMismatch);
        FieldValue output(allocator);
        REQUIRE(writer->read(target, target.OwnerAccess(), incompatible.Value().Handle(), output));
        CHECK(output.reference == ObjectHandle{});
        value.reference = {};
        // 유효한 자기 참조 타입과 일반 Object 경계의 null은 대입할 수 있다.
        REQUIRE(writer->write(target, target.OwnerAccess(), compatible.Value().Handle(), value));
        REQUIRE(genericWriter->write(target, target.OwnerAccess(), incompatible.Value().Handle(),
                                     value));
        // 구체 Node와 Base가 충돌하면 null도 스키마 오류를 숨기지 않는다.
        CHECK(writer->write(target, target.OwnerAccess(), incompatible.Value().Handle(), value)
                  .Error() == ErrorCode::TypeMismatch);
        value.reference = base.Value().Handle();
        CHECK(writer->write(target, target.OwnerAccess(), compatible.Value().Handle(), value)
                  .Error() == ErrorCode::TypeMismatch);
        REQUIRE(writer->read(target, target.OwnerAccess(), compatible.Value().Handle(), output));
        CHECK(output.reference == ObjectHandle{});
        value.reference = {0, 7, 1};
        CHECK(writer->write(target, target.OwnerAccess(), compatible.Value().Handle(), value)
                  .Error() == ErrorCode::StaleHandle);
        REQUIRE(writer->read(target, target.OwnerAccess(), compatible.Value().Handle(), output));
        CHECK(output.reference == ObjectHandle{});
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReferenceWriteDoesNotTreatSameTypeIdAcrossRegistriesAsSameNativeType) {
    SystemAllocator allocator;
    {
        TypeRegistry sourceTypes(allocator);
        TypeRegistry targetTypes(allocator);
        REQUIRE(GeneratedType<Base>::Register(sourceTypes));
        REQUIRE(GeneratedType<Node>::Register(sourceTypes));
        REQUIRE(GeneratedType<Base>::Register(targetTypes));
        // 같은 ID를 서로 다른 registry에서 다른 native 타입으로 등록할 수 있다.
        auto misleading = GeneratedType<Base>::Schema();
        misleading.id = GeneratedType<Node>::Id();
        REQUIRE(targetTypes.Register(misleading, GeneratedType<Base>::Ops()));
        const TypeId ownerAlias{0xc0dec0de, 0x105};
        REQUIRE(RegisterNodeAlias(targetTypes, ownerAlias, GeneratedType<Node>::Id()));
        ObjectRegistry target(allocator, targetTypes, {722, 1});
        auto owner = target.NewObject<Node>(target.OwnerAccess(), ownerAlias);
        REQUIRE(owner);
        auto lease = sourceTypes.Acquire(GeneratedType<Node>::Id());
        REQUIRE(lease);
        const auto* writer = Next(lease.Value());
        REQUIRE(writer);
        FieldValue input(allocator);
        input.kind = ValueKind::ObjectReference;
        input.reference = {};
        CHECK(writer->write(target, target.OwnerAccess(), owner.Value().Handle(), input).Error() ==
              ErrorCode::TypeMismatch);
        FieldValue output(allocator);
        REQUIRE(writer->read(target, target.OwnerAccess(), owner.Value().Handle(), output));
        CHECK(output.reference == ObjectHandle{});
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReferenceWriteKeepsBothDeclaredTypeLeasesUntilCallbackCompletes) {
    SystemAllocator allocator;
    {
        TypeRegistry sourceTypes(allocator);
        TypeRegistry targetTypes(allocator);
        REQUIRE(GeneratedType<Base>::Register(sourceTypes));
        REQUIRE(GeneratedType<Base>::Register(targetTypes));
        const TypeId sourceReference{0xc0dec0de, 0x201};
        const TypeId targetReference{0xc0dec0de, 0x202};
        const TypeId targetOwner{0xc0dec0de, 0x203};
        auto sourceToken = RegisterNodeAlias(sourceTypes, sourceReference, {});
        auto targetToken = RegisterNodeAlias(targetTypes, targetReference, {});
        REQUIRE(sourceToken && targetToken);

        // 각 참조 alias에는 실제 객체가 없으므로 Busy는 owner의 lease가 아닌 검사 lease가 보장한다.
        auto sourceSchema = GeneratedType<Node>::Schema();
        FieldSchema fields[32];
        RuntimeFieldOps accessors[32];
        auto sourceOps = GeneratedType<Node>::Ops();
        REQUIRE(sourceSchema.fields.Size() <= 32 &&
                sourceOps.fields.Size() == sourceSchema.fields.Size());
        for (usize index = 0; index < sourceSchema.fields.Size(); ++index) {
            fields[index] = sourceSchema.fields[index];
            accessors[index] = sourceOps.fields[index];
            if (fields[index].id == NextId) {
                fields[index].referencedType = sourceReference;
            }
            if (accessors[index].fieldId == NextId) {
                accessors[index].write = &ProbeReferenceTypeLifetimes;
            }
        }
        sourceSchema.fields = {fields, sourceSchema.fields.Size()};
        sourceOps.fields = {accessors, sourceOps.fields.Size()};
        REQUIRE(sourceTypes.Register(sourceSchema, Move(sourceOps)));
        REQUIRE(RegisterNodeAlias(targetTypes, targetOwner, targetReference));
        ObjectRegistry target(allocator, targetTypes, {723, 1});
        auto owner = target.NewObject<Node>(target.OwnerAccess(), targetOwner);
        auto sourceLease = sourceTypes.Acquire(sourceSchema.id);
        REQUIRE(owner && sourceLease);
        const auto* writer = Next(sourceLease.Value());
        REQUIRE(writer);
        FieldValue input(allocator);
        input.kind = ValueKind::ObjectReference;
        CallbackUnregistrationProbe probe{&sourceTypes, &targetTypes, sourceToken.Value(),
                                          targetToken.Value()};
        activeProbe = &probe;
        auto result = writer->write(target, target.OwnerAccess(), owner.Value().Handle(), input);
        activeProbe = nullptr;
        REQUIRE(result);
        CHECK(probe.sourceResult == ErrorCode::Busy);
        CHECK(probe.targetResult == ErrorCode::Busy);
        // callback이 반환한 직후에는 두 참조 타입 lease가 해제되어 등록 해제가 가능하다.
        REQUIRE(sourceTypes.Unregister(sourceToken.Value()));
        REQUIRE(targetTypes.Unregister(targetToken.Value()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
