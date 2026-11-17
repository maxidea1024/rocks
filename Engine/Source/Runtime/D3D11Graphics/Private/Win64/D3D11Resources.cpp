#include "D3D11DeviceImpl.h"

#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>

#include <d3d11shader.h>

namespace rocks::graphics::d3d11 {

Result<SwapChainHandle> D3D11Device::CreateSwapChain(const SwapChainDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<SwapChainHandle>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    if (desc.surface.window == nullptr || desc.width == 0 || desc.height == 0) {
        return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
    }

    DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
    swapDesc.Width = desc.width;
    swapDesc.Height = desc.height;
    // flip-model 버퍼는 _SRGB 형식을 쓸 수 없다. RTV를 sRGB로 만들어 쓰기 시 인코딩한다.
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = SwapChainBufferCount;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const HWND window = static_cast<HWND>(desc.surface.window);
    SwapChainRecord record;
    const HRESULT created = factory_->CreateSwapChainForHwnd(
        device_.Get(), window, &swapDesc, nullptr, nullptr, record.swapChain.Out());
    if (FAILED(created)) {
        return FailWith<SwapChainHandle>("CreateSwapChainForHwnd", created);
    }

    factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    record.width = desc.width;
    record.height = desc.height;
    record.vsync = desc.vsync;
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<SwapChainHandle>::Failure(reserved.Error());
    }

    auto view = CreateSwapChainView(record);
    if (!view) {
        return Result<SwapChainHandle>::Failure(view.Error());
    }

    auto added = swapChains_.TryAdd(Move(record));
    if (!added) {
        return Result<SwapChainHandle>::Failure(added.Error());
    }

    return Result<SwapChainHandle>::Success(SwapChainHandle{added.Value()});
}

Result<void> D3D11Device::ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                          uint32 height) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (width == 0 || height == 0 || recording_) {
        return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
    }

    // 버퍼를 참조하는 모든 GPU 작업이 끝난 뒤 버퍼를 놓고 크기를 바꾼다.
    auto idle = WaitIdleLocked();
    record = swapChains_.Find(handle.slot);
    if (!idle) {
        return idle;
    }

    record->rtv.Reset();
    record->buffer.Reset();
    // ResizeBuffers는 버퍼 참조가 모두 풀려야 한다. immediate context에 남은 묶음도 푼다.
    immediate_->ClearState();
    immediate_->Flush();
    // 이 시점부터 버퍼가 없으므로 성공할 때까지 그리기를 막는다.
    record->broken = true;
    const HRESULT resized = record->swapChain->ResizeBuffers(SwapChainBufferCount, width, height,
                                                             DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(resized)) {
        return Fail("IDXGISwapChain::ResizeBuffers", resized);
    }

    auto view = CreateSwapChainView(*record);
    if (!view) {
        return view;
    }

    record->width = width;
    record->height = height;
    record->broken = false;
    return Result<void>::Success();
}

Result<void> D3D11Device::CreateSwapChainView(SwapChainRecord& record) noexcept {
    // flip model에서 D3D11 runtime은 버퍼 0이 언제나 지금의 백버퍼를 가리키게 돌린다.
    HRESULT result = record.swapChain->GetBuffer(0, IID_PPV_ARGS(record.buffer.Out()));
    if (FAILED(result)) {
        return Fail("IDXGISwapChain::GetBuffer", result);
    }

    D3D11_RENDER_TARGET_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    result = device_->CreateRenderTargetView(record.buffer.Get(), &view, record.rtv.Out());
    if (FAILED(result)) {
        record.buffer.Reset();
        return Fail("CreateRenderTargetView (swap chain)", result);
    }

    return Result<void>::Success();
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 데이터는 생성 호출이 올린다.
Result<BufferHandle> D3D11Device::CreateBuffer(const BufferDesc& desc,
                                               Span<const uint8> initial) noexcept {
    if (desc.size == 0 || initial.Size() != desc.size ||
        (desc.usage != BufferUsage::Vertex && desc.usage != BufferUsage::Index) ||
        desc.size > NumericLimits<uint32>::Max()) {
        return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = static_cast<UINT>(desc.size);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags =
        desc.usage == BufferUsage::Index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = initial.Data();
    BufferRecord record;
    record.size = desc.size;
    const HRESULT created = device_->CreateBuffer(&bufferDesc, &data, record.buffer.Out());
    if (FAILED(created)) {
        return FailWith<BufferHandle>("ID3D11Device::CreateBuffer", created);
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<BufferHandle>::Failure(reserved.Error());
    }

    auto added = buffers_.TryAdd(Move(record));
    if (!added) {
        return Result<BufferHandle>::Failure(added.Error());
    }

    return Result<BufferHandle>::Success(BufferHandle{added.Value()});
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation). 초기 픽셀은 생성 호출이 올린다.
Result<TextureHandle> D3D11Device::CreateTexture(const TextureDesc& desc,
                                                 Span<const uint8> pixels) noexcept {
    // 올바른 형식이지만 대응 표에 없으면 미지원이다(다른 형식으로 대신하지 않는다). 크기 검사보다
    // 먼저 본다(깊이 형식처럼 업로드 크기가 없는 형식도 Unsupported로 알린다).
    if (IsValidPixelFormat(desc.format) && ResourceFormat(desc.format) == DXGI_FORMAT_UNKNOWN) {
        return Result<TextureHandle>::Failure(ErrorCode::Unsupported);
    }

    const bool renderTarget = desc.usage == TextureUsage::RenderTarget;
    const uint64 size = MipChainBytes(desc.format, desc.width, desc.height, 1, desc.mipLevels);
    if (desc.width > MaxTextureExtent || desc.height > MaxTextureExtent || size == 0 ||
        desc.mipLevels > MaxMipLevels || !IsValidSamplerDesc(desc.sampler) ||
        (renderTarget ? (pixels.Size() != 0 || desc.mipLevels != 1) : pixels.Size() != size)) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    auto sampler = SamplerFor(desc.sampler);
    if (!sampler) {
        return Result<TextureHandle>::Failure(sampler.Error());
    }

    TextureRecord record;
    record.width = desc.width;
    record.height = desc.height;
    record.mipLevels = desc.mipLevels;
    record.format = desc.format;
    record.renderTarget = renderTarget;
    record.sampler = sampler.Value();
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = desc.width;
    textureDesc.Height = desc.height;
    textureDesc.MipLevels = desc.mipLevels;
    textureDesc.ArraySize = 1;
    textureDesc.Format = ResourceFormat(desc.format);
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                            (renderTarget ? static_cast<UINT>(D3D11_BIND_RENDER_TARGET) : 0u);
    // 초기 픽셀(빈틈 없는 행, mip 0부터 이어 붙임)을 mip별 원본으로 나눈다.
    D3D11_SUBRESOURCE_DATA mips[MaxMipLevels] = {};
    // 블록 압축 형식의 행은 블록 행이다.
    usize offset = 0;
    for (uint32 level = 0; !renderTarget && level < desc.mipLevels; ++level) {
        const usize rowBytes =
            static_cast<usize>(PixelFormatRowBytes(desc.format, MipExtent(desc.width, level)));
        mips[level].pSysMem = pixels.Data() + offset;
        mips[level].SysMemPitch = static_cast<UINT>(rowBytes);
        offset += rowBytes * PixelFormatBlockRows(desc.format, MipExtent(desc.height, level));
    }

    HRESULT result =
        device_->CreateTexture2D(&textureDesc, renderTarget ? nullptr : mips, record.texture.Out());
    if (FAILED(result)) {
        return FailWith<TextureHandle>("ID3D11Device::CreateTexture2D", result);
    }

    result = device_->CreateShaderResourceView(record.texture.Get(), nullptr, record.srv.Out());
    if (FAILED(result)) {
        return FailWith<TextureHandle>("CreateShaderResourceView", result);
    }

    if (renderTarget) {
        result = device_->CreateRenderTargetView(record.texture.Get(), nullptr, record.rtv.Out());
        if (FAILED(result)) {
            return FailWith<TextureHandle>("CreateRenderTargetView", result);
        }
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<TextureHandle>::Failure(reserved.Error());
    }

    auto added = textures_.TryAdd(Move(record));
    if (!added) {
        return Result<TextureHandle>::Failure(added.Error());
    }

    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation, 내부 잠금으로 직렬화한다).
Result<PipelineHandle> D3D11Device::CreatePipeline(const PipelineDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    if (desc.vertex.hlsl.ByteCount() == 0 || desc.pixel.hlsl.ByteCount() == 0 ||
        !IsValidPixelFormat(desc.colorFormat)) {
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    if (ResourceFormat(desc.colorFormat) == DXGI_FORMAT_UNKNOWN) {
        return Result<PipelineHandle>::Failure(ErrorCode::Unsupported);
    }

    auto layoutChecked = ValidateVertexLayout(desc.layout);
    if (!layoutChecked) {
        auto failed = Fail("vertex layout is invalid (RenderBase 10.4 limits)");
        (void)failed;
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    ComRef<ID3DBlob> vertexCode;
    ComRef<ID3DBlob> pixelCode;
    auto vertex = Compile(desc.vertex.hlsl, "VSMain", "vs_5_0", vertexCode);
    if (!vertex) {
        return Result<PipelineHandle>::Failure(vertex.Error());
    }

    auto matched = MatchVertexInputs(vertexCode.Get(), desc.layout);
    if (!matched) {
        return Result<PipelineHandle>::Failure(matched.Error());
    }

    auto formats = CheckVertexFormats(desc.layout);
    if (!formats) {
        return Result<PipelineHandle>::Failure(formats.Error());
    }

    auto pixel = Compile(desc.pixel.hlsl, "PSMain", "ps_5_0", pixelCode);
    if (!pixel) {
        return Result<PipelineHandle>::Failure(pixel.Error());
    }

    PipelineRecord record;
    record.alphaBlend = desc.alphaBlend;
    if (static_cast<usize>(desc.cullMode) >= CullModeCount) {
        return Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    }

    record.cullMode = desc.cullMode;
    HRESULT result =
        device_->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
                                    nullptr, record.vertexShader.Out());
    if (FAILED(result)) {
        return FailWith<PipelineHandle>("CreateVertexShader", result);
    }

    result = device_->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                                        nullptr, record.pixelShader.Out());
    if (FAILED(result)) {
        return FailWith<PipelineHandle>("CreatePixelShader", result);
    }

    // 정점 입력 layout이다(UE D3D11VertexDeclaration.cpp:57–62와 같은 시맨틱·slot 배치).
    D3D11_INPUT_ELEMENT_DESC elements[MaxVertexAttributes] = {};
    for (usize index = 0; index < desc.layout.attributes.Size(); ++index) {
        const VertexAttribute& attribute = desc.layout.attributes[index];
        const VertexStream& stream = desc.layout.streams[attribute.stream];
        const bool instance = stream.stepRate == VertexStepRate::Instance;
        elements[index].SemanticName = "ATTRIBUTE";
        elements[index].SemanticIndex = attribute.location;
        elements[index].Format = AttributeFormat(attribute.format);
        elements[index].InputSlot = attribute.stream;
        elements[index].AlignedByteOffset = attribute.offset;
        elements[index].InputSlotClass =
            instance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        elements[index].InstanceDataStepRate = instance ? 1u : 0u;
    }

    if (desc.layout.attributes.Size() != 0) {
        result = device_->CreateInputLayout(
            elements, static_cast<UINT>(desc.layout.attributes.Size()),
            vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), record.inputLayout.Out());
        if (FAILED(result)) {
            return FailWith<PipelineHandle>("CreateInputLayout", result);
        }
    }

    for (usize index = 0; index < desc.layout.streams.Size(); ++index) {
        record.strides[index] = desc.layout.streams[index].stride;
        record.streamMask |= 1u << index;
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<PipelineHandle>::Failure(reserved.Error());
    }

    auto added = pipelines_.TryAdd(Move(record));
    if (!added) {
        return Result<PipelineHandle>::Failure(added.Error());
    }

    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

Result<ID3D11SamplerState*> D3D11Device::SamplerFor(const SamplerDesc& desc) noexcept {
    for (uint32 index = 0; index < samplerCount_; ++index) {
        if (samplers_[index].desc == desc) {
            return Result<ID3D11SamplerState*>::Success(samplers_[index].state.Get());
        }
    }

    if (samplerCount_ >= SamplerCapacity) {
        auto failed = Fail("sampler cache is full");
        (void)failed;
        return Result<ID3D11SamplerState*>::Failure(ErrorCode::CapacityExceeded);
    }

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = SamplerFilter(desc.filter);
    sampler.AddressU = SamplerAddress(desc.addressU);
    sampler.AddressV = SamplerAddress(desc.addressV);
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = desc.filter == Filter::Anisotropic ? desc.maxAnisotropy : 1u;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    BorderColorValue(desc.borderColor, sampler.BorderColor);
    SamplerEntry& entry = samplers_[samplerCount_];
    const HRESULT created = device_->CreateSamplerState(&sampler, entry.state.Out());
    if (FAILED(created)) {
        return FailWith<ID3D11SamplerState*>("CreateSamplerState", created);
    }

    entry.desc = desc;
    ++samplerCount_;
    return Result<ID3D11SamplerState*>::Success(entry.state.Get());
}

Result<void> D3D11Device::Compile(StringView source, const char* entry, const char* target,
                                  ComRef<ID3DBlob>& code) noexcept {
    ComRef<ID3DBlob> errors;
    // 행렬은 행 우선으로 담는다(DrawConstants 규약, ShaderSystem 7.2절 6항 FXC /Zpr). D3D12와 UE도
    // 같은 플래그다(D3DShaderCompiler.cpp:1337–1339).
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
    if (validation_) {
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    } else {
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }

    const HRESULT compiled = D3DCompile(source.Data(), source.ByteCount(), "RocksShader", nullptr,
                                        nullptr, entry, target, flags, 0, code.Out(), errors.Out());
    if (FAILED(compiled)) {
        // 파이프라인을 만드는 호출자가 LastError로 기록한다(LoggingSystem 13절). 진단은 앞쪽
        // MaxShaderDiagnosticLogBytes까지 싣는다.
        auto failed = Fail("D3DCompile", compiled);
        (void)failed;
        StringView diagnostics("no compiler output");
        if (errors) {
            diagnostics = StringView(static_cast<const char*>(errors->GetBufferPointer()),
                                     errors->GetBufferSize())
                              .LeftBytesAtCodePoint(MaxShaderDiagnosticLogBytes);
        }

        char storage[MaxShaderDiagnosticLogBytes + 128];
        FixedStringBuilder text{Span<char>(storage)};
        (void)text.TryAppendFormat("Shader {} ({}) failed to compile (HRESULT {:#x}): {}", entry,
                                   target, static_cast<uint32>(compiled), diagnostics);
        auto assigned = lastError_.TryAssign(text.View());
        (void)assigned;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

// 컴파일한 VS 입력 signature를 배치와 대조한다(RenderBase 10.4절 셰이더 입력 대조). 사용자 입력은
// 시맨틱 ATTRIBUTEn만 받고, n이 배치에 있어야 하며 성분 타입 계열이 형식의 셰이더 타입과 같아야
// 한다. 셰이더가 읽지 않는 속성은 허용한다.
Result<void> D3D11Device::MatchVertexInputs(ID3DBlob* code, const VertexLayout& layout) noexcept {
    ComRef<ID3D11ShaderReflection> reflection;
    HRESULT result =
        D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(reflection.Out()));
    if (FAILED(result)) {
        return Fail("D3DReflect (vertex shader)", result);
    }

    D3D11_SHADER_DESC shader = {};
    result = reflection->GetDesc(&shader);
    if (FAILED(result)) {
        return Fail("ID3D11ShaderReflection::GetDesc", result);
    }

    for (UINT index = 0; index < shader.InputParameters; ++index) {
        D3D11_SIGNATURE_PARAMETER_DESC input = {};
        result = reflection->GetInputParameterDesc(index, &input);
        if (FAILED(result)) {
            return Fail("ID3D11ShaderReflection::GetInputParameterDesc", result);
        }

        if (input.SystemValueType != D3D_NAME_UNDEFINED) {
            continue; // SV_VertexID·SV_InstanceID 등
        }

        Diagnostic message;
        if (input.SemanticName == nullptr || !IsAttributeSemantic(input.SemanticName) ||
            input.SemanticIndex >= MaxVertexAttributes) {
            message.Text("vertex shader input ")
                .Text(input.SemanticName != nullptr ? input.SemanticName : "?")
                .Number(input.SemanticIndex)
                .Text(" must use semantic ATTRIBUTE0-15");
            return Reject(message);
        }

        const VertexAttribute* attribute = nullptr;
        for (const VertexAttribute& candidate : layout.attributes) {
            if (candidate.location == input.SemanticIndex) {
                attribute = &candidate;
            }
        }

        if (attribute == nullptr) {
            message.Text("vertex shader reads location ")
                .Number(input.SemanticIndex)
                .Text(" which the vertex layout does not provide");
            return Reject(message);
        }

        VertexShaderType shaderType = VertexShaderType::Float;
        if (input.ComponentType == D3D_REGISTER_COMPONENT_SINT32) {
            shaderType = VertexShaderType::Int;
        } else if (input.ComponentType == D3D_REGISTER_COMPONENT_UINT32) {
            shaderType = VertexShaderType::Uint;
        }

        if (VertexFormatShaderType(attribute->format) != shaderType) {
            message.Text("vertex input location ")
                .Number(input.SemanticIndex)
                .Text(": format ")
                .Text(VertexFormatName(attribute->format))
                .Text(" feeds ")
                .Text(VertexShaderTypeName(VertexFormatShaderType(attribute->format)))
                .Text(" but the shader reads ")
                .Text(VertexShaderTypeName(shaderType));
            return Reject(message);
        }
    }

    return Result<void>::Success();
}

// 장치가 정점 입력으로 읽을 수 없는 형식은 Unsupported다(feature level 11_0에서 선택 기능인
// 형식이 있다).
Result<void> D3D11Device::CheckVertexFormats(const VertexLayout& layout) noexcept {
    for (const VertexAttribute& attribute : layout.attributes) {
        UINT support = 0;
        const HRESULT checked =
            device_->CheckFormatSupport(AttributeFormat(attribute.format), &support);
        if (FAILED(checked) || (support & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER) == 0) {
            Diagnostic message;
            message.Text("vertex format ")
                .Text(VertexFormatName(attribute.format))
                .Text(" is not supported as vertex input by this device");
            auto assigned = lastError_.TryAssign(StringView(message.CStr(), message.Length()));
            (void)assigned;
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    return Result<void>::Success();
}

void D3D11Device::Retire(BufferHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Defer(Move(record->buffer));
    auto removed = buffers_.TryRemove(handle.slot);
    (void)removed;
}

void D3D11Device::Retire(TextureHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr || handle == white_) {
        return;
    }

    Defer(Move(record->texture), Move(record->srv), Move(record->rtv));
    auto removed = textures_.TryRemove(handle.slot);
    (void)removed;
}

void D3D11Device::Retire(PipelineHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    PipelineRecord* record = pipelines_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    Defer(Move(record->vertexShader), Move(record->pixelShader), Move(record->inputLayout));
    auto removed = pipelines_.TryRemove(handle.slot);
    (void)removed;
}

void D3D11Device::Retire(SwapChainHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    platform::LockGuard lock(gate_);
    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    // 버퍼·RTV는 마지막으로 쓴 프레임이 끝난 뒤 놓는다. 기록 중인 프레임이 이 스왑체인에 그리고
    // 있어도 회수 항목이 참조를 쥐므로 그 프레임 실행까지 유효하다(present는 생략된다).
    Defer(Move(record->rtv), Move(record->buffer), Move(record->swapChain));
    auto removed = swapChains_.TryRemove(handle.slot);
    (void)removed;
}

} // namespace rocks::graphics::d3d11
