#include <RenderBase/ShaderSet.h>

#include <Core/Assert.h>
#include <Platform/Atomics.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <stdio.h>
#include <string.h>

// 셰이더 집합(Docs/Design/RenderBase 9.2절 RB3)이다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace shader_set_detail {

// 집합 epoch 발급기다(0은 쓰지 않는다).
uint64 nextEpoch = 0;

uint64 NewEpoch() noexcept {
    return platform::AtomicFetchAdd(&nextEpoch, uint64{1}) + 1;
}

uint64 PackHandle(graphics::ShaderHandle handle) noexcept {
    return (static_cast<uint64>(handle.slot.index) << 32) | handle.slot.generation;
}

graphics::ShaderHandle UnpackHandle(uint64 bits) noexcept {
    graphics::ShaderHandle handle;
    handle.slot.index = static_cast<uint32>(bits >> 32);
    handle.slot.generation = static_cast<uint32>(bits);
    return handle;
}

// 형식 backend가 장치 backend와 같은 코드 종류인가.
bool SameBackend(shader::ShaderBackend format, graphics::Backend device) noexcept {
    switch (format) {
    case shader::ShaderBackend::D3D12:
        return device == graphics::Backend::D3D12;
    case shader::ShaderBackend::D3D11:
        return device == graphics::Backend::D3D11;
    case shader::ShaderBackend::Vulkan:
        return device == graphics::Backend::Vulkan;
    case shader::ShaderBackend::Metal:
        return device == graphics::Backend::Metal;
    case shader::ShaderBackend::OpenGL:
        return device == graphics::Backend::OpenGL;
    }

    return false;
}

graphics::ShaderStage StageOf(uint32 stage) noexcept {
    switch (stage) {
    case shader::ShaderStagePixel:
        return graphics::ShaderStage::Pixel;
    case shader::ShaderStageCompute:
        return graphics::ShaderStage::Compute;
    default:
        return graphics::ShaderStage::Vertex;
    }
}

} // namespace shader_set_detail

using namespace shader_set_detail;

const char* ShaderSetStateName(ShaderSetState state) noexcept {
    switch (state) {
    case ShaderSetState::Closed:
        return "Closed";
    case ShaderSetState::WaitingForProvider:
        return "WaitingForProvider";
    case ShaderSetState::Ready:
        return "Ready";
    case ShaderSetState::Failed:
        return "Failed";
    case ShaderSetState::Retired:
        return "Retired";
    }

    return "Unknown";
}

const char* ShaderSetProblemName(ShaderSetProblem problem) noexcept {
    switch (problem) {
    case ShaderSetProblem::None:
        return "None";
    case ShaderSetProblem::CorruptLibrary:
        return "CorruptLibrary";
    case ShaderSetProblem::UnsupportedLibraryVersion:
        return "UnsupportedLibraryVersion";
    case ShaderSetProblem::WrongFormat:
        return "WrongFormat";
    case ShaderSetProblem::TypeSetMismatch:
        return "TypeSetMismatch";
    case ShaderSetProblem::MissingEntry:
        return "MissingEntry";
    case ShaderSetProblem::SignatureMismatch:
        return "SignatureMismatch";
    case ShaderSetProblem::CorruptEntry:
        return "CorruptEntry";
    case ShaderSetProblem::NoProvider:
        return "NoProvider";
    case ShaderSetProblem::ProviderFailed:
        return "ProviderFailed";
    }

    return "Unknown";
}

ShaderSet::ShaderSet(const ShaderSetDesc& desc) noexcept
    : allocator_(*desc.allocator), desc_(desc), epoch_(NewEpoch()), bytes_(*desc.allocator),
      entries_(*desc.allocator) {
}

ShaderSet::~ShaderSet() noexcept {
    ROCKS_ASSERT(leases_ == 0);
}

Result<void> ShaderSet::FailWith(ShaderSetProblem problem, const char* detail) noexcept {
    problem_ = problem;
    state_ = ShaderSetState::Failed;
    snprintf(detail_, sizeof(detail_), "%s", detail);
    entries_.Clear();
    view_.Close();
    ErrorCode code = ErrorCode::DependencyFailed;
    if (problem == ShaderSetProblem::NoProvider) {
        code = ErrorCode::UnsupportedCapability;
    }

    return Result<void>::Failure(code);
}

void ShaderSet::Settle() noexcept {
    settled_.Signal();
}

Result<void> ShaderSet::OpenCooked(Array<uint8>&& library) noexcept {
    platform::LockGuard lock(gate_);
    if (state_ != ShaderSetState::Closed) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    bytes_ = Move(library);
    return Validate();
}

Result<void> ShaderSet::OpenFromProvider(ShaderProvider* provider) noexcept {
    {
        platform::LockGuard lock(gate_);
        if (state_ != ShaderSetState::Closed) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        if (provider == nullptr) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "format %.*s needs a development shader provider and this executable has none",
                     static_cast<int>(desc_.formatId.ByteCount()), desc_.formatId.Data());
            return FailWith(ShaderSetProblem::NoProvider, detail);
        }

        state_ = ShaderSetState::WaitingForProvider;
        settled_.Reset();
    }

    ShaderProvisionRequest request;
    request.formatId = desc_.formatId;
    request.library = desc_.library;
    request.registry = desc_.registry;
    request.vertexInputTypes = desc_.vertexInputTypes;
    auto requested = provider->RequestLibrary(request, *this);
    if (!requested) {
        platform::LockGuard lock(gate_);
        auto failed = FailWith(ShaderSetProblem::ProviderFailed,
                               "the shader provider refused the library request");
        (void)failed;
        Settle();
        return requested;
    }

    platform::LockGuard lock(gate_);
    if (state_ == ShaderSetState::Failed) {
        return Result<void>::Failure(problem_ == ShaderSetProblem::NoProvider
                                         ? ErrorCode::UnsupportedCapability
                                         : ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

void ShaderSet::CompleteProvision(Result<void> outcome, Array<uint8>&& library,
                                  StringView diagnostics) noexcept {
    platform::LockGuard lock(gate_);
    if (state_ != ShaderSetState::WaitingForProvider) {
        return;
    }

    if (!outcome) {
        char detail[sizeof(detail_)];
        snprintf(detail, sizeof(detail), "shader provider failed: %.*s",
                 static_cast<int>(diagnostics.ByteCount() < 200 ? diagnostics.ByteCount() : 200),
                 diagnostics.Data());
        auto failed = FailWith(ShaderSetProblem::ProviderFailed, detail);
        (void)failed;
        Settle();
        return;
    }

    bytes_ = Move(library);
    auto validated = Validate();
    (void)validated;
    Settle();
}

ShaderSetState ShaderSet::WaitUntilSettled() noexcept {
    (void)settled_.Wait();
    return State();
}

ShaderSetState ShaderSet::State() const noexcept {
    platform::LockGuard lock(gate_);
    return state_;
}

ShaderSetProblem ShaderSet::Problem() const noexcept {
    platform::LockGuard lock(gate_);
    return problem_;
}

StringView ShaderSet::ProblemDetail() const noexcept {
    platform::LockGuard lock(gate_);
    return StringView(detail_);
}

// gate_를 잡고 부른다. bytes_를 열어 9.2절 검사를 하고 항목 표를 만든다.
Result<void> ShaderSet::Validate() noexcept {
    if (desc_.device == nullptr || desc_.registry == nullptr || !desc_.registry->IsFrozen()) {
        return FailWith(ShaderSetProblem::WrongFormat,
                        "shader set needs a device and a frozen shader type registry");
    }

    char detail[sizeof(detail_)];
    const shader::ShaderReadStatus opened = view_.Open(bytes_.View());
    if (!opened) {
        snprintf(detail, sizeof(detail), "shader library cannot be read: %s", opened.reason);
        return FailWith(opened.error == shader::ShaderReadError::UnsupportedVersion
                            ? ShaderSetProblem::UnsupportedLibraryVersion
                            : ShaderSetProblem::CorruptLibrary,
                        detail);
    }

    const shader::ShaderFormatFacts& facts = view_.Facts();
    if (facts.FormatId() != desc_.formatId || view_.Kind() != desc_.library ||
        !SameBackend(facts.backend, desc_.device->Info().backend)) {
        snprintf(detail, sizeof(detail),
                 "shader library is %s %.*s but the set expects %s %.*s for a %s device",
                 shader::ShaderLibraryKindName(view_.Kind()),
                 static_cast<int>(facts.FormatId().ByteCount()), facts.FormatId().Data(),
                 shader::ShaderLibraryKindName(desc_.library),
                 static_cast<int>(desc_.formatId.ByteCount()), desc_.formatId.Data(),
                 graphics::BackendName(desc_.device->Info().backend));
        return FailWith(ShaderSetProblem::WrongFormat, detail);
    }

    if (!(view_.TypeSetSha256() == desc_.registry->TypeSetDigest())) {
        return FailWith(ShaderSetProblem::TypeSetMismatch,
                        "shader library was cooked for a different shader type set than this "
                        "executable registers");
    }

    // 항목 표와 바인딩 layout이다.
    entries_.Clear();
    if (!entries_.TryReserve(view_.EntryCount())) {
        return FailWith(ShaderSetProblem::CorruptLibrary, "out of memory");
    }

    for (uint32 index = 0; index < view_.EntryCount(); ++index) {
        (void)entries_.TryPushBack(EntryState(allocator_));
        EntryState& state = entries_[index];
        state.record = view_.Entry(index);
        const Span<const uint8> layout =
            view_.Value(state.record, shader::ShaderLibraryValueBindingLayout);
        const shader::ShaderReadStatus decoded =
            shader::DecodeShaderBindingLayout(layout, state.layout);
        if (!decoded || state.layout.stage != state.record.stage) {
            const StringView name = view_.TypeName(state.record.key.nameHash);
            snprintf(
                detail, sizeof(detail), "shader %.*s permutation %u has a bad binding layout: %s",
                static_cast<int>(name.ByteCount()), name.Data(), state.record.key.permutationId,
                decoded ? "stage differs from the entry" : decoded.reason);
            return FailWith(ShaderSetProblem::CorruptEntry, detail);
        }
    }

    // 완결성과 서명이다(개발 전용 항목은 검사하지 않는다 — 9.2절).
    Array<shader::ShaderVertexInputType> inputs(allocator_);
    Array<shader::ShaderLibraryCombination> combinations(allocator_);
    if (!shader::SortShaderVertexInputTypes(desc_.vertexInputTypes, inputs) ||
        !shader::EnumerateShaderLibraryCombinations(desc_.registry->Types(), inputs.View(), facts,
                                                    desc_.library, false, combinations)) {
        return FailWith(ShaderSetProblem::WrongFormat,
                        "shader set cannot enumerate the registered shader types (vertex input "
                        "types or permutation domains are invalid)");
    }

    for (const shader::ShaderLibraryCombination& combination : combinations) {
        shader::ShaderLibraryEntry entry;
        const StringView name = combination.type->name;
        if (!view_.Find(shader::ShaderLibraryCombinationKey(combination), entry)) {
            snprintf(detail, sizeof(detail),
                     "shader library has no entry for %.*s vertex input %llu permutation %u",
                     static_cast<int>(name.ByteCount()), name.Data(),
                     static_cast<unsigned long long>(combination.vertexInputType),
                     combination.permutationId);
            return FailWith(ShaderSetProblem::MissingEntry, detail);
        }

        const Sha256Digest signature =
            shader::ShaderParameterLayoutSignature(combination.type->parameterLayouts);
        if (!(entry.layoutSignature == signature) || entry.stage != combination.type->stage) {
            snprintf(detail, sizeof(detail),
                     "shader %.*s permutation %u was cooked with a different parameter layout "
                     "signature or stage than this executable",
                     static_cast<int>(name.ByteCount()), name.Data(), combination.permutationId);
            return FailWith(ShaderSetProblem::SignatureMismatch, detail);
        }
    }

    problem_ = ShaderSetProblem::None;
    detail_[0] = '\0';
    state_ = ShaderSetState::Ready;
    return Result<void>::Success();
}

Result<uint32> ShaderSet::FindEntry(uint64 typeNameHash, shader::VertexInputTypeId vertexInputType,
                                    uint32 permutationId) const noexcept {
    platform::LockGuard lock(gate_);
    if (state_ != ShaderSetState::Ready) {
        return Result<uint32>::Failure(ErrorCode::NotReady);
    }

    // 항목 표는 키 순이다(ShaderLibraryView 규칙) — 이진 탐색.
    const shader::ShaderLibraryKey key{0, typeNameHash, vertexInputType, permutationId};
    usize low = 0;
    usize high = entries_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const shader::ShaderLibraryKey& candidate = entries_[middle].record.key;
        if (shader::ShaderLibraryKeyLess(candidate, key)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low < entries_.Size() && entries_[low].record.key == key) {
        return Result<uint32>::Success(static_cast<uint32>(low));
    }

    return Result<uint32>::Failure(ErrorCode::NotFound);
}

const shader::ShaderLibraryEntry* ShaderSet::Entry(uint32 entry) const noexcept {
    return entry < entries_.Size() ? &entries_[entry].record : nullptr;
}

const shader::ShaderBindingLayout* ShaderSet::BindingLayout(uint32 entry) const noexcept {
    return entry < entries_.Size() ? &entries_[entry].layout : nullptr;
}

bool ShaderSet::RuntimeFeaturesSupported(uint32 features) const noexcept {
    const graphics::DeviceFeatures& device = desc_.device->Caps().features;
    if ((features & shader::ShaderFeatureVertexViewportLayerOutput) != 0 &&
        !device.vertexViewportLayerOutput) {
        return false;
    }

    // PS 진단 버퍼의 런타임 처리는 엔진 셰이더 SL2다(16.1절 SSD22). 그 전에는 장치가 받지 않는다.
    return (features & shader::ShaderFeatureDebugPixelUav) == 0;
}

Result<graphics::ShaderHandle> ShaderSet::DeviceShader(uint32 entry) noexcept {
    const EntryState* state = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (retired_) {
            return Result<graphics::ShaderHandle>::Failure(ErrorCode::Destroyed);
        }

        if (state_ != ShaderSetState::Ready) {
            return Result<graphics::ShaderHandle>::Failure(ErrorCode::NotReady);
        }

        if (entry >= entries_.Size()) {
            return Result<graphics::ShaderHandle>::Failure(ErrorCode::InvalidArgument);
        }

        state = &entries_[entry];
    }

    uint64* published = &const_cast<EntryState*>(state)->published;
    const uint64 existing = platform::AtomicLoad<platform::MemoryOrder::Acquire>(published);
    if (existing != 0) {
        return Result<graphics::ShaderHandle>::Success(UnpackHandle(existing));
    }

    if (!RuntimeFeaturesSupported(state->record.runtimeFeatures)) {
        return Result<graphics::ShaderHandle>::Failure(ErrorCode::Unsupported);
    }

    // 잠금 밖에서 만든다(UE ShaderResource.cpp의 지연 생성과 같다).
    graphics::ShaderCodeDesc code;
    code.kind = graphics::ShaderCodeKindOf(desc_.device->Info().backend);
    code.stage = StageOf(state->record.stage);
    code.code = view_.Value(state->record, shader::ShaderLibraryValueCode);
    code.bindingLayout = view_.Value(state->record, shader::ShaderLibraryValueBindingLayout);
    if (code.kind == graphics::ShaderCodeKind::Essl) {
        code.glesBindingHeader =
            view_.Value(state->record, shader::ShaderLibraryValueGlesCopyTable);
    }

    auto created = desc_.device->CreateShader(code);
    if (!created) {
        return created;
    }

    uint64 expected = 0;
    const uint64 bits = PackHandle(created.Value());
    platform::LockGuard lock(gate_);
    if (retired_) {
        desc_.device->Retire(created.Value());
        return Result<graphics::ShaderHandle>::Failure(ErrorCode::Destroyed);
    }

    if (!platform::AtomicCompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                               platform::MemoryOrder::Acquire>(published, expected,
                                                                               bits)) {
        // 경쟁에서 졌다: 게시된 것을 쓰고 만든 것은 놓는다(9.2절).
        desc_.device->Retire(created.Value());
        return Result<graphics::ShaderHandle>::Success(UnpackHandle(expected));
    }

    ++publishedCount_;
    return created;
}

uint32 ShaderSet::PublishedShaders() const noexcept {
    platform::LockGuard lock(gate_);
    return publishedCount_;
}

bool ShaderSet::TryAcquireLease() noexcept {
    platform::LockGuard lock(gate_);
    if (retired_ || state_ != ShaderSetState::Ready) {
        return false;
    }

    ++leases_;
    return true;
}

void ShaderSet::ReleaseLease() noexcept {
    platform::LockGuard lock(gate_);
    ROCKS_ASSERT(leases_ > 0);
    if (leases_ > 0) {
        --leases_;
    }
}

uint32 ShaderSet::LeaseCount() const noexcept {
    platform::LockGuard lock(gate_);
    return leases_;
}

void ShaderSet::Retire() noexcept {
    platform::LockGuard lock(gate_);
    if (retired_) {
        return;
    }

    retired_ = true;
    for (EntryState& state : entries_) {
        const uint64 bits = platform::AtomicExchange(&state.published, uint64{0});
        if (bits != 0) {
            desc_.device->Retire(UnpackHandle(bits));
        }
    }

    if (state_ == ShaderSetState::Ready) {
        state_ = ShaderSetState::Retired;
    }
}

} // namespace rocks::render
