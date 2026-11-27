#include "ShaderReloadSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <RenderBase/ShaderSet.h>
#include <stdio.h>
#include <string.h>

// 감시 재컴파일 self-test다(Docs/Design/ShaderSystem 16.3절 SSD78). 규칙은 헤더 주석을 따른다.
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 대상 크기·byte 수다.
constexpr uint32 TargetSize = 4;
constexpr uint32 BytesPerPixel = 4;
constexpr usize TargetBytes = static_cast<usize>(TargetSize) * TargetSize * BytesPerPixel;
// 통지 대기의 실패 판정 상한(ms)이다. 시간 초과는 통지가 오지 않은 실패이고, 성능 판정은 따로
// 잰 값으로 한다(시간에 기대지 않는다 — 통지가 gate다).
constexpr uint32 ReloadNoticeBoundMilliseconds = 180'000;
// UNORM8 readback 허용 오차다.
constexpr int ColorTolerance = 1;
// 문법 오류를 넣는 ReloadColor.rsh의 줄이다(BrokenSource 둘째 줄).
constexpr uint32 BrokenLine = 2;
// 원문 버퍼 byte 수다.
constexpr usize SourceBytes = 256;
// 경로 버퍼 byte 수다.
constexpr usize PathBytes = 1024;
// 나노초 → 밀리초다.
constexpr uint64 NanosecondsPerMillisecond = 1'000'000;
// 시험 원문의 가상 경로다.
constexpr char ReloadColorVirtualPath[] = "/Plugin/SandboxReload/ReloadColor.rsh";

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s shader reload: %s\n", backendName, what);
    }
}

struct Color {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
};

// ReloadColor.rsh를 text로 바꾼다: 임시 파일에 다 쓴 뒤 Replace(감시가 반쯤 쓴 원문을 보지 않는다).
bool WriteReloadColor(StringView text) noexcept {
    const StringView directory = shaders::ReloadSourceDirectory();
    char path[PathBytes];
    char temporary[PathBytes];
    snprintf(path, sizeof(path), "%.*s/ReloadColor.rsh", static_cast<int>(directory.ByteCount()),
             directory.Data());
    snprintf(temporary, sizeof(temporary), "%s.%u.tmp", path,
             static_cast<unsigned>(platform::CurrentProcessId()));
    platform::File file;
    if (!file.Open(temporary, platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    const platform::FileTransfer written = file.WriteAll(text.Data(), text.ByteCount());
    const bool flushed = written.status && file.Flush();
    if (!file.Close() || !flushed) {
        return false;
    }

    return static_cast<bool>(
        platform::PhysicalFileSystem().Replace(temporary, strlen(temporary), path, strlen(path)));
}

bool WriteColor(Color color) noexcept {
    char text[SourceBytes];
    snprintf(text, sizeof(text),
             "#pragma once\n"
             "static const float4 ReloadColor = float4(%u.0 / 255.0, %u.0 / 255.0, %u.0 / 255.0, "
             "1.0);\n",
             color.r, color.g, color.b);
    return WriteReloadColor(StringView(text));
}

// 둘째 줄(BrokenLine)에 모르는 이름이 있다.
bool WriteBroken() noexcept {
    return WriteReloadColor("#pragma once\nstatic const float4 ReloadColor = reloadColr;\n");
}

// 전체화면 삼각형을 활성 집합의 FullScreenVS·ReloadColorPS로 그려 가운데 픽셀을 읽는다.
bool Draw(Device& device, TextureHandle target, Array<uint8>& pixels, Color& out) noexcept {
    PipelineDesc desc;
    desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    auto pipeline = shaders::CreateGraphicsPipeline(device, shaders::FullScreenVS,
                                                    shaders::ReloadColorPS, desc);
    if (!pipeline) {
        return false;
    }

    bool drawn = false;
    if (auto frame = device.BeginOffscreen(target, {})) {
        frame.Value()->SetPipeline(pipeline.Value());
        frame.Value()->Draw(3);
        drawn = device.EndFrame().IsOk() && device.ReadPixels(target, pixels.View()).IsOk();
    }

    device.Retire(pipeline.Value());
    if (drawn) {
        const usize center =
            (static_cast<usize>(TargetSize / 2) * TargetSize + TargetSize / 2) * BytesPerPixel;
        out = Color{pixels[center], pixels[center + 1], pixels[center + 2]};
    }

    return drawn;
}

bool Near(Color left, Color right) noexcept {
    const auto close = [](uint8 a, uint8 b) noexcept {
        const int difference = static_cast<int>(a) - static_cast<int>(b);
        return difference <= ColorTolerance && difference >= -ColorTolerance;
    };
    return close(left.r, right.r) && close(left.g, right.g) && close(left.b, right.b);
}

// 통지 gate: 순번 after 뒤 통지 가운데 accept를 만족하는 것까지 기다린다(같은 저장에 감시 이벤트가
// 여럿 와도 — 재컴파일이 여러 번 돌아도 — 판정은 결과로 한다). 상한 안에 오지 않으면 false다.
template <class Accept>
bool WaitFor(render::ShaderProvider& provider, uint64& after, Accept&& accept,
             render::ShaderReloadNotice& notice) noexcept {
    while (provider.WaitForReload(after, ReloadNoticeBoundMilliseconds, notice)) {
        after = notice.sequence;
        if (accept(notice)) {
            return true;
        }
    }

    return false;
}

uint64 LatestSequence(render::ShaderProvider& provider) noexcept {
    render::ShaderReloadNotice notice;
    return provider.WaitForReload(0, 0, notice) ? notice.sequence : 0;
}

uint64 ElapsedMilliseconds(uint64 started) noexcept {
    return (platform::MonotonicNanoseconds() - started) / NanosecondsPerMillisecond;
}

} // namespace

uint32 RunShaderReloadChecks(const char* name, Device& device, Allocator& allocator,
                             uint32 index) noexcept {
    failures = 0;
    backendName = name;
    render::ShaderProvider* provider = shaders::Host().Provider();
    if (provider == nullptr || shaders::ActiveSet() == nullptr ||
        shaders::ReloadSourceDirectory().IsEmpty() || !shaders::Has(shaders::ReloadColorPS)) {
        printf("  [NOT RUN] %s shader reload: no development shader provider or test root\n", name);
        return 0;
    }

    const StringView format = shaders::ActiveFormat();
    TextureDesc targetDesc;
    targetDesc.width = TargetSize;
    targetDesc.height = TargetSize;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    Array<uint8> pixels(allocator);
    Check(target.IsOk() && pixels.TryResize(TargetBytes).IsOk(), "target creation");
    if (!target || pixels.Size() != TargetBytes) {
        return failures;
    }

    // 장치마다 다른 두 색이다(앞 장치의 상태와 무관하게 매번 실제 재컴파일이 일어난다).
    const uint8 step = static_cast<uint8>(16 * index);
    const Color colors[] = {{static_cast<uint8>(32 + step), 192, 64},
                            {200, static_cast<uint8>(40 + step), 120}};
    uint64 sequence = LatestSequence(*provider);
    for (const Color color : colors) {
        const uint64 before = provider->LibraryRevision(format, shader::ShaderLibraryKind::Engine);
        const uint64 started = platform::MonotonicNanoseconds();
        Check(WriteColor(color), "the include file is rewritten");
        render::ShaderReloadNotice notice;
        const bool reloaded = WaitFor(
            *provider, sequence,
            [&](const render::ShaderReloadNotice& latest) noexcept {
                return latest.succeeded && provider->LibraryRevision(
                                               format, shader::ShaderLibraryKind::Engine) > before;
            },
            notice);
        Check(reloaded, "a successful recompile notice raises the format revision");
        const bool swapped = shaders::ApplyShaderUpdates();
        const uint64 elapsed = ElapsedMilliseconds(started);
        Check(swapped, "the frame boundary swaps to the recompiled shader set");
        Check(shaders::ActiveRevision() ==
                  provider->LibraryRevision(format, shader::ShaderLibraryKind::Engine),
              "the active shader set has the provider revision");
        Color drawn;
        Check(Draw(device, target.Value(), pixels, drawn) && Near(drawn, color),
              "the fullscreen pixel shows the saved color");
        printf("    %s shader reload: save -> swap %llu ms (recompile %llu ms, %u format "
               "librar(ies) changed), pixel (%u, %u, %u)\n",
               name, static_cast<unsigned long long>(elapsed),
               static_cast<unsigned long long>(notice.elapsedMilliseconds), notice.changedFormats,
               drawn.r, drawn.g, drawn.b);
    }

    // 문법 오류: 실패 통지와 진단 위치, 교체 없음, 픽셀 그대로.
    const Color kept = colors[1];
    const uint64 revision = provider->LibraryRevision(format, shader::ShaderLibraryKind::Engine);
    Check(WriteBroken(), "the broken include file is written");
    render::ShaderReloadNotice failed;
    Check(WaitFor(
              *provider, sequence,
              [](const render::ShaderReloadNotice& latest) noexcept { return !latest.succeeded; },
              failed),
          "a failed recompile notice arrives");
    Check(failed.failedJobs > 0 && StringView(failed.path) == StringView(ReloadColorVirtualPath) &&
              failed.line == BrokenLine && StringView(failed.message).Contains("reloadColr"),
          "the failure notice carries the error location in the include file");
    if (!failed.succeeded) {
        printf("    %s shader reload: failure reported at %s(%u,%u): %s\n", name, failed.path,
               failed.line, failed.column, failed.message);
    }

    Check(!shaders::ApplyShaderUpdates(), "a failed recompile does not swap");
    Check(provider->LibraryRevision(format, shader::ShaderLibraryKind::Engine) == revision,
          "a failed recompile keeps the previous library revision");
    Color drawn;
    Check(Draw(device, target.Value(), pixels, drawn) && Near(drawn, kept),
          "the previous shader keeps drawing after a failed recompile");

    // 고치면(이전과 같은 원문) 성공 통지이고 byte가 같아 revision·집합이 그대로다.
    Check(WriteColor(kept), "the include file is restored");
    render::ShaderReloadNotice restored;
    Check(WaitFor(
              *provider, sequence,
              [](const render::ShaderReloadNotice& latest) noexcept { return latest.succeeded; },
              restored),
          "the restored source recompiles");
    Check(provider->LibraryRevision(format, shader::ShaderLibraryKind::Engine) == revision &&
              !shaders::ApplyShaderUpdates(),
          "identical libraries keep the revision and the shader set");
    Check(Draw(device, target.Value(), pixels, drawn) && Near(drawn, kept),
          "the restored shader draws the same color");
    device.Retire(target.Value());
    Check(device.ValidationErrorCount() == 0, "no validation errors");
    return failures;
}

} // namespace sandbox
