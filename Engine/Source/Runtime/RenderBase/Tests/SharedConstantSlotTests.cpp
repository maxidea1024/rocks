#include "RecordingDevice.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <RenderBase/SharedConstantBindings.h>
#include <RenderBase/SharedConstantSlotMap.h>
#include <RenderBase/SharedConstantSlotTable.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 공유 상수 slot 표 기반 시험이다(Docs/Design/RenderBase 9.14절 RR13 완료 기준): 표 등록 규칙, 구조체
// identity로 찾는 파이프라인 지도, 묶음의 identity 대조·미묶음 거부·장치 호출 열.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::shader;
using rocks::render::testing::RecordingDevice;

namespace shared_constant_slot_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 생성 표와 같은 모양(identity 있음)의 시험 표다.
constexpr ShaderParameterMember ViewMembers[] = {{"viewToClip", ShaderParameterType::Float4x4, 0, 0},
                                                 {"time", ShaderParameterType::Float4, 64, 0}};
constexpr ShaderParameterLayout ViewLayout("View", false, 80, ViewMembers, "rocks::test::ViewParameters",
                                           "/Engine/Generated/Test/ViewParameters.rsh",
                                           "cbuffer View { float4x4 viewToClip; float4 time; };");
constexpr ShaderParameterMember FogMembers[] = {{"density", ShaderParameterType::Float4, 0, 0}};
constexpr ShaderParameterLayout FogLayout("Fog", false, 16, FogMembers, "rocks::test::FogParameters",
                                          "/Engine/Generated/Test/FogParameters.rsh",
                                          "cbuffer Fog { float4 density; };");
// 같은 크기·멤버지만 다른 구조체다(identity가 달라 slot이 아니다).
constexpr ShaderParameterLayout OtherFogLayout("Fog", false, 16, FogMembers, "rocks::test::OtherFog",
                                               "/Engine/Generated/Test/OtherFog.rsh",
                                               "cbuffer Fog { float4 density; };");
constexpr ShaderParameterLayout HandLayout{"Hand", false, 16, FogMembers};
constexpr ShaderParameterMember RootMembers[] = {{"tint", ShaderParameterType::Float4, 0, 0}};
constexpr ShaderParameterLayout RootLayout("Root", true, 16, RootMembers, "rocks::test::Root",
                                           "/Engine/Generated/Test/Root.rsh",
                                           "ROCKS_ROOT_CONSTANTS(Root, Draw);");

void AddBuffer(ShaderBindingLayout& layout, const ShaderParameterLayout& table, StringView name,
               uint16 slot, uint32 bytes) noexcept {
    const uint32 first = static_cast<uint32>(layout.members.Size());
    for (const ShaderParameterMember& member : table.members) {
        (void)layout.members.TryPushBack(ShaderBindingMember{
            member.name, member.offset, static_cast<uint32>(ShaderParameterMemberBytes(member))});
    }

    ShaderBindingResource buffer;
    buffer.kind = ShaderResourceKind::ConstantBuffer;
    buffer.slot = slot;
    buffer.name = name;
    buffer.constantBufferBytes = bytes;
    buffer.structId = table.structId;
    buffer.firstMember = first;
    buffer.memberCount = static_cast<uint32>(table.members.Size());
    (void)layout.resources.TryPushBack(buffer);
}

uint64 NthValue(const RecordingDevice& device, RecordingDevice::Op op, usize n) noexcept {
    usize seen = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        if (call.op == op && seen++ == n) {
            return call.value;
        }
    }

    return 0;
}

usize CountOps(const RecordingDevice& device, RecordingDevice::Op op) noexcept {
    usize count = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        count += call.op == op ? 1u : 0u;
    }

    return count;
}

TEST(SharedConstantSlots_TableRegistrationRules) {
    SharedConstantSlotTable table;
    const char* reason = nullptr;
    auto view = table.Register({"View", &ViewLayout, ConstantBlockLifetime::PerFrame}, &reason);
    REQUIRE(view);
    CHECK(view.Value() == 0);
    auto fog = table.Register({"Fog", &FogLayout, ConstantBlockLifetime::PerFrame});
    REQUIRE(fog);
    CHECK(fog.Value() == 1);
    // 이름·identity 중복.
    CHECK(table.Register({"View", &OtherFogLayout, ConstantBlockLifetime::PerFrame}).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(table.Register({"Fog2", &FogLayout, ConstantBlockLifetime::PerFrame}).Error() ==
          ErrorCode::AlreadyExists);
    // identity 없는(손으로 쓴) 표, 루트 상수, 표 없음, 이름 규칙.
    CHECK(table.Register({"Hand", &HandLayout, ConstantBlockLifetime::PerFrame}, &reason).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(table.Register({"Root", &RootLayout, ConstantBlockLifetime::PerFrame}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(table.Register({"Empty", nullptr, ConstantBlockLifetime::PerFrame}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(table.Register({"1View", &OtherFogLayout, ConstantBlockLifetime::PerFrame}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(table.Register({"", &OtherFogLayout, ConstantBlockLifetime::PerFrame}).Error() ==
          ErrorCode::InvalidArgument);
    // 조회.
    CHECK(table.FindByStruct(FogLayout.structId).Value() == 1);
    CHECK(table.FindByName("View").Value() == 0);
    CHECK(table.FindByStruct(OtherFogLayout.structId).Error() == ErrorCode::NotFound);
    CHECK(table.FindByStruct(0).Error() == ErrorCode::NotFound);
    CHECK(table.At(2) == nullptr && table.At(1)->layout == &FogLayout);
    // Freeze 전 지도는 만들지 않는다.
    CHECK(SharedConstantSlotMap::Build(table, {}).Error() == ErrorCode::InvalidArgument);
    // Freeze 뒤 등록은 ScopeClosed다(두 번째 Freeze는 아무 일도 하지 않는다).
    table.Freeze();
    table.Freeze();
    CHECK(table.IsFrozen());
    CHECK(table.Register({"Other", &OtherFogLayout, ConstantBlockLifetime::PerFrame}).Error() ==
          ErrorCode::ScopeClosed);
    CHECK(table.Count() == 2);

    // 상한(8).
    SharedConstantSlotTable full;
    static char names[MaxSharedConstantSlots + 1][16];
    static char structs[MaxSharedConstantSlots + 1][32];
    static ShaderParameterLayout layouts[MaxSharedConstantSlots + 1];
    for (usize index = 0; index <= MaxSharedConstantSlots; ++index) {
        (void)snprintf(names[index], sizeof(names[index]), "Slot%zu", index);
        (void)snprintf(structs[index], sizeof(structs[index]), "rocks::test::Slot%zu", index);
        layouts[index] = ShaderParameterLayout("Slot", false, 16, FogMembers, StringView(structs[index]),
                                               StringView(), StringView());
        const auto added =
            full.Register({StringView(names[index]), &layouts[index], ConstantBlockLifetime::PerFrame});
        CHECK(index < MaxSharedConstantSlots ? added.IsOk()
                                             : added.Error() == ErrorCode::CapacityExceeded);
    }
}

TEST(SharedConstantSlots_MapFindsSlotsByStructIdentity) {
    SharedConstantSlotTable table;
    REQUIRE(table.Register({"View", &ViewLayout, ConstantBlockLifetime::PerFrame}));
    REQUIRE(table.Register({"Fog", &FogLayout, ConstantBlockLifetime::PerFrame}));
    table.Freeze();

    // VS는 View를 cbuffer 이름 "Camera"로 slot 2에, PS는 View를 slot 0에, 이름은 Fog지만 다른
    // 구조체인 버퍼를 slot 3에 둔다.
    ShaderBindingLayout vertex(Allocator_());
    vertex.stage = ShaderStageVertex;
    AddBuffer(vertex, ViewLayout, "Camera", 2, 80);
    ShaderBindingLayout pixel(Allocator_());
    pixel.stage = ShaderStagePixel;
    AddBuffer(pixel, ViewLayout, "View", 0, 80);
    AddBuffer(pixel, OtherFogLayout, "Fog", 3, 16);
    const ShaderBindingLayout* stages[] = {&vertex, &pixel};
    auto map = SharedConstantSlotMap::Build(table, stages);
    REQUIRE(map);
    REQUIRE(map.Value().Uses().Size() == 1);
    const SharedConstantSlotUse& use = map.Value().Uses()[0];
    CHECK(use.slot == 0 && map.Value().Reads(0) && !map.Value().Reads(1));
    REQUIRE(use.map.Placements().Size() == 2);
    CHECK(use.map.Placements()[0].stage == ShaderStageVertex && use.map.Placements()[0].slot == 2);
    CHECK(use.map.Placements()[1].stage == ShaderStagePixel && use.map.Placements()[1].slot == 0);

    // 같은 구조체인데 크기가 slot 표보다 크면 거부한다.
    ShaderBindingLayout wide(Allocator_());
    wide.stage = ShaderStagePixel;
    AddBuffer(wide, ViewLayout, "View", 0, 96);
    const ShaderBindingLayout* wideStages[] = {&wide};
    const char* reason = nullptr;
    CHECK(SharedConstantSlotMap::Build(table, wideStages, &reason).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(reason != nullptr);
    // 공유 slot이 없는 파이프라인은 빈 지도다.
    ShaderBindingLayout none(Allocator_());
    none.stage = ShaderStagePixel;
    const ShaderBindingLayout* noneStages[] = {&none, nullptr};
    auto empty = SharedConstantSlotMap::Build(table, noneStages);
    REQUIRE(empty);
    CHECK(empty.Value().Uses().IsEmpty());
}

TEST(SharedConstantSlots_BindingsApplyBlocksAndRejectMismatches) {
    SharedConstantSlotTable table;
    REQUIRE(table.Register({"View", &ViewLayout, ConstantBlockLifetime::PerDraw}));
    REQUIRE(table.Register({"Fog", &FogLayout, ConstantBlockLifetime::PerDraw}));
    table.Freeze();
    ShaderBindingLayout pixel(Allocator_());
    pixel.stage = ShaderStagePixel;
    AddBuffer(pixel, ViewLayout, "View", 1, 80);
    AddBuffer(pixel, FogLayout, "Fog", 5, 16);
    const ShaderBindingLayout* stages[] = {&pixel};
    auto map = SharedConstantSlotMap::Build(table, stages);
    REQUIRE(map);
    REQUIRE(map.Value().Uses().Size() == 2);

    ConstantBlock view(Allocator_(), ViewLayout, ConstantBlockLifetime::PerDraw);
    ConstantBlock fog(Allocator_(), FogLayout, ConstantBlockLifetime::PerDraw);
    ConstantBlock otherFog(Allocator_(), OtherFogLayout, ConstantBlockLifetime::PerDraw);
    REQUIRE(view.Initialize() && fog.Initialize() && otherFog.Initialize());
    const float density[4] = {0.25f, 0.0f, 0.0f, 0.0f};
    REQUIRE(fog.SetValue("density", density));

    SharedConstantBindings bindings(table);
    // identity가 다른 블록·없는 slot은 거부하고 앞 값은 그대로다.
    CHECK(bindings.Bind("Fog", &otherFog).Error() == ErrorCode::InvalidArgument);
    CHECK(bindings.Bind("Missing", &fog).Error() == ErrorCode::NotFound);
    CHECK(bindings.Bind(MaxSharedConstantSlots, &fog).Error() == ErrorCode::NotFound);
    REQUIRE(bindings.Bind("View", &view));
    CHECK(bindings.Bound(0) == &view && bindings.Bound(1) == nullptr);

    RecordingDevice device(Allocator_());
    graphics::PassDesc pass;
    auto recording = device.BeginPass(pass);
    REQUIRE(recording);
    graphics::CommandContext& commands = *recording.Value();
    // Fog가 묶이지 않았다: View는 이미 묶였고 Fog에서 멈춘다.
    const char* reason = nullptr;
    CHECK(bindings.Apply(commands, map.Value(), &reason).Error() == ErrorCode::InvalidArgument);
    CHECK(reason != nullptr);
    CHECK(CountOps(device, RecordingDevice::Op::SetConstantData) == 1);
    REQUIRE(bindings.Bind(1, &fog));
    REQUIRE(bindings.Apply(commands, map.Value()));
    // View(slot 1), View, Fog(slot 5) 순서다 — 값의 첫 word로 확인한다.
    CHECK(CountOps(device, RecordingDevice::Op::SetConstantData) == 3);
    uint32 densityWord = 0;
    platform::MemoryCopy(&densityWord, &density[0], sizeof(densityWord));
    CHECK(NthValue(device, RecordingDevice::Op::SetConstantData, 0) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Pixel, 1, 0));
    CHECK(NthValue(device, RecordingDevice::Op::SetConstantData, 2) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Pixel, 5, densityWord));
    // null로 비우면 다시 미묶음이다.
    REQUIRE(bindings.Bind(1, nullptr));
    CHECK(!bindings.Apply(commands, map.Value()));
}

} // namespace shared_constant_slot_tests_file
