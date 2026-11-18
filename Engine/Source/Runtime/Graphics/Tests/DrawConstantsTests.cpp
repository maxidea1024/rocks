#include <Base/NumericLimits.h>
#include <Graphics/DeviceTypes.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::graphics;

namespace {

struct TintConstants {
    float color[4];
};

struct ObjectConstants {
    float transform[16];
    float color[4];
};

} // namespace

TEST(DrawConstants_SizeRuleIsMultipleOf16UpTo128) {
    // 0(없음)부터 128 byte까지 16 byte 배수만 유효하다(RenderBase 결정 R7).
    CHECK(MaxDrawConstantBytes == RootConstantContractBytes);
    CHECK(MaxDrawConstantBytes == 128u);
    for (uint32 bytes = 0; bytes <= MaxDrawConstantBytes; bytes += DrawConstantAlignment) {
        CHECK(IsValidDrawConstantBytes(bytes));
    }

    CHECK(!IsValidDrawConstantBytes(4));
    CHECK(!IsValidDrawConstantBytes(DrawConstantAlignment + 8));
    CHECK(!IsValidDrawConstantBytes(MaxDrawConstantBytes - 4));
    CHECK(!IsValidDrawConstantBytes(MaxDrawConstantBytes + DrawConstantAlignment));
    CHECK(!IsValidDrawConstantBytes(NumericLimits<uint32>::Max()));
}

TEST(DrawConstants_PipelineRulesRejectInvalidSizes) {
    PipelineDesc graphicsDesc;
    CHECK(ValidatePipelineDesc(graphicsDesc).IsOk());
    graphicsDesc.drawConstantBytes = MaxDrawConstantBytes;
    CHECK(ValidatePipelineDesc(graphicsDesc).IsOk());
    graphicsDesc.drawConstantBytes = MaxDrawConstantBytes + DrawConstantAlignment;
    CHECK(ValidatePipelineDesc(graphicsDesc).Error() == ErrorCode::InvalidArgument);
    graphicsDesc.drawConstantBytes = 20;
    CHECK(ValidatePipelineDesc(graphicsDesc).Error() == ErrorCode::InvalidArgument);

    ComputePipelineDesc computeDesc;
    CHECK(ValidateComputePipelineDesc(computeDesc).IsOk());
    computeDesc.drawConstantBytes = sizeof(ObjectConstants);
    CHECK(ValidateComputePipelineDesc(computeDesc).IsOk());
    computeDesc.drawConstantBytes = MaxDrawConstantBytes + DrawConstantAlignment;
    CHECK(ValidateComputePipelineDesc(computeDesc).Error() == ErrorCode::InvalidArgument);
    computeDesc.drawConstantBytes = 8;
    CHECK(ValidateComputePipelineDesc(computeDesc).Error() == ErrorCode::InvalidArgument);
}

TEST(DrawConstants_BytesViewTheStructureInPlace) {
    // DrawConstantBytes는 복사 없이 구조체 저장소를 그대로 가리킨다(크기 규칙은 컴파일 시간 검사).
    const TintConstants tint = {{0.25f, 0.5f, 0.75f, 1.0f}};
    const Span<const uint8> tintBytes = DrawConstantBytes(tint);
    CHECK(tintBytes.Size() == sizeof(TintConstants));
    CHECK(tintBytes.Data() == reinterpret_cast<const uint8*>(&tint));

    ObjectConstants object = {};
    object.color[3] = 1.0f;
    const Span<const uint8> objectBytes = DrawConstantBytes(object);
    CHECK(objectBytes.Size() == 80u);
    float alpha = 0.0f;
    platform::MemoryCopy(&alpha, objectBytes.Data() + 76, sizeof(alpha));
    CHECK(alpha == 1.0f);
}
