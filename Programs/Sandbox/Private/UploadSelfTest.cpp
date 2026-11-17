#include "UploadSelfTest.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// offscreen 대상 크기다.
constexpr uint32 TargetSize = 32;
// ring wrap 시험의 텍스처 한 변과 갱신 횟수다(256×256×4 = 256 KiB, 20번이면 기본 ring 4 MiB를
// 넘는다).
constexpr uint32 WrapTextureSize = 256;
constexpr uint32 WrapUpdates = 20;
// 갱신 몇 번마다 프레임을 하나 그려 앞 묶음을 제출하는지다.
constexpr uint32 WrapUpdatesPerFrame = 4;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s upload: %s\n", backendName, what);
    }
}

// 로컬 Y(오른쪽)·Z(위)를 clip X(오른쪽)·Y(위)로 보내는 변환이다(+X를 바라보는 정면, UE 좌표).
// 단위 메시가 화면을 가득 채운다.
Mat4 FillClip() noexcept {
    Mat4 matrix = Mat4::Zero();
    matrix.m[4] = 2.0f;
    matrix.m[9] = 2.0f;
    matrix.m[14] = 0.5f;
    matrix.m[15] = 1.0f;
    return matrix;
}

struct Color {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
};

constexpr Color Red{255, 0, 0};
constexpr Color Green{0, 255, 0};
constexpr Color Blue{0, 0, 255};

bool Near(uint8 actual, uint8 expected) noexcept {
    const int difference = static_cast<int>(actual) - static_cast<int>(expected);
    return difference <= 2 && difference >= -2;
}

bool CenterIs(const Array<uint8>& pixels, Color expected) noexcept {
    const usize offset = (static_cast<usize>(TargetSize / 2) * TargetSize + TargetSize / 2) * 4;
    const bool same = Near(pixels[offset], expected.r) && Near(pixels[offset + 1], expected.g) &&
                      Near(pixels[offset + 2], expected.b);
    if (!same) {
        printf("    center (%u, %u, %u), expected (%u, %u, %u)\n", pixels[offset],
               pixels[offset + 1], pixels[offset + 2], expected.r, expected.g, expected.b);
    }

    return same;
}

// 한 색으로 채운 width×height RGBA 픽셀이다.
bool Fill(Array<uint8>& out, uint32 width, uint32 height, Color color) noexcept {
    if (!out.TryResize(static_cast<usize>(width) * height * 4)) {
        return false;
    }

    for (usize index = 0; index < out.Size(); index += 4) {
        out[index] = color.r;
        out[index + 1] = color.g;
        out[index + 2] = color.b;
        out[index + 3] = 255;
    }

    return true;
}

struct Bench {
    Device* device = nullptr;
    sample::Renderer* renderer = nullptr;
    TextureHandle target;
    Array<uint8> pixels;

    explicit Bench(Allocator& allocator) noexcept : pixels(allocator) {
    }

    Result<TextureHandle> Texture(uint32 width, uint32 height, Color color) noexcept {
        Array<uint8> data(pixels.GetAllocator());
        if (!Fill(data, width, height, color)) {
            return Result<TextureHandle>::Failure(ErrorCode::OutOfMemory);
        }

        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.sampler.filter = Filter::Nearest;
        return device->CreateTexture(desc, data.View());
    }

    // texture를 화면 가득 그리고 대상을 읽는다.
    bool Draw(TextureHandle texture) noexcept {
        auto frame = device->BeginOffscreen(target, {});
        if (!frame) {
            return false;
        }

        sample::RenderView view;
        view.viewProjection = FillClip();
        sample::RenderItem item;
        item.texture = texture;
        renderer->Draw(*frame.Value(), view, Span<const sample::RenderItem>(&item, 1));
        return device->EndFrame().IsOk() &&
               pixels.TryResize(static_cast<usize>(TargetSize) * TargetSize * 4).IsOk() &&
               device->ReadPixels(target, pixels.View()).IsOk();
    }
};

void RunTextureUpdate(Bench& bench) noexcept {
    auto texture = bench.Texture(2, 2, Red);
    Array<uint8> green(bench.pixels.GetAllocator());
    bool ok = texture && Fill(green, 2, 2, Green) &&
              bench.device->UpdateTexture(texture.Value(), 0, green.View()).IsOk();
    Check(ok && bench.Draw(texture.Value()) && CenterIs(bench.pixels, Green),
          "texture content update is visible to the next frame");
    if (texture) {
        Check(bench.device->UpdateTexture(texture.Value(), 1, green.View()).Error() ==
                      ErrorCode::InvalidArgument &&
                  bench.device->UpdateTexture(texture.Value(), 0, Span<const uint8>()).Error() ==
                      ErrorCode::InvalidArgument,
              "texture update rejects a missing mip and a wrong size");
        bench.device->Retire(texture.Value());
    }
}

void RunRingWrap(Bench& bench, bool ring) noexcept {
    const UploadStats before = bench.device->UploadStatistics();
    auto texture = bench.Texture(WrapTextureSize, WrapTextureSize, Red);
    Array<uint8> pixels(bench.pixels.GetAllocator());
    bool ok = texture.IsOk();
    for (uint32 index = 0; ok && index < WrapUpdates; ++index) {
        const bool last = index + 1 == WrapUpdates;
        ok =
            Fill(pixels, WrapTextureSize, WrapTextureSize, last || index % 2 == 1 ? Blue : Green) &&
            bench.device->UpdateTexture(texture.Value(), 0, pixels.View()).IsOk();
        if (ok && index % WrapUpdatesPerFrame == WrapUpdatesPerFrame - 1) {
            ok = bench.Draw(texture.Value());
        }
    }

    Check(ok && bench.Draw(texture.Value()) && CenterIs(bench.pixels, Blue),
          "many updates through the upload ring keep the last content");
    if (ring) {
        const UploadStats after = bench.device->UploadStatistics();
        Check(after.ringWraps > before.ringWraps && after.ringCapacity == DefaultUploadRingBytes,
              "upload ring wraps after its fences complete");
        printf("  %s upload ring: %llu wraps, %llu dedicated uploads\n", backendName,
               static_cast<unsigned long long>(after.ringWraps),
               static_cast<unsigned long long>(after.dedicatedUploads));
    }

    if (texture) {
        bench.device->Retire(texture.Value());
    }
}

void RunStaging(Bench& bench, bool deferredReclaim) noexcept {
    // 행 간격이 장치 정렬과 같은 64×64는 staging에서 바로 복사한다.
    constexpr uint32 DirectSize = 64;
    const uint64 bytes = uint64{DirectSize} * DirectSize * 4;
    auto texture = bench.Texture(DirectSize, DirectSize, Red);
    auto staging = bench.device->AcquireStaging(bytes);
    bool ok = texture && staging && staging.Value().size >= bytes;
    if (ok) {
        for (uint64 index = 0; index < bytes; index += 4) {
            staging.Value().data[index] = 0;
            staging.Value().data[index + 1] = 255;
            staging.Value().data[index + 2] = 0;
            staging.Value().data[index + 3] = 255;
        }

        ok = bench.device->UploadTextureFromStaging(texture.Value(), 0, staging.Value().handle, 0)
                 .IsOk();
        bench.device->ReleaseStaging(staging.Value().handle);
        // 업로드가 제출·완료되기 전에는 재사용 목록에 가지 않는다(D3D12·D3D11). OpenGL은 client
        // 메모리 업로드라 바로 돌아간다.
        const StagingStats waiting = bench.device->StagingStatistics();
        Check(deferredReclaim ? waiting.waiting == 1 : waiting.waiting == 0,
              "released staging waits for its upload fence");
        Check(bench.device->UploadTextureFromStaging(texture.Value(), 0, staging.Value().handle, 0)
                      .Error() == ErrorCode::StaleHandle,
              "released staging handle is stale");
    }

    Check(ok && bench.Draw(texture.Value()) && CenterIs(bench.pixels, Green),
          "texture uploaded straight from staging");
    Check(bench.device->WaitIdle().IsOk() && bench.device->StagingStatistics().waiting == 0 &&
              bench.device->StagingStatistics().free >= 1,
          "staging is reclaimed after the upload fence");
    const uint64 reusedBefore = bench.device->StagingStatistics().reused;
    auto again = bench.device->AcquireStaging(bytes);
    Check(again && bench.device->StagingStatistics().reused == reusedBefore + 1,
          "reclaimed staging is reused");

    // 정렬되지 않은 offset·좁은 행은 ring으로 다시 옮겨 올린다.
    auto small = bench.Texture(2, 2, Green);
    if (again && small) {
        uint8* data = again.Value().data;
        for (uint32 index = 0; index < 4; ++index) {
            platform::MemoryCopy(data + 4 + index * 4, "\xff\x00\x00\xff", 4);
        }

        Check(bench.device->UploadTextureFromStaging(small.Value(), 0, again.Value().handle, 4)
                      .IsOk() &&
                  bench.Draw(small.Value()) && CenterIs(bench.pixels, Red),
              "texture uploaded from an unaligned staging offset");
    }

    if (again) {
        bench.device->ReleaseStaging(again.Value().handle);
    }

    if (small) {
        bench.device->Retire(small.Value());
    }

    if (texture) {
        bench.device->Retire(texture.Value());
    }
}

void RunReadback(Bench& bench) noexcept {
    auto texture = bench.Texture(2, 2, Blue);
    bool ok = texture && bench.Draw(texture.Value());
    Array<uint8> expected(bench.pixels.GetAllocator());
    ok = ok && expected.TryAppend(bench.pixels.View()).IsOk();
    auto request = ok ? bench.device->RequestReadback(bench.target)
                      : Result<ReadbackRequest>::Failure(ErrorCode::InvalidArgument);
    Array<uint8> read(bench.pixels.GetAllocator());
    bool same = false;
    if (request) {
        // 완료를 다른 스레드 안전 대기로 기다린 뒤 비차단으로 읽는다.
        auto waited = bench.device->WaitFence(request.Value().completion);
        auto copied = read.TryResize(static_cast<usize>(request.Value().bytes)) && waited
                          ? bench.device->TryReadback(request.Value().handle, read.View())
                          : Result<bool>::Failure(ErrorCode::InvalidArgument);
        same = copied && copied.Value() && read.Size() == expected.Size() &&
               platform::MemoryCompare(read.Data(), expected.Data(), read.Size()) == 0;
        Check(bench.device->TryReadback(request.Value().handle, Span<uint8>()).Error() ==
                  ErrorCode::InvalidArgument,
              "readback rejects a wrong output size");
        bench.device->ReleaseReadback(request.Value().handle);
        Check(bench.device->TryReadback(request.Value().handle, read.View()).Error() ==
                  ErrorCode::StaleHandle,
              "released readback is stale");
    }

    Check(same, "asynchronous readback matches ReadPixels");
    auto frame = bench.device->BeginOffscreen(bench.target, {});
    if (frame) {
        Check(bench.device->RequestReadback(bench.target).Error() == ErrorCode::Busy,
              "readback is refused while recording");
        auto ended = bench.device->EndFrame();
        (void)ended;
    }

    if (texture) {
        bench.device->Retire(texture.Value());
    }
}

// 다른 스레드의 자원 생성 결과다.
struct CreatorThread {
    Device* device = nullptr;
    Result<TextureHandle> texture = Result<TextureHandle>::Failure(ErrorCode::Cancelled);

    static void Run(void* raw) noexcept {
        auto* self = static_cast<CreatorThread*>(raw);
        const uint8 green[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
        TextureDesc desc;
        desc.width = 2;
        desc.height = 2;
        self->texture = self->device->CreateTexture(desc, Span<const uint8>(green, sizeof(green)));
    }
};

void RunOtherThreadCreation(Bench& bench) noexcept {
    const bool concurrent = bench.device->Info().concurrentResourceCreation;
    CreatorThread creator;
    creator.device = bench.device;
    platform::Thread thread;
    const bool joined =
        thread.Start(&CreatorThread::Run, &creator) == platform::ThreadStatus::Success &&
        thread.Join() == platform::ThreadStatus::Success;
    if (concurrent) {
        Check(joined && creator.texture && bench.Draw(creator.texture.Value()) &&
                  CenterIs(bench.pixels, Green),
              "texture created on another thread is drawn by the owner");
    } else {
        Check(joined && creator.texture.Error() == ErrorCode::WrongThread,
              "creation on another thread is refused when not thread-safe");
    }

    if (creator.texture) {
        bench.device->Retire(creator.texture.Value());
    }
}

} // namespace

uint32 RunUploadChecks(const char* name, Device& device, sample::Renderer& renderer,
                       Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    // D3D12·D3D11은 업로드를 다음 제출이 실행하므로 ring·staging을 fence 뒤에 회수한다. OpenGL은
    // 갱신을 즉시 올린다.
    const bool deferredUploads = device.Info().backend != Backend::OpenGL;
    Bench bench(allocator);
    bench.device = &device;
    bench.renderer = &renderer;
    TextureDesc targetDesc;
    targetDesc.width = TargetSize;
    targetDesc.height = TargetSize;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    Check(target.IsOk(), "render target creation");
    if (!target) {
        return failures;
    }

    bench.target = target.Value();
    RunTextureUpdate(bench);
    RunRingWrap(bench, deferredUploads);
    RunStaging(bench, deferredUploads);
    RunReadback(bench);
    RunOtherThreadCreation(bench);
    device.Retire(bench.target);
    Check(device.WaitIdle().IsOk(), "upload checks finish idle");
    return failures;
}

} // namespace sandbox
