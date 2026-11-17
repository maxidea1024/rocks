#include <Graphics/DeviceTypes.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::graphics;

namespace {

bool Valid(Span<const VertexAttribute> attributes, Span<const VertexStream> streams) noexcept {
    return ValidateVertexLayout({attributes, streams}).IsOk();
}

bool Rejected(Span<const VertexAttribute> attributes, Span<const VertexStream> streams) noexcept {
    return ValidateVertexLayout({attributes, streams}).Error() == ErrorCode::InvalidArgument;
}

} // namespace

TEST(VertexLayout_AcceptsStreamsInstancesAndNoInput) {
    // 속성이 없는 배치(정점 입력 없음)도 유효하다.
    CHECK(Valid({}, {}));

    const VertexAttribute twoStreams[] = {{0, VertexFormat::Float3, 0, 0},
                                          {1, VertexFormat::UByte4Unorm, 1, 0},
                                          {2, VertexFormat::Half2, 1, 4}};
    const VertexStream streams[] = {{12, VertexStepRate::Vertex}, {8, VertexStepRate::Instance}};
    CHECK(Valid(twoStreams, streams));

    // 한계 값 그대로는 받는다: offset 2044(4 배수 최대) + 4 byte ≤ stride 2048, location 15,
    // stream 7.
    const VertexAttribute edge[] = {{15, VertexFormat::Float1, 0, 2044}};
    const VertexStream edgeStream[] = {{MaxVertexStride, VertexStepRate::Vertex}};
    CHECK(Valid(edge, edgeStream));

    VertexAttribute many[MaxVertexAttributes];
    for (uint32 index = 0; index < MaxVertexAttributes; ++index) {
        many[index] = {index, VertexFormat::Float1, index % MaxVertexStreams,
                       (index / MaxVertexStreams) * 4u};
    }

    VertexStream eight[MaxVertexStreams];
    for (VertexStream& stream : eight) {
        stream = {8, VertexStepRate::Vertex};
    }

    CHECK(Valid(many, eight));
}

TEST(VertexLayout_RejectsEveryLimitAndConsistencyViolation) {
    const VertexStream one[] = {{16, VertexStepRate::Vertex}};

    // location 중복·범위 밖.
    const VertexAttribute duplicate[] = {{1, VertexFormat::Float2, 0, 0},
                                         {1, VertexFormat::Float2, 0, 8}};
    CHECK(Rejected(duplicate, one));
    const VertexAttribute location16[] = {{MaxVertexAttributes, VertexFormat::Float1, 0, 0}};
    CHECK(Rejected(location16, one));

    // offset 한계·정렬, offset + 크기 > stride.
    const VertexStream wide[] = {{MaxVertexStride, VertexStepRate::Vertex}};
    const VertexAttribute farOffset[] = {
        {0, VertexFormat::Float1, 0, MaxVertexAttributeOffset + 1}};
    CHECK(Rejected(farOffset, wide));
    const VertexAttribute unaligned[] = {{0, VertexFormat::Float1, 0, 2}};
    CHECK(Rejected(unaligned, one));
    const VertexAttribute overflow[] = {{0, VertexFormat::Float4, 0, 4}};
    CHECK(Rejected(overflow, one));

    // stride 한계·정렬·0.
    const VertexAttribute first[] = {{0, VertexFormat::Float1, 0, 0}};
    const VertexStream tooWide[] = {{MaxVertexStride + 4, VertexStepRate::Vertex}};
    CHECK(Rejected(first, tooWide));
    const VertexStream oddStride[] = {{6, VertexStepRate::Vertex}};
    CHECK(Rejected(first, oddStride));
    const VertexStream zeroStride[] = {{0, VertexStepRate::Vertex}};
    CHECK(Rejected(first, zeroStride));

    // 속성이 가리키는 stream이 없거나, 선언한 stream에 속성이 없다.
    const VertexAttribute missingStream[] = {{0, VertexFormat::Float1, 1, 0}};
    CHECK(Rejected(missingStream, one));
    const VertexStream two[] = {{16, VertexStepRate::Vertex}, {16, VertexStepRate::Instance}};
    CHECK(Rejected(first, two));

    // 모르는 형식·속성 수·stream 수 초과.
    const VertexAttribute unknown[] = {{0, static_cast<VertexFormat>(0), 0, 0}};
    CHECK(Rejected(unknown, one));
    VertexAttribute seventeen[MaxVertexAttributes + 1];
    for (uint32 index = 0; index <= MaxVertexAttributes; ++index) {
        seventeen[index] = {index, VertexFormat::Float1, 0, index * 4u};
    }

    const VertexStream large[] = {{128, VertexStepRate::Vertex}};
    CHECK(Rejected(seventeen, large));
    VertexStream nine[MaxVertexStreams + 1];
    for (VertexStream& stream : nine) {
        stream = {4, VertexStepRate::Vertex};
    }

    CHECK(Rejected(first, nine));
}
