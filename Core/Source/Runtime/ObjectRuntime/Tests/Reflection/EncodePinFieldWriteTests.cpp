#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// encode 고정 중 등록된 필드 쓰기 시험이다(Docs/Design/ReflectionFoundations.ko.md 5.6절 "encode
// 고정"): encode가 순회하는 owner 밖 버퍼(String·Soft 경로)를 쓰기가 해제하지 못하게, 고정된
// owner의 생성 필드 쓰기(WriteGeneratedField·FieldWriteOperation)는 값·String·Soft·참조 모두
// Busy다.
using namespace rocks;
using reflection_fixture::AssetFields;
using reflection_fixture::Base;
using reflection_fixture::Node;

namespace {

const RuntimeFieldOps& FieldByName(const TypeLease& lease, StringView name) noexcept {
    const auto fields = lease.Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (StringView(fields[index].name.Data(), fields[index].name.Size()) != name) {
            continue;
        }

        for (const RuntimeFieldOps& ops : lease.Ops().fields) {
            if (ops.fieldId == fields[index].id) {
                return ops;
            }
        }
    }

    platform::Fatal("fixture field missing");
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

} // namespace

TEST(EncodePin_RegisteredFieldWritesAreBusy) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<Base>::Register(types));
        REQUIRE(GeneratedType<Node>::Register(types));
        REQUIRE(GeneratedType<AssetFields>::Register(types));
        ObjectRegistry objects(allocator, types, {0x4550, 1});
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, TypeIdOf<Node>());
        auto other = objects.NewObject<Node>(access, TypeIdOf<Node>());
        auto asset = objects.NewObject<AssetFields>(access, TypeIdOf<AssetFields>());
        auto nodeType = types.Acquire(TypeIdOf<Node>());
        auto assetType = types.Acquire(TypeIdOf<AssetFields>());
        REQUIRE(node && other && asset && nodeType && assetType);
        const ObjectHandle handle = node.Value().Handle();
        const RuntimeFieldOps& label = FieldByName(nodeType.Value(), "label");
        const RuntimeFieldOps& flags = FieldByName(nodeType.Value(), "flags");
        const RuntimeFieldOps& next = FieldByName(nodeType.Value(), "next");
        const RuntimeFieldOps& icon = FieldByName(assetType.Value(), "icon");

        FieldValue text(allocator);
        text.kind = ValueKind::Utf8String;
        REQUIRE(text.string.TryAssign("old"));
        REQUIRE(label.write(objects, access, handle, text));
        FieldValue number(allocator);
        number.kind = ValueKind::UInt32;
        number.unsignedInteger = 7;
        FieldValue reference(allocator);
        reference.kind = ValueKind::ObjectReference;
        reference.reference = other.Value().Handle();
        FieldValue soft(allocator);
        soft.kind = ValueKind::SoftObjectReference;
        soft.guid = Guid{0x0a, 0x0b};
        REQUIRE(soft.string.TryAssign("Body.Mesh"));

        // 고정된 owner: 등록된 쓰기(FieldWriteOperation)와 생성 thunk 직접 호출 모두 Busy다.
        auto pin = objects.PinForEncode(access, handle);
        auto assetPin = objects.PinForEncode(access, asset.Value().Handle());
        REQUIRE(pin && assetPin);
        REQUIRE(text.string.TryAssign("new"));
        CHECK(ErrorOf(label.write(objects, access, handle, text)) == ErrorCode::Busy);
        CHECK(ErrorOf(detail::WriteGeneratedField<Node>(objects, access, handle, &Node::label,
                                                        text)) == ErrorCode::Busy);
        CHECK(ErrorOf(flags.write(objects, access, handle, number)) == ErrorCode::Busy);
        CHECK(ErrorOf(next.write(objects, access, handle, reference)) == ErrorCode::Busy);
        CHECK(ErrorOf(icon.write(objects, access, asset.Value().Handle(), soft)) ==
              ErrorCode::Busy);
        // 읽기는 고정과 무관하고 값은 바뀌지 않았다. 고정되지 않은 다른 객체는 쓸 수 있다.
        FieldValue read(allocator);
        REQUIRE(label.read(objects, access, handle, read));
        CHECK(read.string.View() == StringView("old"));
        CHECK(label.write(objects, access, other.Value().Handle(), text));

        // 고정을 풀면 다시 쓸 수 있다.
        pin.Value().Reset();
        assetPin.Value().Reset();
        CHECK(label.write(objects, access, handle, text));
        CHECK(flags.write(objects, access, handle, number));
        CHECK(next.write(objects, access, handle, reference));
        CHECK(icon.write(objects, access, asset.Value().Handle(), soft));
        REQUIRE(label.read(objects, access, handle, read));
        CHECK(read.string.View() == StringView("new"));
        nodeType.Value().Reset();
        assetType.Value().Reset();
        node.Value().Reset();
        other.Value().Reset();
        asset.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
