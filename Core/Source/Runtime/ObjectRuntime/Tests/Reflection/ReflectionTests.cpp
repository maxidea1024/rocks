#include "ReflectionFixture.generated.h"
#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Thread.h>

using namespace rocks;
using reflection_fixture::Base;
using reflection_fixture::Node;

namespace {

constexpr Guid FieldId(uint64 low) noexcept {
    return {0xad10000000000000ull, low};
}

bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<Base>::Register(types).IsOk() &&
           GeneratedType<Node>::Register(types).IsOk();
}

const RuntimeFieldOps& Field(const TypeLease& lease, uint64 low) noexcept {
    for (usize index = 0; index < lease.Ops().fields.Size(); ++index) {
        if (lease.Ops().fields[index].fieldId == FieldId(low)) {
            return lease.Ops().fields[index];
        }
    }
    platform::Fatal("Missing generated fixture field");
}

class DestroyCallbackAllocator final : public Allocator {
public:
    explicit DestroyCallbackAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (onAllocate) {
            onAllocate = false;
            destruction = registry->RequestDestroy(registry->OwnerAccess(), target).Error();
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (onFree) {
            onFree = false;
            destruction = registry->RequestDestroy(registry->OwnerAccess(), target).Error();
        }
        return backing_.Deallocate(address);
    }

    ObjectRegistry* registry = nullptr;
    ObjectHandle target{};
    ErrorCode destruction = ErrorCode::DependencyFailed;
    bool onAllocate = false;
    bool onFree = false;

private:
    Allocator& backing_;
};

struct ForeignRead {
    ObjectRegistry* registry;
    const RuntimeFieldOps* field;
    ObjectHandle object;
    FieldValue* output;
    ErrorCode error = ErrorCode::None;
};

void ReadFromWorker(void* raw) noexcept {
    auto& context = *static_cast<ForeignRead*>(raw);
    context.error = context.field
                        ->read(*context.registry, context.registry->OwnerAccess(), context.object,
                               *context.output)
                        .Error();
}

} // namespace

TEST(ReflectionGeneratedScalarGuidStringAccessUsesOwnedValues) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterFixture(types));
        ObjectRegistry objects(allocator, types, {701, 1});
        auto root = objects.NewObject<Node>(objects.OwnerAccess(), GeneratedType<Node>::Id());
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(root && lease);
        CHECK(lease.Value().Ops().completeFieldLayout);
        CHECK(lease.Value().Ops().parallelTraceSafe);
        CHECK(lease.Value().Schema().fields.Size() == 12);
        FieldValue input(allocator);
        FieldValue output(allocator);
        const uint64 lows[] = {0x23, 0x24, 0x25, 0x26, 0x27, 0x12, 0x28, 0x29, 0x2a, 0x11};
        const ValueKind kinds[] = {ValueKind::Bool,   ValueKind::UInt8,   ValueKind::UInt16,
                                   ValueKind::UInt32, ValueKind::UInt64,  ValueKind::Int32,
                                   ValueKind::Int64,  ValueKind::Float32, ValueKind::Float64,
                                   ValueKind::Guid};
        input.boolean = true;
        input.unsignedInteger = 127;
        input.signedInteger = -321;
        input.float32 = 1.25f;
        input.float64 = -2.5;
        input.guid = {9, 17};
        for (usize index = 0; index < 10; ++index) {
            input.kind = kinds[index];
            const auto& field = Field(lease.Value(), lows[index]);
            REQUIRE(field.write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
            REQUIRE(field.read(objects, objects.OwnerAccess(), root.Value().Handle(), output));
            CHECK(output.kind == kinds[index]);
            switch (kinds[index]) {
            case ValueKind::Bool:
                CHECK(output.boolean == input.boolean);
                break;
            case ValueKind::UInt8:
            case ValueKind::UInt16:
            case ValueKind::UInt32:
            case ValueKind::UInt64:
                CHECK(output.unsignedInteger == input.unsignedInteger);
                break;
            case ValueKind::Int32:
            case ValueKind::Int64:
                CHECK(output.signedInteger == input.signedInteger);
                break;
            case ValueKind::Float32:
                CHECK(output.float32 == input.float32);
                break;
            case ValueKind::Float64:
                CHECK(output.float64 == input.float64);
                break;
            default:
                CHECK(output.guid == input.guid);
                break;
            }
        }
        CHECK(output.guid == input.guid);
        input.kind = ValueKind::UInt8;
        input.unsignedInteger = 256;
        CHECK(Field(lease.Value(), 0x24)
                  .write(objects, objects.OwnerAccess(), root.Value().Handle(), input)
                  .Error() == ErrorCode::Overflow);
        REQUIRE(Field(lease.Value(), 0x24)
                    .read(objects, objects.OwnerAccess(), root.Value().Handle(), output));
        CHECK(output.unsignedInteger == 127);
        input.kind = ValueKind::Utf8String;
        REQUIRE(input.string.TryAssign("\xED\x95\x9C\xEA\xB8\x80"));
        REQUIRE(Field(lease.Value(), 0x21)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        REQUIRE(Field(lease.Value(), 0x21)
                    .read(objects, objects.OwnerAccess(), root.Value().Handle(), output));
        input.string.Clear();
        REQUIRE(objects.RequestDestroy(objects.OwnerAccess(), root.Value().Handle()));
        CHECK(output.string.View() == StringView("\xED\x95\x9C\xEA\xB8\x80"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionIsAAndMetadataLifetimeDoNotEnableUnsafeNativeBaseCast) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        auto base = GeneratedType<Base>::Register(types);
        auto derived = GeneratedType<Node>::Register(types);
        REQUIRE(base && derived);
        CHECK(types.Unregister(base.Value()).Error() == ErrorCode::Busy);
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(lease);
        CHECK(lease.Value().IsA(GeneratedType<Base>::Id()));
        CHECK(lease.Value().IsA(GeneratedType<Node>::Id()));
        CHECK(!lease.Value().IsA(Guid{88, 99}));
        {
            ObjectRegistry objects(allocator, types, {702, 1});
            auto object = objects.NewObject<Node>(objects.OwnerAccess(), GeneratedType<Node>::Id());
            REQUIRE(object);
            // 파생 Node는 기반 Base로 Resolve할 수 있다. 관계없는 타입은 여전히 TypeMismatch다.
            CHECK(objects.Resolve<Base>(objects.OwnerAccess(), object.Value().Handle()).IsOk());
            CHECK(objects
                      .Resolve<reflection_fixture::AliasPrivate>(objects.OwnerAccess(),
                                                                 object.Value().Handle())
                      .Error() == ErrorCode::TypeMismatch);
            FieldValue output(allocator);
            output.kind = ValueKind::UInt64;
            output.unsignedInteger = 91;
            CHECK(Field(lease.Value(), 0x21)
                      .read(objects, ObjectAccessContext{{999, 1}}, object.Value().Handle(), output)
                      .Error() == ErrorCode::WrongContext);
            CHECK(output.kind == ValueKind::UInt64 && output.unsignedInteger == 91);
            ForeignRead work{&objects, &Field(lease.Value(), 0x21), object.Value().Handle(),
                             &output};
            platform::Thread worker;
            REQUIRE(worker.Start(ReadFromWorker, &work) == platform::ThreadStatus::Success);
            REQUIRE(worker.WaitForCompletion(10000) == platform::WaitResult::Signaled);
            REQUIRE(worker.Join() == platform::ThreadStatus::Success);
            CHECK(work.error == ErrorCode::WrongThread);
        }
        CHECK(types.Shutdown().Error() == ErrorCode::Busy);
        lease.Value().Reset();
        REQUIRE(types.Shutdown());
        CHECK(types.Stats().registeredTypes == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionStagedWritesAndTypedReferenceUseRegistryBarriers) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterFixture(types));
        ObjectRegistry objects(allocator, types, {703, 1});
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(lease);
        RootRef<Object> root;
        ObjectHandle child{};
        {
            ConstructionContext construction(objects, objects.OwnerAccess());
            auto parent = construction.Create<Node>(GeneratedType<Node>::Id());
            auto next = construction.Create<Node>(GeneratedType<Node>::Id());
            REQUIRE(parent && next);
            child = next.Value();
            FieldValue value(allocator);
            value.kind = ValueKind::ObjectReference;
            value.reference = child;
            REQUIRE(Field(lease.Value(), 0x22).writeStaged(construction, parent.Value(), value));
            value.kind = ValueKind::Utf8String;
            REQUIRE(value.string.TryAssign("staged"));
            REQUIRE(Field(lease.Value(), 0x21).writeStaged(construction, parent.Value(), value));
            auto committed = construction.Commit();
            REQUIRE(committed);
            root = Move(committed.Value());
        }
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Stats().liveObjects == 2);
        auto wrongType = objects.NewObject<Base>(objects.OwnerAccess(), GeneratedType<Base>::Id());
        REQUIRE(wrongType);
        FieldValue value(allocator);
        value.kind = ValueKind::ObjectReference;
        value.reference = wrongType.Value().Handle();
        CHECK(Field(lease.Value(), 0x22)
                  .write(objects, objects.OwnerAccess(), root.Handle(), value)
                  .Error() == ErrorCode::TypeMismatch);
        REQUIRE(
            Field(lease.Value(), 0x22).read(objects, objects.OwnerAccess(), root.Handle(), value));
        CHECK(value.reference == child);
        value.reference = {};
        REQUIRE(
            Field(lease.Value(), 0x22).write(objects, objects.OwnerAccess(), root.Handle(), value));
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Stats().liveObjects == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionStringReadOomKeepsOutputAndSwapTransfersAllocatorOwnership) {
    SystemAllocator allocator;
    SystemAllocator second;
    {
        String left(allocator);
        String right(second);
        REQUIRE(left.TryAssign("left"));
        REQUIRE(right.TryAssign("right"));
        const char* leftData = left.Data();
        const char* rightData = right.Data();
        left.Swap(right);
        CHECK(left.Data() == rightData && right.Data() == leftData);
        CHECK(&left.GetAllocator() == &second && &right.GetAllocator() == &allocator);
        left.Swap(left);
        CHECK(left.View() == StringView("right"));
        TypeRegistry types(allocator);
        REQUIRE(RegisterFixture(types));
        ObjectRegistry objects(allocator, types, {704, 1});
        auto root = objects.NewObject<Node>(objects.OwnerAccess(), GeneratedType<Node>::Id());
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(root && lease);
        FieldValue input(allocator);
        input.kind = ValueKind::Utf8String;
        REQUIRE(input.string.TryAssign("a complete owned value"));
        REQUIRE(Field(lease.Value(), 0x21)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        test::NFailAllocator failing(second);
        FieldValue output(failing);
        output.kind = ValueKind::UInt64;
        output.unsignedInteger = 91;
        REQUIRE(output.string.TryAssign("preserved"));
        failing.FailOnAttempt(1);
        CHECK(Field(lease.Value(), 0x21)
                  .read(objects, objects.OwnerAccess(), root.Value().Handle(), output)
                  .Error() == ErrorCode::OutOfMemory);
        CHECK(output.kind == ValueKind::UInt64 && output.unsignedInteger == 91);
        CHECK(output.string.View() == StringView("preserved"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

TEST(ReflectionStringSetterLogicalDestroyBeforeCommitLeavesOldValue) {
    SystemAllocator backing;
    DestroyCallbackAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterFixture(types));
        ObjectRegistry objects(allocator, types, {705, 1});
        allocator.registry = &objects;
        auto root = objects.NewObject<Node>(objects.OwnerAccess(), GeneratedType<Node>::Id());
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(root && lease);
        allocator.target = root.Value().Handle();
        FieldValue input(backing);
        input.kind = ValueKind::Utf8String;
        REQUIRE(input.string.TryAssign("old"));
        REQUIRE(Field(lease.Value(), 0x21)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        REQUIRE(input.string.TryAssign("new"));
        allocator.onAllocate = true;
        CHECK(Field(lease.Value(), 0x21)
                  .write(objects, objects.OwnerAccess(), root.Value().Handle(), input)
                  .Error() == ErrorCode::Destroyed);
        CHECK(allocator.destruction == ErrorCode::None);
        root.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(Node::destroyedWithOldLabel);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ReflectionStringSetterLogicalDestroyAfterCommitKeepsSuccess) {
    SystemAllocator backing;
    DestroyCallbackAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterFixture(types));
        ObjectRegistry objects(allocator, types, {706, 1});
        allocator.registry = &objects;
        auto root = objects.NewObject<Node>(objects.OwnerAccess(), GeneratedType<Node>::Id());
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(root && lease);
        allocator.target = root.Value().Handle();
        FieldValue input(backing);
        input.kind = ValueKind::Utf8String;
        REQUIRE(input.string.TryAssign("old"));
        REQUIRE(Field(lease.Value(), 0x21)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        REQUIRE(input.string.TryAssign("new"));
        allocator.onFree = true;
        REQUIRE(Field(lease.Value(), 0x21)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        CHECK(allocator.destruction == ErrorCode::None);
        root.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(Node::destroyedWithNewLabel);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ReflectionRegistryOwnsOpsAndRejectsIncompleteTypedMetadata) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        auto base = GeneratedType<Base>::Register(types);
        REQUIRE(base);
        auto sourceOps = GeneratedType<Node>::Ops();
        RuntimeFieldOps copied[12];
        for (usize index = 0; index < 12; ++index) {
            copied[index] = sourceOps.fields[index];
        }
        sourceOps.fields = {copied, 12};
        auto registered = types.Register(GeneratedType<Node>::Schema(), Move(sourceOps));
        REQUIRE(registered);
        copied[0].read = nullptr;
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(lease);
        CHECK(lease.Value().Ops().fields.Data() != copied);
        CHECK(lease.Value().Ops().fields[0].read != nullptr);
        lease.Value().Reset();
        REQUIRE(types.Unregister(registered.Value()));
        auto missing = GeneratedType<Node>::Ops();
        missing.fields = {};
        CHECK(types.Register(GeneratedType<Node>::Schema(), Move(missing)).Error() ==
              ErrorCode::InvalidArgument);
        auto mismatch = GeneratedType<Node>::Ops();
        mismatch.nativeBaseType = NativeTypeKey<Node>();
        CHECK(types.Register(GeneratedType<Node>::Schema(), Move(mismatch)).Error() ==
              ErrorCode::TypeMismatch);
        REQUIRE(types.Unregister(base.Value()));
        CHECK(GeneratedType<Node>::Register(types).Error() == ErrorCode::NotFound);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionStringSetterOomDoesNotChangeFieldOrDestroyOwner) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterFixture(types));
        ObjectRegistry objects(allocator, types, {707, 1});
        auto root = objects.NewObject<Node>(objects.OwnerAccess(), GeneratedType<Node>::Id());
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(root && lease);
        FieldValue input(backing);
        input.kind = ValueKind::Utf8String;
        REQUIRE(input.string.TryAssign("old"));
        REQUIRE(Field(lease.Value(), 0x21)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        REQUIRE(input.string.TryAssign("new text requiring owned replacement"));
        allocator.FailOnAttempt(1);
        CHECK(Field(lease.Value(), 0x21)
                  .write(objects, objects.OwnerAccess(), root.Value().Handle(), input)
                  .Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Attempts() == 1);
        allocator.DisableFailure();
        FieldValue output(backing);
        REQUIRE(Field(lease.Value(), 0x21)
                    .read(objects, objects.OwnerAccess(), root.Value().Handle(), output));
        CHECK(output.string.View() == StringView("old"));
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ReflectionStringGetterLogicalDestroyDuringCopyKeepsOutput) {
    SystemAllocator backing;
    DestroyCallbackAllocator outputAllocator(backing);
    {
        TypeRegistry types(backing);
        REQUIRE(RegisterFixture(types));
        ObjectRegistry objects(backing, types, {708, 1});
        auto root = objects.NewObject<Node>(objects.OwnerAccess(), GeneratedType<Node>::Id());
        auto lease = types.Acquire(GeneratedType<Node>::Id());
        REQUIRE(root && lease);
        FieldValue input(backing);
        input.kind = ValueKind::Utf8String;
        REQUIRE(input.string.TryAssign("old"));
        REQUIRE(Field(lease.Value(), 0x21)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), input));
        FieldValue output(outputAllocator);
        output.kind = ValueKind::UInt64;
        output.unsignedInteger = 93;
        REQUIRE(output.string.TryAssign("preserved"));
        outputAllocator.registry = &objects;
        outputAllocator.target = root.Value().Handle();
        outputAllocator.onAllocate = true;
        CHECK(Field(lease.Value(), 0x21)
                  .read(objects, objects.OwnerAccess(), root.Value().Handle(), output)
                  .Error() == ErrorCode::Destroyed);
        CHECK(outputAllocator.destruction == ErrorCode::None);
        CHECK(output.kind == ValueKind::UInt64 && output.unsignedInteger == 93);
        CHECK(output.string.View() == StringView("preserved"));
        root.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(Node::destroyedWithOldLabel);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ReflectionRegistryAllocationFailureDoesNotPinBaseAndCanRetry) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        TypeRegistry types(allocator);
        auto base = GeneratedType<Base>::Register(types);
        REQUIRE(base);
        const auto before = backing.Stats();
        allocator.FailOnAttempt(1);
        CHECK(GeneratedType<Node>::Register(types).Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Attempts() == 1);
        CHECK(backing.Stats().liveAllocations == before.liveAllocations);
        CHECK(backing.Stats().liveBytes == before.liveBytes);
        allocator.DisableFailure();
        REQUIRE(types.Unregister(base.Value()));
        REQUIRE(RegisterFixture(types));
        REQUIRE(types.Shutdown());
    }
    CHECK(backing.Stats().liveAllocations == 0);
}
