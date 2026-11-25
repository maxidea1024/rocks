#include "RecordingDevice.h"
#include "RegistryShaderSet.h"

#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/BuiltinResources.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/RenderLane.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>

// 내장 자원 팩 시험이다(Docs/Design/RenderBase 11.4절 5–7항, 6.2–6.4절): 기본 메시 CPU 데이터(크기·
// 감김·UV·색), 정점 배치, 팩 생성 내용·두 번째 생성 Busy·Retire, fallback 셰이더가 없는 집합의
// 생성 실패(자원 0), typed 대체 선택표와 Unsupported, ResolveTexture의 상태별 결과와 계수, 렌더
// lane 시작 단계의 팩 생성(성공하면 준비 완료, 실패하면 frame slot 확보가 그 오류).
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::render::testing::RegistryShaderSet;

namespace builtin_resources_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 메시 정점·index의 기대 수와 반 변(cm)이다.
constexpr usize CubeVertexCount = 24;
constexpr usize CubeIndexCount = 36;
constexpr usize PlaneVertexCount = 4;
constexpr usize PlaneIndexCount = 6;
constexpr float HalfExtent = 50.0f;

struct Vector {
    float x = 0;
    float y = 0;
    float z = 0;
};

Vector Of(const BuiltinVertex& vertex) noexcept {
    return Vector{vertex.position[0], vertex.position[1], vertex.position[2]};
}

Vector Minus(Vector a, Vector b) noexcept {
    return Vector{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector Cross(Vector a, Vector b) noexcept {
    return Vector{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Dot(Vector a, Vector b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Absolute(float value) noexcept {
    return value < 0.0f ? -value : value;
}

// 삼각형이 바깥 법선 쪽에서 보아 화면 반시계(앞면)인지다: 왼손 좌표에서 cross(b − a, c − a)가 보는
// 쪽의 반대(법선 반대)를 향한다(11.4절 5항).
bool FacesOutward(const BuiltinMeshData& data, usize triangle, Vector outward) noexcept {
    const Vector a = Of(data.vertices[data.indices[triangle * 3]]);
    const Vector b = Of(data.vertices[data.indices[triangle * 3 + 1]]);
    const Vector c = Of(data.vertices[data.indices[triangle * 3 + 2]]);
    return Dot(Cross(Minus(b, a), Minus(c, a)), outward) < 0.0f;
}

// cube 삼각형의 바깥 방향(세 정점 합의 가장 큰 축)이다.
Vector CubeOutward(const BuiltinMeshData& data, usize triangle) noexcept {
    Vector sum;
    for (usize corner = 0; corner < 3; ++corner) {
        const Vector p = Of(data.vertices[data.indices[triangle * 3 + corner]]);
        sum = Vector{sum.x + p.x, sum.y + p.y, sum.z + p.z};
    }

    if (Absolute(sum.x) >= Absolute(sum.y) && Absolute(sum.x) >= Absolute(sum.z)) {
        return Vector{sum.x > 0 ? 1.0f : -1.0f, 0, 0};
    }

    if (Absolute(sum.y) >= Absolute(sum.z)) {
        return Vector{0, sum.y > 0 ? 1.0f : -1.0f, 0};
    }

    return Vector{0, 0, sum.z > 0 ? 1.0f : -1.0f};
}

bool AllVerticesValid(const BuiltinMeshData& data, bool flat) noexcept {
    for (const BuiltinVertex& vertex : data.vertices) {
        for (usize axis = 0; axis < 3; ++axis) {
            const float coordinate = vertex.position[axis];
            const bool onPlane = flat && axis == 2 && coordinate == 0.0f;
            if (Absolute(coordinate) != HalfExtent && !onPlane) {
                return false;
            }
        }

        for (float uv : vertex.texCoord) {
            if (uv != 0.0f && uv != 1.0f) {
                return false;
            }
        }

        for (float channel : vertex.color) {
            if (channel != 1.0f) {
                return false;
            }
        }
    }

    return true;
}

// 엔진 셰이더 타입·정점 입력 타입을 등록한다. meshShaders가 거짓이면 전체화면 두 타입만이다.
bool RegisterEngine(shader::ShaderTypeRegistry& registry, bool meshShaders) noexcept {
    if (meshShaders) {
        return registry.Register(EngineShaderTypes()) &&
               registry.RegisterVertexInputTypes(EngineVertexInputTypes()) && registry.Freeze();
    }

    const shader::ShaderTypeDesc* const fullscreen[] = {EngineShaderTypes()[0],
                                                        EngineShaderTypes()[1]};
    return registry.Register(fullscreen) && registry.Freeze();
}

usize CountOps(const RecordingDevice& device, RecordingDevice::Op op) noexcept {
    usize count = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        count += call.op == op ? 1u : 0u;
    }

    return count;
}

// 렌더 lane 시작 단계에서 팩을 만드는 처리기다.
class PackProcessor final : public RenderLaneProcessor {
public:
    explicit PackProcessor(ShaderSet& shaders) noexcept : shaders_(shaders) {
    }

    Result<void> OnLaneStart(RenderLaneContext& context) noexcept override {
        return pack.Create(context.Device(), shaders_);
    }

    // 종료 4단계(5.5절): 내장 자원과 엔진 셰이더 집합을 Retire한다.
    void OnLaneStop(RenderLaneContext& context) noexcept override {
        pack.Retire(context.Device());
        shaders_.Retire();
    }

    BuiltinResources pack;

private:
    ShaderSet& shaders_;
};

// 기본 메시는 UE 기본 도형 크기(100 cm)이고 바깥에서 보아 모든 삼각형이 앞면이다.
TEST(RenderBase_BuiltinMeshDataAndLayout) {
    const BuiltinMeshData cube = GetBuiltinMeshData(BuiltinMesh::Cube);
    const BuiltinMeshData plane = GetBuiltinMeshData(BuiltinMesh::Plane);
    REQUIRE(cube.vertices.Size() == CubeVertexCount && cube.indices.Size() == CubeIndexCount);
    REQUIRE(plane.vertices.Size() == PlaneVertexCount && plane.indices.Size() == PlaneIndexCount);
    CHECK(AllVerticesValid(cube, false) && AllVerticesValid(plane, true));
    bool outward = true;
    for (usize triangle = 0; triangle < CubeIndexCount / 3; ++triangle) {
        outward = outward && FacesOutward(cube, triangle, CubeOutward(cube, triangle));
    }

    for (usize triangle = 0; triangle < PlaneIndexCount / 3; ++triangle) {
        outward = outward && FacesOutward(plane, triangle, Vector{0, 0, 1});
    }

    CHECK(outward);
    // 면마다 바깥 방향이 다르다(여섯 방향을 모두 덮는다).
    uint32 directions = 0;
    for (usize triangle = 0; triangle < CubeIndexCount / 3; ++triangle) {
        const Vector n = CubeOutward(cube, triangle);
        const uint32 bit = n.x > 0   ? 1u
                           : n.x < 0 ? 2u
                           : n.y > 0 ? 4u
                           : n.y < 0 ? 8u
                           : n.z > 0 ? 16u
                                     : 32u;
        directions |= bit;
    }

    CHECK(directions == 63u);
    for (uint16 index : cube.indices) {
        CHECK(index < CubeVertexCount);
    }

    // 배치: stream 0 stride 36, ATTRIBUTE0 Float3 0·ATTRIBUTE1 Float2 12·ATTRIBUTE2 Float4 20.
    const graphics::VertexLayout layout = BuiltinVertexLayout();
    REQUIRE(layout.attributes.Size() == 3 && layout.streams.Size() == 1);
    CHECK(layout.streams[0].stride == sizeof(BuiltinVertex) &&
          layout.streams[0].stepRate == graphics::VertexStepRate::Vertex);
    CHECK(layout.attributes[0].location == 0 &&
          layout.attributes[0].format == graphics::VertexFormat::Float3 &&
          layout.attributes[0].offset == 0);
    CHECK(layout.attributes[1].location == 1 &&
          layout.attributes[1].format == graphics::VertexFormat::Float2 &&
          layout.attributes[1].offset == 12);
    CHECK(layout.attributes[2].location == 2 &&
          layout.attributes[2].format == graphics::VertexFormat::Float4 &&
          layout.attributes[2].offset == 20);
    CHECK(graphics::ValidateVertexLayout(layout));
}

// 팩 내용·두 번째 생성 Busy·Retire 뒤 null, 대체 선택표, ResolveTexture 상태별 결과.
TEST(RenderBase_BuiltinResourcesCreateFallbackAndResolve) {
    RecordingDevice device(Allocator_());
    shader::ShaderTypeRegistry registry(Allocator_());
    REQUIRE(RegisterEngine(registry, true));
    REQUIRE(registry.FindVertexInputType(BuiltinVertexInputName.hash) != nullptr);
    RegistryShaderSet shaders(Allocator_(), device, registry);
    REQUIRE(shaders.ready);
    // 정점 입력 축 VS는 BuiltinVertexInput 항목만 있다.
    CHECK(shaders.set.FindEntry(BuiltinMeshVertexType.hash, BuiltinVertexInputName.hash));
    CHECK(shaders.set.FindEntry(BuiltinMeshVertexType.hash).Error() == ErrorCode::NotFound);

    BuiltinResources pack;
    CHECK(pack.FallbackTexture(shader::ShaderResourceShape::Texture2D,
                               shader::ShaderComponentType::Float)
              .Error() == ErrorCode::NotReady);
    REQUIRE(pack.Create(device, shaders.set));
    CHECK(pack.IsReady());
    CHECK(pack.Create(device, shaders.set).Error() == ErrorCode::Busy);
    const bool cubeArray = device.Caps().features.textureCubeArray;
    for (uint32 index = 0; index < BuiltinTextureCount; ++index) {
        const auto texture = static_cast<BuiltinTexture>(index);
        const bool expected = texture != BuiltinTexture::BlackCubeArray || cubeArray;
        CHECK(pack.Texture(texture).IsNull() == !expected);
    }

    for (uint32 index = 0; index < BuiltinBufferCount; ++index) {
        CHECK(!pack.Buffer(static_cast<BuiltinBuffer>(index)).IsNull() &&
              !pack.BufferView(static_cast<BuiltinBuffer>(index)).IsNull());
    }

    CHECK(pack.Mesh(BuiltinMesh::Cube).indexCount == CubeIndexCount &&
          pack.Mesh(BuiltinMesh::Cube).vertexCount == CubeVertexCount &&
          pack.Mesh(BuiltinMesh::Plane).indexCount == PlaneIndexCount &&
          pack.Mesh(BuiltinMesh::Cube).indexFormat == graphics::IndexFormat::UInt16);
    const BuiltinShaderEntries& entries = pack.ShaderEntries();
    CHECK(entries.meshVertex != NoShaderSetEntry && entries.meshPixel != NoShaderSetEntry &&
          entries.fullscreenVertex != NoShaderSetEntry && entries.copyPixel != NoShaderSetEntry);
    // 깊이 대체는 clear pass 하나로 채운다.
    CHECK(CountOps(device, RecordingDevice::Op::EndFrame) == 1);

    using Shape = shader::ShaderResourceShape;
    using Component = shader::ShaderComponentType;
    const auto fallback = [&pack](Shape shape, Component component) noexcept {
        return pack.FallbackTexture(shape, component);
    };
    CHECK(fallback(Shape::Texture2D, Component::Float).Value() ==
          pack.Texture(BuiltinTexture::White2D));
    CHECK(fallback(Shape::Texture2DArray, Component::Float).Value() ==
          pack.Texture(BuiltinTexture::White2DArray));
    CHECK(fallback(Shape::Texture3D, Component::Float).Value() ==
          pack.Texture(BuiltinTexture::White3D));
    CHECK(fallback(Shape::TextureCube, Component::Float).Value() ==
          pack.Texture(BuiltinTexture::BlackCube));
    CHECK(fallback(Shape::Texture2D, Component::Uint).Value() ==
          pack.Texture(BuiltinTexture::ZeroUint2D));
    if (cubeArray) {
        CHECK(fallback(Shape::TextureCubeArray, Component::Float).Value() ==
              pack.Texture(BuiltinTexture::BlackCubeArray));
    } else {
        CHECK(fallback(Shape::TextureCubeArray, Component::Float).Error() ==
              ErrorCode::Unsupported);
    }

    // 맞는 대체가 없는 조합은 Unsupported다(2D white를 다른 모양에 넣지 않는다).
    const Shape unsupportedShapes[] = {Shape::None,
                                       Shape::Texture1D,
                                       Shape::Texture1DArray,
                                       Shape::Texture2DMultisample,
                                       Shape::Texture2DMultisampleArray,
                                       Shape::TypedBuffer,
                                       Shape::StructuredBuffer,
                                       Shape::RawBuffer};
    for (Shape shape : unsupportedShapes) {
        CHECK(fallback(shape, Component::Float).Error() == ErrorCode::Unsupported);
    }

    CHECK(fallback(Shape::TextureCube, Component::Uint).Error() == ErrorCode::Unsupported);
    CHECK(fallback(Shape::Texture2D, Component::Int).Error() == ErrorCode::Unsupported);
    CHECK(fallback(Shape::Texture2D, Component::None).Error() == ErrorCode::Unsupported);

    // ResolveTexture: Ready는 그 텍스처, Pending·Failed는 대체와 계수, Retired·모르는 handle은
    // StaleHandle, 버퍼 handle은 TypeMismatch, null 대체는 InvalidArgument.
    RenderHandleTable handles(Allocator_());
    const graphics::TextureHandle white = pack.Texture(BuiltinTexture::White2D);
    const graphics::TextureHandle black = pack.Texture(BuiltinTexture::Black2D);
    auto pending = handles.Reserve(RenderResourceKind::Texture);
    auto failed = handles.Reserve(RenderResourceKind::Texture);
    auto ready = handles.Reserve(RenderResourceKind::Texture);
    auto buffer = handles.Reserve(RenderResourceKind::Buffer);
    REQUIRE(pending && failed && ready && buffer);
    REQUIRE(handles.MarkFailed(failed.Value(), ErrorCode::Unsupported));
    REQUIRE(handles.BindTexture(ready.Value(), black));
    CHECK(pack.ResolveTexture(handles, pending.Value(), white).Value() == white);
    CHECK(pack.ResolveTexture(handles, failed.Value(), white).Value() == white);
    CHECK(pack.FallbackUses() == 2);
    CHECK(pack.ResolveTexture(handles, ready.Value(), white).Value() == black);
    CHECK(pack.FallbackUses() == 2);
    CHECK(pack.ResolveTexture(handles, buffer.Value(), white).Error() == ErrorCode::TypeMismatch);
    CHECK(pack.ResolveTexture(handles, pending.Value(), graphics::TextureHandle{}).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(handles.BeginRetire(ready.Value()));
    CHECK(pack.ResolveTexture(handles, ready.Value(), white).Error() == ErrorCode::StaleHandle);
    CHECK(pack.ResolveTexture(handles, RenderHandle{}, white).Error() == ErrorCode::StaleHandle);
    CHECK(pack.FallbackUses() == 2);

    // Retire하면 모두 null이고 다시 만들 수 있다.
    pack.Retire(device);
    CHECK(!pack.IsReady() && pack.Texture(BuiltinTexture::White2D).IsNull() &&
          pack.Buffer(BuiltinBuffer::ZeroFloat4).IsNull() &&
          pack.Mesh(BuiltinMesh::Plane).vertices.IsNull());
    REQUIRE(pack.Create(device, shaders.set));
    pack.Retire(device);
    shaders.set.Retire();
    device.CompleteAll();
    device.ReclaimCompleted();
    CHECK(device.PendingRetireCount() == 0);
}

// fallback 메시 셰이더가 없는 엔진 셰이더 집합이면 생성이 실패하고 자원을 만들지 않는다. 렌더 lane
// 시작 단계에서 쓰면 준비 완료가 게시되지 않고 frame slot 확보가 그 오류로 실패한다(6.3절).
TEST(RenderBase_BuiltinResourcesStartGatesBuiltinReady) {
    {
        RecordingDevice device(Allocator_());
        shader::ShaderTypeRegistry registry(Allocator_());
        REQUIRE(RegisterEngine(registry, false));
        RegistryShaderSet shaders(Allocator_(), device, registry);
        REQUIRE(shaders.ready);
        BuiltinResources pack;
        CHECK(pack.Create(device, shaders.set).Error() == ErrorCode::NotFound);
        CHECK(StringView(pack.LastFailure()).Contains(StringView("BuiltinMeshVertex")));
        CHECK(!pack.IsReady() && CountOps(device, RecordingDevice::Op::CreateTexture) == 0 &&
              CountOps(device, RecordingDevice::Op::CreateBuffer) == 0);

        PackProcessor processor(shaders.set);
        RenderLane lane(Allocator_(), device);
        RenderLaneConfig config;
        config.mode = RenderLaneMode::Inline;
        config.processor = &processor;
        config.hangTimeoutMs = 0;
        REQUIRE(lane.Start(config));
        CHECK(lane.BeginFrame().Error() == ErrorCode::NotFound);
        CHECK(!lane.IsReady() && !processor.pack.IsReady());
        (void)lane.Shutdown();
        shaders.set.Retire();
    }

    RecordingDevice device(Allocator_());
    shader::ShaderTypeRegistry registry(Allocator_());
    REQUIRE(RegisterEngine(registry, true));
    RegistryShaderSet shaders(Allocator_(), device, registry);
    REQUIRE(shaders.ready);
    PackProcessor processor(shaders.set);
    RenderLane lane(Allocator_(), device);
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Inline;
    config.processor = &processor;
    config.hangTimeoutMs = 0;
    REQUIRE(lane.Start(config));
    auto ticket = lane.BeginFrame();
    REQUIRE(ticket);
    CHECK(lane.IsReady() && processor.pack.IsReady());
    lane.AbandonFrame(Move(ticket.Value()));
    CHECK(lane.Shutdown());
    CHECK(!processor.pack.IsReady());
    shaders.set.Retire();
}

} // namespace builtin_resources_tests_file
