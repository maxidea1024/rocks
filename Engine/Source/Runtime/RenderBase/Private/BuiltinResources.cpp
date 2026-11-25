#include <RenderBase/BuiltinResources.h>

#include <RenderBase/EngineShaderTypes.h>

// 내장 자원 팩이다(Docs/Design/RenderBase 11.4절 5·6항). 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace {

using graphics::BufferUsage;
using graphics::PixelFormat;
using graphics::TextureDimension;

// 기본 메시 크기(cm)의 절반이다(UE 기본 도형은 한 변 100 cm).
constexpr float HalfExtent = 50.0f;
// cube 면 수와 면마다의 정점·index 수다.
constexpr uint32 CubeFaces = 6;
constexpr uint32 QuadVertices = 4;
constexpr uint32 QuadIndices = 6;
// typed 버퍼 원소 byte다(RGBA32Float·R32Uint).
constexpr uint32 Float4Bytes = 16;
constexpr uint32 UintBytes = 4;
// identity affine 행렬의 열 수(행 벡터 4×3 — 결정 R18)다.
constexpr uint32 AffineColumns = 3;
// RGBA8 텍셀 하나의 byte다.
constexpr uint32 Rgba8Bytes = 4;

constexpr graphics::VertexAttribute VertexAttributes[] = {
    {0, graphics::VertexFormat::Float3, 0, 0},
    {1, graphics::VertexFormat::Float2, 0, 12},
    {2, graphics::VertexFormat::Float4, 0, 20}};
constexpr graphics::VertexStream VertexStreams[] = {
    {sizeof(BuiltinVertex), graphics::VertexStepRate::Vertex}};

// 면 하나: 바깥 법선 n과 cross(u, v) = n인 축 u·v(왼손 좌표에서 화면 반시계가 되는 순서 — 11.4절
// 5항: 정점 (−u,−v)·(−u,+v)·(+u,+v)·(+u,−v)에 삼각형 (0,1,2)·(0,2,3)이면 cross(b − a, c − a)·n <
// 0).
struct Face {
    float normal[3];
    float u[3];
    float v[3];
};

constexpr Face CubeFaceAxes[CubeFaces] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   // +X
                                          {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // −X
                                          {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   // +Y
                                          {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // −Y
                                          {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +Z
                                          {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}}; // −Z

// 면 하나의 정점 넷을 out에 쓴다. UV는 (−u,−v) 모서리가 (0,1), (+u,+v)가 (1,0)이다.
constexpr void WriteQuad(const Face& face, float offset, BuiltinVertex* out) noexcept {
    constexpr float Signs[QuadVertices][2] = {{-1, -1}, {-1, 1}, {1, 1}, {1, -1}};
    for (uint32 corner = 0; corner < QuadVertices; ++corner) {
        BuiltinVertex& vertex = out[corner];
        const float su = Signs[corner][0];
        const float sv = Signs[corner][1];
        for (uint32 axis = 0; axis < 3; ++axis) {
            vertex.position[axis] =
                face.normal[axis] * offset + (face.u[axis] * su + face.v[axis] * sv) * HalfExtent;
        }

        vertex.texCoord[0] = (su + 1.0f) * 0.5f;
        vertex.texCoord[1] = (1.0f - sv) * 0.5f;
        for (float& channel : vertex.color) {
            channel = 1.0f;
        }
    }
}

struct CubeData {
    BuiltinVertex vertices[CubeFaces * QuadVertices];
    uint16 indices[CubeFaces * QuadIndices];
};

struct PlaneData {
    BuiltinVertex vertices[QuadVertices];
    uint16 indices[QuadIndices];
};

constexpr void WriteQuadIndices(uint16 base, uint16* out) noexcept {
    constexpr uint16 Order[QuadIndices] = {0, 1, 2, 0, 2, 3};
    for (uint32 index = 0; index < QuadIndices; ++index) {
        out[index] = static_cast<uint16>(base + Order[index]);
    }
}

constexpr CubeData MakeCube() noexcept {
    CubeData data{};
    for (uint32 face = 0; face < CubeFaces; ++face) {
        WriteQuad(CubeFaceAxes[face], HalfExtent, data.vertices + face * QuadVertices);
        WriteQuadIndices(static_cast<uint16>(face * QuadVertices),
                         data.indices + face * QuadIndices);
    }

    return data;
}

constexpr PlaneData MakePlane() noexcept {
    PlaneData data{};
    WriteQuad(CubeFaceAxes[4], 0.0f, data.vertices);
    WriteQuadIndices(0, data.indices);
    return data;
}

constexpr CubeData Cube = MakeCube();
constexpr PlaneData Plane = MakePlane();

// 1×1 텍스처 desc다.
graphics::TextureDesc OnePixel(TextureDimension dimension, PixelFormat format) noexcept {
    graphics::TextureDesc desc;
    desc.dimension = dimension;
    desc.width = 1;
    desc.height = 1;
    desc.format = format;
    desc.sampler.filter = graphics::Filter::Point;
    return desc;
}

struct TextureRecipe {
    BuiltinTexture id;
    TextureDimension dimension;
    PixelFormat format;
    uint8 texel[Rgba8Bytes];
    const char* name;
};

constexpr TextureRecipe ColorTextures[] = {{BuiltinTexture::White2D,
                                            TextureDimension::Texture2D,
                                            PixelFormat::RGBA8Unorm,
                                            {255, 255, 255, 255},
                                            "builtin white 2D texture"},
                                           {BuiltinTexture::Black2D,
                                            TextureDimension::Texture2D,
                                            PixelFormat::RGBA8Unorm,
                                            {0, 0, 0, 255},
                                            "builtin black 2D texture"},
                                           {BuiltinTexture::Transparent2D,
                                            TextureDimension::Texture2D,
                                            PixelFormat::RGBA8Unorm,
                                            {0, 0, 0, 0},
                                            "builtin transparent 2D texture"},
                                           {BuiltinTexture::FlatNormal2D,
                                            TextureDimension::Texture2D,
                                            PixelFormat::RGBA8Unorm,
                                            {128, 128, 255, 255},
                                            "builtin flat normal texture"},
                                           {BuiltinTexture::White2DArray,
                                            TextureDimension::Texture2DArray,
                                            PixelFormat::RGBA8Unorm,
                                            {255, 255, 255, 255},
                                            "builtin white 2D array texture"},
                                           {BuiltinTexture::White3D,
                                            TextureDimension::Texture3D,
                                            PixelFormat::RGBA8Unorm,
                                            {255, 255, 255, 255},
                                            "builtin white 3D texture"},
                                           {BuiltinTexture::BlackCube,
                                            TextureDimension::TextureCube,
                                            PixelFormat::RGBA8Unorm,
                                            {0, 0, 0, 255},
                                            "builtin black cube texture"},
                                           {BuiltinTexture::BlackCubeArray,
                                            TextureDimension::TextureCubeArray,
                                            PixelFormat::RGBA8Unorm,
                                            {0, 0, 0, 0},
                                            "builtin black cube array texture"}};

constexpr uint32 Index(BuiltinTexture texture) noexcept {
    return static_cast<uint32>(texture);
}

constexpr uint32 Index(BuiltinBuffer buffer) noexcept {
    return static_cast<uint32>(buffer);
}

constexpr uint32 Index(BuiltinMesh mesh) noexcept {
    return static_cast<uint32>(mesh);
}

constexpr BuiltinMeshBuffers EmptyMesh{};

} // namespace

graphics::VertexLayout BuiltinVertexLayout() noexcept {
    return graphics::VertexLayout{VertexAttributes, VertexStreams};
}

BuiltinMeshData GetBuiltinMeshData(BuiltinMesh mesh) noexcept {
    if (mesh == BuiltinMesh::Cube) {
        return BuiltinMeshData{Cube.vertices, Cube.indices};
    }

    return BuiltinMeshData{Plane.vertices, Plane.indices};
}

Result<void> BuiltinResources::Fail(graphics::Device& device, ErrorCode code,
                                    const char* reason) noexcept {
    failure_ = reason;
    Retire(device);
    return Result<void>::Failure(code);
}

Result<void> BuiltinResources::CreateTextures(graphics::Device& device) noexcept {
    const bool cubeArray = device.Caps().features.textureCubeArray;
    for (const TextureRecipe& recipe : ColorTextures) {
        if (recipe.dimension == TextureDimension::TextureCubeArray && !cubeArray) {
            continue;
        }

        graphics::TextureDesc desc = OnePixel(recipe.dimension, recipe.format);
        // cube 계열은 면 6(cube array는 cube 1 × 면 6)을 같은 값으로 채운다.
        const uint32 slices = graphics::TextureSliceCount(desc);
        uint8 pixels[CubeFaces * Rgba8Bytes] = {};
        for (uint32 slice = 0; slice < slices; ++slice) {
            for (uint32 channel = 0; channel < Rgba8Bytes; ++channel) {
                pixels[slice * Rgba8Bytes + channel] = recipe.texel[channel];
            }
        }

        auto created = device.CreateTexture(desc, Span<const uint8>(pixels, slices * Rgba8Bytes));
        if (!created) {
            return Fail(device, created.Error(), recipe.name);
        }

        textures_[Index(recipe.id)] = created.Value();
    }

    const uint32 zero = 0;
    auto uint2D = device.CreateTexture(
        OnePixel(TextureDimension::Texture2D, PixelFormat::R32Uint),
        Span<const uint8>(reinterpret_cast<const uint8*>(&zero), sizeof(zero)));
    if (!uint2D) {
        return Fail(device, uint2D.Error(), "builtin uint 2D texture");
    }

    textures_[Index(BuiltinTexture::ZeroUint2D)] = uint2D.Value();

    // 깊이는 초기 픽셀을 받지 않으므로 깊이 attachment clear 0으로 채운다(11.4절 5항).
    graphics::TextureDesc depthDesc = OnePixel(TextureDimension::Texture2D, PixelFormat::D32Float);
    depthDesc.usage = graphics::TextureUsage::DepthStencil;
    auto depth = device.CreateTexture(depthDesc, {});
    if (!depth) {
        return Fail(device, depth.Error(), "builtin far depth texture");
    }

    textures_[Index(BuiltinTexture::FarDepth2D)] = depth.Value();
    graphics::PassDesc pass;
    pass.colorCount = 0;
    pass.depth.texture = depth.Value();
    pass.depth.depthLoad = graphics::LoadAction::Clear;
    pass.depth.clearDepth = 0.0f;
    auto cleared = device.BeginPass(pass);
    if (!cleared) {
        return Fail(device, cleared.Error(), "builtin far depth texture clear");
    }

    if (auto ended = device.EndFrame(); !ended) {
        return Fail(device, ended.Error(), "builtin far depth texture clear");
    }

    return Result<void>::Success();
}

Result<void> BuiltinResources::CreateBuffers(graphics::Device& device) noexcept {
    struct BufferRecipe {
        BuiltinBuffer id;
        PixelFormat format;
        uint32 elementBytes;
        const float* values;
        uint32 elements;
        const char* name;
    };

    static constexpr float Zero[4] = {0, 0, 0, 0};
    static constexpr float One[4] = {1, 1, 1, 1};
    static constexpr float Identity[AffineColumns * 4] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    static constexpr uint32 ZeroUint[1] = {0};
    const BufferRecipe recipes[] = {
        {BuiltinBuffer::ZeroFloat4, PixelFormat::RGBA32Float, Float4Bytes, Zero, 1,
         "builtin zero float4 buffer"},
        {BuiltinBuffer::OneFloat4, PixelFormat::RGBA32Float, Float4Bytes, One, 1,
         "builtin one float4 buffer"},
        {BuiltinBuffer::ZeroUint, PixelFormat::R32Uint, UintBytes,
         reinterpret_cast<const float*>(ZeroUint), 1, "builtin zero uint buffer"},
        {BuiltinBuffer::IdentityAffine, PixelFormat::RGBA32Float, Float4Bytes, Identity,
         AffineColumns, "builtin identity affine buffer"}};
    for (const BufferRecipe& recipe : recipes) {
        graphics::BufferDesc desc;
        desc.size = static_cast<uint64>(recipe.elementBytes) * recipe.elements;
        desc.usage = BufferUsage::ShaderRead;
        auto created = device.CreateBuffer(
            desc, Span<const uint8>(reinterpret_cast<const uint8*>(recipe.values), desc.size));
        if (!created) {
            return Fail(device, created.Error(), recipe.name);
        }

        buffers_[Index(recipe.id)] = created.Value();
        graphics::BufferViewDesc view;
        view.kind = graphics::BufferViewKind::Typed;
        view.format = recipe.format;
        auto viewed = device.CreateBufferView(created.Value(), view);
        if (!viewed) {
            return Fail(device, viewed.Error(), recipe.name);
        }

        views_[Index(recipe.id)] = viewed.Value();
    }

    return Result<void>::Success();
}

Result<void> BuiltinResources::CreateMeshes(graphics::Device& device) noexcept {
    for (uint32 index = 0; index < BuiltinMeshCount; ++index) {
        const BuiltinMeshData data = GetBuiltinMeshData(static_cast<BuiltinMesh>(index));
        graphics::BufferDesc vertexDesc;
        vertexDesc.size = data.vertices.Size() * sizeof(BuiltinVertex);
        vertexDesc.usage = BufferUsage::Vertex;
        auto vertices = device.CreateBuffer(
            vertexDesc, Span<const uint8>(reinterpret_cast<const uint8*>(data.vertices.Data()),
                                          vertexDesc.size));
        if (!vertices) {
            return Fail(device, vertices.Error(), "builtin mesh vertex buffer");
        }

        meshes_[index].vertices = vertices.Value();
        graphics::BufferDesc indexDesc;
        indexDesc.size = data.indices.Size() * sizeof(uint16);
        indexDesc.usage = BufferUsage::Index;
        auto indices = device.CreateBuffer(
            indexDesc,
            Span<const uint8>(reinterpret_cast<const uint8*>(data.indices.Data()), indexDesc.size));
        if (!indices) {
            return Fail(device, indices.Error(), "builtin mesh index buffer");
        }

        meshes_[index].indices = indices.Value();
        meshes_[index].indexFormat = graphics::IndexFormat::UInt16;
        meshes_[index].vertexCount = static_cast<uint32>(data.vertices.Size());
        meshes_[index].indexCount = static_cast<uint32>(data.indices.Size());
    }

    return Result<void>::Success();
}

Result<void> BuiltinResources::PrepareShaders(ShaderSet& engineShaders) noexcept {
    struct ShaderRecipe {
        uint64 type;
        shader::VertexInputTypeId vertexInput;
        uint32* entry;
        const char* name;
    };

    const ShaderRecipe recipes[] = {
        {BuiltinMeshVertexType.hash, BuiltinVertexInputName.hash, &entries_.meshVertex,
         "fallback shader BuiltinMeshVertex (BuiltinVertexInput)"},
        {BuiltinMeshPixelType.hash, shader::NoVertexInputType, &entries_.meshPixel,
         "fallback shader BuiltinMeshPixel"},
        {FullscreenTriangleVertexType.hash, shader::NoVertexInputType, &entries_.fullscreenVertex,
         "fallback shader FullscreenTriangleVertex"},
        {CopyTexturePixelType.hash, shader::NoVertexInputType, &entries_.copyPixel,
         "fallback shader CopyTexturePixel"}};
    for (const ShaderRecipe& recipe : recipes) {
        auto entry = engineShaders.FindEntry(recipe.type, recipe.vertexInput);
        if (!entry) {
            failure_ = recipe.name;
            return Result<void>::Failure(entry.Error());
        }

        // 첫 사용 생성을 앞당긴다(실패가 시작 실패여야 한다 — 11.4절 5항).
        if (auto shader = engineShaders.DeviceShader(entry.Value()); !shader) {
            failure_ = recipe.name;
            return Result<void>::Failure(shader.Error());
        }

        *recipe.entry = entry.Value();
    }

    return Result<void>::Success();
}

Result<void> BuiltinResources::Create(graphics::Device& device, ShaderSet& engineShaders) noexcept {
    if (created_) {
        failure_ = "builtin resources are already created for this device";
        return Result<void>::Failure(ErrorCode::Busy);
    }

    failure_ = "";
    if (engineShaders.State() != ShaderSetState::Ready) {
        failure_ = "the engine shader set is not ready";
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    created_ = true;
    if (auto prepared = PrepareShaders(engineShaders); !prepared) {
        const char* reason = failure_;
        return Fail(device, prepared.Error(), reason);
    }

    if (auto textures = CreateTextures(device); !textures) {
        return textures;
    }

    if (auto buffers = CreateBuffers(device); !buffers) {
        return buffers;
    }

    if (auto meshes = CreateMeshes(device); !meshes) {
        return meshes;
    }

    ready_ = true;
    return Result<void>::Success();
}

void BuiltinResources::Retire(graphics::Device& device) noexcept {
    for (graphics::TextureHandle& texture : textures_) {
        if (!texture.IsNull()) {
            device.Retire(texture);
            texture = {};
        }
    }

    for (uint32 index = 0; index < BuiltinBufferCount; ++index) {
        if (!views_[index].IsNull()) {
            device.Retire(views_[index]);
            views_[index] = {};
        }

        if (!buffers_[index].IsNull()) {
            device.Retire(buffers_[index]);
            buffers_[index] = {};
        }
    }

    for (BuiltinMeshBuffers& mesh : meshes_) {
        if (!mesh.vertices.IsNull()) {
            device.Retire(mesh.vertices);
        }

        if (!mesh.indices.IsNull()) {
            device.Retire(mesh.indices);
        }

        mesh = BuiltinMeshBuffers{};
    }

    // 장치 셰이더는 엔진 셰이더 집합이 가진다(집합 Retire가 놓는다).
    entries_ = BuiltinShaderEntries{};
    created_ = false;
    ready_ = false;
}

graphics::TextureHandle BuiltinResources::Texture(BuiltinTexture texture) const noexcept {
    return Index(texture) < BuiltinTextureCount ? textures_[Index(texture)]
                                                : graphics::TextureHandle{};
}

graphics::BufferHandle BuiltinResources::Buffer(BuiltinBuffer buffer) const noexcept {
    return Index(buffer) < BuiltinBufferCount ? buffers_[Index(buffer)] : graphics::BufferHandle{};
}

graphics::BufferViewHandle BuiltinResources::BufferView(BuiltinBuffer buffer) const noexcept {
    return Index(buffer) < BuiltinBufferCount ? views_[Index(buffer)]
                                              : graphics::BufferViewHandle{};
}

const BuiltinMeshBuffers& BuiltinResources::Mesh(BuiltinMesh mesh) const noexcept {
    return Index(mesh) < BuiltinMeshCount ? meshes_[Index(mesh)] : EmptyMesh;
}

Result<graphics::TextureHandle>
BuiltinResources::FallbackTexture(shader::ShaderResourceShape shape,
                                  shader::ShaderComponentType component) const noexcept {
    using Shape = shader::ShaderResourceShape;
    if (!ready_) {
        return Result<graphics::TextureHandle>::Failure(ErrorCode::NotReady);
    }

    BuiltinTexture texture = BuiltinTexture::White2D;
    bool found = false;
    if (component == shader::ShaderComponentType::Float) {
        found = true;
        switch (shape) {
        case Shape::Texture2D:
            texture = BuiltinTexture::White2D;
            break;
        case Shape::Texture2DArray:
            texture = BuiltinTexture::White2DArray;
            break;
        case Shape::Texture3D:
            texture = BuiltinTexture::White3D;
            break;
        case Shape::TextureCube:
            texture = BuiltinTexture::BlackCube;
            break;
        case Shape::TextureCubeArray:
            texture = BuiltinTexture::BlackCubeArray;
            break;
        default:
            found = false;
            break;
        }
    } else if (component == shader::ShaderComponentType::Uint && shape == Shape::Texture2D) {
        texture = BuiltinTexture::ZeroUint2D;
        found = true;
    }

    const graphics::TextureHandle handle =
        found ? textures_[Index(texture)] : graphics::TextureHandle{};
    if (handle.IsNull()) {
        return Result<graphics::TextureHandle>::Failure(ErrorCode::Unsupported);
    }

    return Result<graphics::TextureHandle>::Success(handle);
}

Result<graphics::TextureHandle>
BuiltinResources::ResolveTexture(const RenderHandleTable& handles, RenderHandle handle,
                                 graphics::TextureHandle fallback) noexcept {
    if (fallback.IsNull()) {
        return Result<graphics::TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto state = handles.State(handle);
    if (!state) {
        return Result<graphics::TextureHandle>::Failure(state.Error());
    }

    switch (state.Value()) {
    case RenderResourceState::Ready:
        return handles.Texture(handle);
    case RenderResourceState::Pending:
    case RenderResourceState::Failed: {
        // 종류가 다른 handle은 대체하지 않는다.
        auto texture = handles.Texture(handle);
        if (!texture && texture.Error() == ErrorCode::TypeMismatch) {
            return texture;
        }

        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&fallbackUses_, 1);
        return Result<graphics::TextureHandle>::Success(fallback);
    }
    case RenderResourceState::Retired:
        break;
    }

    return Result<graphics::TextureHandle>::Failure(ErrorCode::StaleHandle);
}

} // namespace rocks::render
