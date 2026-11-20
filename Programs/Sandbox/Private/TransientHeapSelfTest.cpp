#include "TransientHeapSelfTest.h"

#include <Core/Containers/Array.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 시험 렌더 타깃의 한 변과 픽셀 byte(RGBA8)다.
constexpr uint32 Extent = 32;
constexpr uint32 PixelBytes = 4;
// 시험 버퍼 byte와 두 원본 버퍼의 채움 값이다.
constexpr uint64 BufferBytes = 256;
constexpr uint8 FirstFill = 0x11;
constexpr uint8 SecondFill = 0x22;
// 8 bit 색 성분의 최댓값이다.
constexpr uint8 FullChannel = 255;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s transient heaps: %s\n", backendName, what);
    }
}

void PrintLastError(Device& device) noexcept {
    printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

TextureDesc TargetDesc() noexcept {
    TextureDesc desc;
    desc.width = Extent;
    desc.height = Extent;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.usage = TextureUsage::RenderTarget;
    return desc;
}

// placed 버퍼(그래프의 쓰기 버퍼와 같은 용도)다.
BufferDesc PlacedBufferDesc() noexcept {
    BufferDesc desc;
    desc.size = BufferBytes;
    desc.usage = BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Raw;
    return desc;
}

uint64 AlignUp(uint64 value, uint64 alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

// 모든 픽셀이 (r, g, b, 255)인지다.
bool AllPixels(Span<const uint8> pixels, uint8 r, uint8 g, uint8 b) noexcept {
    if (pixels.Size() != static_cast<usize>(Extent) * Extent * PixelBytes) {
        return false;
    }

    for (usize index = 0; index < pixels.Size(); index += PixelBytes) {
        if (pixels[index] != r || pixels[index + 1] != g || pixels[index + 2] != b ||
            pixels[index + 3] != FullChannel) {
            return false;
        }
    }

    return true;
}

bool AllBytes(Span<const uint8> bytes, uint8 value) noexcept {
    for (const uint8 byte : bytes) {
        if (byte != value) {
            return false;
        }
    }

    return !bytes.IsEmpty();
}

bool ReadTarget(Device& device, TextureHandle target, Array<uint8>& pixels) noexcept {
    return pixels.TryResize(static_cast<usize>(Extent) * Extent * PixelBytes).IsOk() &&
           device.ReadPixels(target, pixels.View()).IsOk();
}

// attachment 획득(ColorAttachment::acquire)으로 target을 clear 색으로 채운다.
bool ClearAcquired(Device& device, TextureHandle target, const ClearColor& clear) noexcept {
    PassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].texture = target;
    pass.colors[0].clear = clear;
    pass.colors[0].acquire = true;
    auto frame = device.BeginPass(pass);
    if (!frame) {
        PrintLastError(device);
        return false;
    }

    return device.EndFrame().IsOk();
}

// Targets heap: A·B는 offset 0을 나누고 C는 그 뒤다.
void RunTextureAliasing(Device& device, Allocator& allocator) noexcept {
    const TextureDesc desc = TargetDesc();
    auto placement = device.GetTexturePlacement(desc);
    Check(placement.IsOk() && placement.Value().kind == TransientHeapKind::Targets &&
              placement.Value().bytes != 0 && placement.Value().alignment != 0,
          "render target placement has a size, an alignment and the Targets kind");
    if (!placement) {
        PrintLastError(device);
        return;
    }

    const uint64 alignment = placement.Value().alignment;
    const uint64 separateOffset = AlignUp(placement.Value().bytes, alignment);
    const uint64 heapBytes = separateOffset + placement.Value().bytes;
    auto heap =
        device.CreateTransientHeap({.bytes = heapBytes, .kind = TransientHeapKind::Targets});
    Check(heap.IsOk(), "targets heap");
    if (!heap) {
        PrintLastError(device);
        return;
    }

    auto first = device.CreatePlacedTexture(heap.Value(), 0, desc);
    auto aliased = device.CreatePlacedTexture(heap.Value(), 0, desc);
    auto separate = device.CreatePlacedTexture(heap.Value(), separateOffset, desc);
    const bool created = first.IsOk() && aliased.IsOk() && separate.IsOk();
    Check(created, "placed render targets A and B at offset 0 and C after them");
    if (!created) {
        PrintLastError(device);
    }

    if (alignment > 1) {
        Check(device.CreatePlacedTexture(heap.Value(), alignment / 2, desc).Error() ==
                  ErrorCode::InvalidArgument,
              "misaligned placement offset is rejected");
    }

    Check(device.CreatePlacedTexture(heap.Value(), separateOffset + alignment, desc).Error() ==
              ErrorCode::InvalidArgument,
          "placement past the heap end is rejected");
    if (created) {
        Array<uint8> pixels(allocator);
        Check(ClearAcquired(device, first.Value(), {1.0f, 0.0f, 0.0f, 1.0f}) &&
                  ReadTarget(device, first.Value(), pixels) &&
                  AllPixels(pixels.View(), FullChannel, 0, 0),
              "A acquired as an attachment clears to red");
        // C는 명시 획득 전이로 받아 A를 복사한다.
        bool copied = false;
        auto frame = device.BeginPass(PassDesc{});
        if (frame) {
            ResourceTransition transitions[2];
            transitions[0].texture = first.Value();
            transitions[0].before = ResourceAccess::RenderTarget;
            transitions[0].after = ResourceAccess::CopySource;
            transitions[1].texture = separate.Value();
            transitions[1].before = ResourceAccess::Discard;
            transitions[1].after = ResourceAccess::CopyDestination;
            transitions[1].acquire = true;
            frame.Value()->Transition(Span<const ResourceTransition>(transitions, 2));
            copied = frame.Value()->CopyTexture(separate.Value(), first.Value()).IsOk();
            copied = device.EndFrame().IsOk() && copied;
        }

        Check(copied && ReadTarget(device, separate.Value(), pixels) &&
                  AllPixels(pixels.View(), FullChannel, 0, 0),
              "C acquired by an explicit transition receives the copy of A");
        Check(ClearAcquired(device, aliased.Value(), {0.0f, 0.0f, 1.0f, 1.0f}) &&
                  ReadTarget(device, aliased.Value(), pixels) &&
                  AllPixels(pixels.View(), 0, 0, FullChannel),
              "B acquired over A clears to blue");
        Check(ReadTarget(device, separate.Value(), pixels) &&
                  AllPixels(pixels.View(), FullChannel, 0, 0),
              "C outside the aliased range keeps its content");
    }

    // heap을 placed 자원보다 먼저 놓는다(마지막 자원이 파괴될 때 메모리를 놓는 경로).
    device.RetireTransientHeap(heap.Value());
    Check(device.CreatePlacedTexture(heap.Value(), 0, desc).IsOk() == false,
          "a retired heap takes no new placed resource");
    const Result<TextureHandle>* textures[] = {&first, &aliased, &separate};
    for (const Result<TextureHandle>* texture : textures) {
        if (texture->IsOk()) {
            device.Retire(texture->Value());
        }
    }
}

// Buffers heap: X·Y는 offset 0을 나눈다.
void RunBufferAliasing(Device& device) noexcept {
    const BufferDesc desc = PlacedBufferDesc();
    auto placement = device.GetBufferPlacement(desc);
    Check(placement.IsOk() && placement.Value().kind == TransientHeapKind::Buffers &&
              placement.Value().bytes >= BufferBytes,
          "buffer placement covers the buffer and has the Buffers kind");
    if (!placement) {
        PrintLastError(device);
        return;
    }

    auto heap = device.CreateTransientHeap(
        {.bytes = placement.Value().bytes, .kind = TransientHeapKind::Buffers});
    Check(heap.IsOk(), "buffers heap");
    if (!heap) {
        PrintLastError(device);
        return;
    }

    Check(device.CreatePlacedTexture(heap.Value(), 0, TargetDesc()).Error() ==
              ErrorCode::InvalidArgument,
          "a render target in a buffers heap is rejected");
    auto first = device.CreatePlacedBuffer(heap.Value(), 0, desc);
    auto second = device.CreatePlacedBuffer(heap.Value(), 0, desc);
    uint8 firstFill[BufferBytes];
    uint8 secondFill[BufferBytes];
    for (usize index = 0; index < BufferBytes; ++index) {
        firstFill[index] = FirstFill;
        secondFill[index] = SecondFill;
    }

    BufferDesc sourceDesc;
    sourceDesc.size = BufferBytes;
    sourceDesc.usage = BufferUsage::ShaderRead | BufferUsage::Raw;
    auto firstSource =
        device.CreateBuffer(sourceDesc, Span<const uint8>(firstFill, sizeof(firstFill)));
    auto secondSource =
        device.CreateBuffer(sourceDesc, Span<const uint8>(secondFill, sizeof(secondFill)));
    const bool created = first.IsOk() && second.IsOk() && firstSource.IsOk() && secondSource.IsOk();
    Check(created, "placed buffers X and Y at offset 0 and their copy sources");
    if (!created) {
        PrintLastError(device);
    }

    // 획득 전이 뒤 원본을 복사한다.
    const auto copy = [&device](BufferHandle destination, BufferHandle source) noexcept {
        auto frame = device.BeginPass(PassDesc{});
        if (!frame) {
            return false;
        }

        ResourceTransition acquire;
        acquire.buffer = destination;
        acquire.before = ResourceAccess::Discard;
        acquire.after = ResourceAccess::CopyDestination;
        acquire.acquire = true;
        frame.Value()->Transition(Span<const ResourceTransition>(&acquire, 1));
        const bool copied =
            frame.Value()->CopyBuffer(destination, 0, source, 0, BufferBytes).IsOk();
        return device.EndFrame().IsOk() && copied;
    };
    if (created) {
        uint8 read[BufferBytes] = {};
        const Span<uint8> out(read, sizeof(read));
        Check(copy(first.Value(), firstSource.Value()) &&
                  device.ReadBuffer(first.Value(), 0, out).IsOk() && AllBytes(out, FirstFill),
              "X acquired by a transition holds the first copy");
        Check(copy(second.Value(), secondSource.Value()) &&
                  device.ReadBuffer(second.Value(), 0, out).IsOk() && AllBytes(out, SecondFill),
              "Y acquired over X holds the second copy");
        // 같은 heap 구간이므로 X는 Y가 쓴 내용을 읽는다(선형 메모리, placed가 실제로 메모리를
        // 나누는지 확인한다).
        Check(device.ReadBuffer(first.Value(), 0, out).IsOk() && AllBytes(out, SecondFill),
              "X reads the bytes Y wrote to the shared heap range");
    }

    // placed 자원을 먼저 놓고 heap을 나중에 놓는다(heap의 마지막 사용 fence 뒤 해제 경로).
    const Result<BufferHandle>* buffers[] = {&first, &second, &firstSource, &secondSource};
    for (const Result<BufferHandle>* buffer : buffers) {
        if (buffer->IsOk()) {
            device.Retire(buffer->Value());
        }
    }

    device.RetireTransientHeap(heap.Value());
}

} // namespace

uint32 RunTransientHeapChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    if (!device.Caps().features.transientHeaps) {
        printf("  %s transient heaps: not supported, skipped\n", name);
        return 0;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    const usize retiresBefore = device.PendingRetireCount();
    Check(device.CreateTransientHeap({.bytes = 0, .kind = TransientHeapKind::Targets}).Error() ==
              ErrorCode::InvalidArgument,
          "an empty heap is rejected");
    TextureDesc sampled = TargetDesc();
    sampled.usage = TextureUsage::Sampled;
    Check(device.GetTexturePlacement(sampled).Error() == ErrorCode::InvalidArgument,
          "a Sampled texture has no placement");
    RunTextureAliasing(device, allocator);
    RunBufferAliasing(device);
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle after retiring placed resources and heaps");
    Check(device.PendingRetireCount() == retiresBefore,
          "placed resources and heaps are reclaimed after WaitIdle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s transient heaps: %u failure%s\n", name, failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
