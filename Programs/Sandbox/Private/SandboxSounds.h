#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <EngineAudio/SoundSourceRef.h>

// Sandbox 오디오 확인용 소리다(Docs/Design/AudioSystem 3.35.11절 — AU3a3). 에셋 없이 메모리 PCM으로
// 만든다(쿠킹·임포트는 AU4·AU5).
namespace sandbox {

struct SandboxSounds {
    // 짧은 UI 클릭(880 Hz, 80 ms, 지수 감쇠 envelope) — 2D.
    rocks::SoundSourceRef click;
    // 방향을 듣기 쉬운 mono loop(220 Hz 톱니를 1 kHz one-pole LPF로 거름, 1초 — 220 주기라 이음매
    // 없음) — 감쇠·패닝 스냅숏 포함.
    rocks::SoundSourceRef loop;
};

// 두 소리를 만든다. 오류: 할당 실패.
rocks::Result<SandboxSounds> MakeSandboxSounds(rocks::Allocator& allocator) noexcept;

} // namespace sandbox
