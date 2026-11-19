#include <Graphics/DeviceTypes.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace buffer_view_tests_file {

using namespace rocks::graphics;

namespace {

BufferDesc Desc(uint64 size, BufferUsage usage, uint32 stride = 0) noexcept {
    BufferDesc desc;
    desc.size = size;
    desc.usage = usage;
    desc.stride = stride;
    return desc;
}

} // namespace

TEST(BufferDesc_AcceptsUsageCombinations) {
    CHECK(ValidateBufferDesc(Desc(12, BufferUsage::Vertex)));
    CHECK(ValidateBufferDesc(Desc(12, BufferUsage::Vertex | BufferUsage::Index)));
    CHECK(ValidateBufferDesc(Desc(256, BufferUsage::Constant)));
    CHECK(ValidateBufferDesc(Desc(20, BufferUsage::Indirect | BufferUsage::ShaderWrite)));
    CHECK(ValidateBufferDesc(Desc(64, BufferUsage::ShaderRead | BufferUsage::ShaderWrite)));
    CHECK(ValidateBufferDesc(Desc(64, BufferUsage::ShaderRead | BufferUsage::Structured, 32)));
    CHECK(ValidateBufferDesc(
        Desc(64, BufferUsage::Vertex | BufferUsage::ShaderRead | BufferUsage::Raw)));
}

TEST(BufferDesc_RejectsInvalidCombinations) {
    const auto rejected = [](const BufferDesc& desc) noexcept {
        return ValidateBufferDesc(desc).Error() == ErrorCode::InvalidArgument;
    };
    CHECK(rejected(Desc(0, BufferUsage::Vertex)));
    CHECK(rejected(Desc(16, BufferUsage::None)));
    CHECK(rejected(Desc(16, static_cast<BufferUsage>(1u << 12))));
    // 상수 버퍼는 홀로, 16 byte 배수로만 만든다.
    CHECK(rejected(Desc(16, BufferUsage::Constant | BufferUsage::ShaderRead)));
    CHECK(rejected(Desc(20, BufferUsage::Constant)));
    // 구조화 버퍼는 stride·셰이더 접근이 있어야 하고 정점·index·간접 인자와 묶지 않는다.
    CHECK(rejected(Desc(64, BufferUsage::ShaderRead | BufferUsage::Structured)));
    CHECK(rejected(Desc(64, BufferUsage::ShaderRead | BufferUsage::Structured, 6)));
    CHECK(rejected(Desc(64, BufferUsage::ShaderRead | BufferUsage::Structured, 24)));
    CHECK(rejected(Desc(4096, BufferUsage::ShaderRead | BufferUsage::Structured, 4096)));
    CHECK(rejected(Desc(64, BufferUsage::Structured, 16)));
    CHECK(rejected(
        Desc(64, BufferUsage::Vertex | BufferUsage::ShaderRead | BufferUsage::Structured, 16)));
    CHECK(rejected(
        Desc(64, BufferUsage::ShaderRead | BufferUsage::Structured | BufferUsage::Raw, 16)));
    CHECK(rejected(Desc(64, BufferUsage::ShaderRead, 16)));
    // raw는 셰이더 접근과 4 byte 배수가 필요하다. 간접 인자도 4 byte 배수다.
    CHECK(rejected(Desc(64, BufferUsage::Vertex | BufferUsage::Raw)));
    CHECK(rejected(Desc(62, BufferUsage::ShaderRead | BufferUsage::Raw)));
    CHECK(rejected(Desc(10, BufferUsage::Indirect)));
}

TEST(BufferView_ResolvesRanges) {
    const BufferDesc typed = Desc(256, BufferUsage::ShaderRead | BufferUsage::ShaderWrite);
    BufferViewDesc view;
    view.format = PixelFormat::RGBA32Float;
    auto whole = ResolveBufferView(typed, view);
    REQUIRE(whole);
    CHECK(whole.Value().offset == 0);
    CHECK(whole.Value().elementCount == 16);
    CHECK(whole.Value().elementBytes == 16);
    view.format = PixelFormat::R32Uint;
    view.access = ViewAccess::ReadWrite;
    view.offset = 64;
    view.elementCount = 8;
    auto part = ResolveBufferView(typed, view);
    REQUIRE(part);
    CHECK(part.Value().Bytes() == 32);

    BufferViewDesc structured;
    structured.kind = BufferViewKind::Structured;
    structured.offset = 32;
    structured.elementCount = 1;
    auto element = ResolveBufferView(
        Desc(64, BufferUsage::ShaderRead | BufferUsage::Structured, 32), structured);
    REQUIRE(element);
    CHECK(element.Value().elementBytes == 32);

    BufferViewDesc raw;
    raw.kind = BufferViewKind::Raw;
    auto words = ResolveBufferView(Desc(64, BufferUsage::ShaderRead | BufferUsage::Raw), raw);
    REQUIRE(words);
    CHECK(words.Value().elementCount == 16);
    // raw 버퍼에도 typed view를 만들 수 있다.
    view = {};
    view.format = PixelFormat::R32Float;
    CHECK(ResolveBufferView(Desc(64, BufferUsage::ShaderRead | BufferUsage::Raw), view));
}

TEST(BufferView_RejectsKindAccessFormatAndRange) {
    const auto rejected = [](const BufferDesc& buffer, const BufferViewDesc& view) noexcept {
        return ResolveBufferView(buffer, view).Error() == ErrorCode::InvalidArgument;
    };
    const BufferDesc readOnly = Desc(256, BufferUsage::ShaderRead);
    const BufferDesc structured = Desc(256, BufferUsage::ShaderRead | BufferUsage::Structured, 16);
    BufferViewDesc typed;
    typed.format = PixelFormat::R32Uint;
    // 용도 위반: 쓰기 용도 없는 UAV, 셰이더 용도 없는 버퍼.
    BufferViewDesc write = typed;
    write.access = ViewAccess::ReadWrite;
    CHECK(rejected(readOnly, write));
    CHECK(rejected(Desc(256, BufferUsage::Vertex), typed));
    // 형식 위반: 없음·압축·깊이·다중 plane, 구조화 버퍼의 typed view.
    BufferViewDesc format = typed;
    format.format = PixelFormat::Unknown;
    CHECK(rejected(readOnly, format));
    format.format = PixelFormat::BC1Unorm;
    CHECK(rejected(readOnly, format));
    format.format = PixelFormat::D32Float;
    CHECK(rejected(readOnly, format));
    format.format = PixelFormat::NV12;
    CHECK(rejected(readOnly, format));
    CHECK(rejected(structured, typed));
    // 종류 위반: 구조화 아닌 버퍼의 구조화 view, raw 아닌 버퍼의 raw view, 형식 지정.
    BufferViewDesc structuredView;
    structuredView.kind = BufferViewKind::Structured;
    CHECK(rejected(readOnly, structuredView));
    structuredView.format = PixelFormat::R32Uint;
    CHECK(rejected(structured, structuredView));
    BufferViewDesc raw;
    raw.kind = BufferViewKind::Raw;
    CHECK(rejected(readOnly, raw));
    // 범위 위반: 원소 정렬, raw 16 byte, 버퍼 밖, offset만 주고 개수 0, 끝을 넘는 개수.
    BufferViewDesc range = typed;
    range.offset = 2;
    range.elementCount = 1;
    CHECK(rejected(readOnly, range));
    raw.offset = 8;
    raw.elementCount = 1;
    CHECK(rejected(Desc(64, BufferUsage::ShaderRead | BufferUsage::Raw), raw));
    range.offset = 256;
    CHECK(rejected(readOnly, range));
    range.offset = 16;
    range.elementCount = 0;
    CHECK(rejected(readOnly, range));
    range.offset = 0;
    range.elementCount = 65;
    CHECK(rejected(readOnly, range));
    structuredView.format = PixelFormat::Unknown;
    structuredView.offset = 8;
    structuredView.elementCount = 1;
    CHECK(rejected(structured, structuredView));
}

TEST(BufferView_ChecksDeviceLimits) {
    DeviceLimits limits;
    limits.texelBufferOffsetAlignment = 16;
    limits.storageBufferOffsetAlignment = 64;
    limits.texelBufferElements = 32;
    limits.storageBufferBytes = 128;
    BufferViewDesc typed;
    typed.format = PixelFormat::R32Uint;
    ResolvedBufferView resolved{0, 8, 4};
    CHECK(CheckBufferViewLimits(limits, PixelFormatCaps::TexelBufferRead, typed, resolved));
    // 형식 능력이 없으면 Unsupported, 쓰기 view는 TexelBufferWrite가 필요하다.
    CHECK(CheckBufferViewLimits(limits, PixelFormatCaps::None, typed, resolved).Error() ==
          ErrorCode::Unsupported);
    typed.access = ViewAccess::ReadWrite;
    CHECK(
        CheckBufferViewLimits(limits, PixelFormatCaps::TexelBufferRead, typed, resolved).Error() ==
        ErrorCode::Unsupported);
    typed.access = ViewAccess::Read;
    // 장치 정렬 위반은 InvalidArgument, 원소 수 한도 초과는 Unsupported다.
    CHECK(
        CheckBufferViewLimits(limits, PixelFormatCaps::TexelBufferRead, typed, {4, 8, 4}).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(CheckBufferViewLimits(limits, PixelFormatCaps::TexelBufferRead, typed, {0, 33, 4})
              .Error() == ErrorCode::Unsupported);
    BufferViewDesc storage;
    storage.kind = BufferViewKind::Structured;
    CHECK(CheckBufferViewLimits(limits, PixelFormatCaps::None, storage, {64, 4, 16}));
    CHECK(CheckBufferViewLimits(limits, PixelFormatCaps::None, storage, {32, 4, 16}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(CheckBufferViewLimits(limits, PixelFormatCaps::None, storage, {0, 9, 16}).Error() ==
          ErrorCode::Unsupported);
}

TEST(DrawBufferSlots_RequireMatchingViews) {
    DrawBufferSlots slots;
    slots.usedMask = 0b101;
    slots.kinds[0] = BufferViewKind::Typed;
    slots.typedShaderTypes[0] = PixelShaderType::Float;
    slots.kinds[2] = BufferViewKind::Structured;
    BoundBufferViewInfo bound[MaxDrawBufferViews] = {};
    CHECK(!DrawBufferSlotsSatisfied(slots, bound));
    bound[0] = {true, BufferViewKind::Typed, PixelShaderType::Float};
    bound[2] = {true, BufferViewKind::Structured, PixelShaderType::None};
    CHECK(DrawBufferSlotsSatisfied(slots, bound));
    // 값 계열이 다른 typed view, 다른 종류는 채우지 못한다. 쓰지 않는 slot은 상관없다.
    bound[0].shaderType = PixelShaderType::Uint;
    CHECK(!DrawBufferSlotsSatisfied(slots, bound));
    bound[0].shaderType = PixelShaderType::Float;
    bound[2].kind = BufferViewKind::Raw;
    CHECK(!DrawBufferSlotsSatisfied(slots, bound));
    bound[2].kind = BufferViewKind::Structured;
    bound[1] = {true, BufferViewKind::Raw, PixelShaderType::None};
    CHECK(DrawBufferSlotsSatisfied(slots, bound));
    CHECK(SampleCountBit(1) == 1u);
    CHECK(SampleCountBit(4) == 4u);
    CHECK(SampleCountBit(32) == 32u);
}

TEST(DrawBufferSlots_AddSlotAndStructureStride) {
    DrawBufferSlots slots;
    CHECK(AddDrawBufferSlot(slots, 0, BufferViewKind::Typed, PixelShaderType::Float));
    // 다른 단계가 같은 slot을 같은 종류로 읽으면 받고, 다른 종류·값 계열이면 거부한다.
    CHECK(AddDrawBufferSlot(slots, 0, BufferViewKind::Typed, PixelShaderType::Float));
    CHECK(!AddDrawBufferSlot(slots, 0, BufferViewKind::Typed, PixelShaderType::Uint));
    CHECK(!AddDrawBufferSlot(slots, 0, BufferViewKind::Raw, PixelShaderType::None));
    CHECK(
        !AddDrawBufferSlot(slots, MaxDrawBufferViews, BufferViewKind::Raw, PixelShaderType::None));
    // 구조화 stride는 둘 다 알 때 같아야 한다(0은 모름).
    CHECK(AddDrawBufferSlot(slots, 1, BufferViewKind::Structured, PixelShaderType::None, 32));
    CHECK(AddDrawBufferSlot(slots, 1, BufferViewKind::Structured, PixelShaderType::None, 0));
    CHECK(!AddDrawBufferSlot(slots, 1, BufferViewKind::Structured, PixelShaderType::None, 16));
    CHECK(slots.usedMask == 0b11u && slots.structureStrides[1] == 32u);

    BoundBufferViewInfo bound[MaxDrawBufferViews] = {};
    bound[0] = {true, BufferViewKind::Typed, PixelShaderType::Float, 0};
    bound[1] = {true, BufferViewKind::Structured, PixelShaderType::None, 16};
    CHECK(!DrawBufferSlotsSatisfied(slots, bound));
    bound[1].stride = 32;
    CHECK(DrawBufferSlotsSatisfied(slots, bound));
    // 셰이더 stride를 모르는 slot은 view stride를 대조하지 않는다.
    slots.structureStrides[1] = 0;
    bound[1].stride = 48;
    CHECK(DrawBufferSlotsSatisfied(slots, bound));
}
} // namespace buffer_view_tests_file
