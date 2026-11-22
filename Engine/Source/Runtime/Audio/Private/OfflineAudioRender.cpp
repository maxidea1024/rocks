#include <Audio/OfflineAudioRender.h>

#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Platform/FloatingPointEnvironment.h>

namespace rocks::audio {

OfflineAudioRender::OfflineAudioRender(AudioMixer&& mixer, Allocator& allocator) noexcept
    : mixer_(Move(mixer)), pending_(allocator) {
}

Result<OfflineAudioRender> OfflineAudioRender::TryCreate(const AudioMixerConfig& config,
                                                         Allocator& allocator) noexcept {
    auto mixer = AudioMixer::TryCreate(config, allocator);
    if (!mixer) {
        return Result<OfflineAudioRender>::Failure(mixer.Error());
    }

    return Result<OfflineAudioRender>::Success(OfflineAudioRender(Move(mixer.Value()), allocator));
}

Result<void> OfflineAudioRender::Submit(const AudioRtCommand& command) noexcept {
    // 순번은 제어 API(PreparePlay·Make*)가 매긴 값을 그대로 쓴다(3.23.1절).
    return pending_.TryPushBack(command);
}

Result<void> OfflineAudioRender::Render(Span<float> interleaved, uint32 blockCount) noexcept {
    const usize blockFloats =
        static_cast<usize>(mixer_.Config().blockFrames) * mixer_.OutputChannelCount();
    if (interleaved.Size() != blockFloats * blockCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    renderTarget_ = interleaved;
    renderBlocks_ = blockCount;
    platform::RunWithRealtimeAudioFloatingPoint(&OfflineAudioRender::RenderBlocks, this);
    // 호출자 버퍼를 가리키는 Span을 남기지 않는다.
    renderTarget_ = Span<float>();
    renderBlocks_ = 0;
    return Result<void>::Success();
}

void OfflineAudioRender::RenderBlocks(void* context) noexcept {
    auto& self = *static_cast<OfflineAudioRender*>(context);
    AudioMixer& mixer = self.mixer_;
    const usize blockFloats =
        static_cast<usize>(mixer.Config().blockFrames) * mixer.OutputChannelCount();
    for (uint32 block = 0; block < self.renderBlocks_; ++block) {
        // Immediate는 이 블록 시작으로 바꾸고 (mixFrame, sequence)로 정렬한다(3.22.1절 — 미래 명령
        // 뒤에 갇히지 않게).
        const uint64 blockStart = mixer.MixFrame();
        for (AudioRtCommand& command : self.pending_) {
            if (command.mixFrame == ImmediateMixFrame) {
                command.mixFrame = blockStart;
            }
        }

        algo::Sort(Span<AudioRtCommand>(self.pending_.Data(), self.pending_.Size()),
                   [](const AudioRtCommand& left, const AudioRtCommand& right) noexcept {
                       return left.mixFrame < right.mixFrame ||
                              (left.mixFrame == right.mixFrame && left.sequence < right.sequence);
                   });
        const RenderBlockResult result = mixer.RenderBlock(
            Span<const AudioRtCommand>(self.pending_.Data(), self.pending_.Size()),
            self.renderTarget_.Slice(blockFloats * block, blockFloats));
        if (result.consumedCommands > 0) {
            // 앞부분 제거는 실패하지 않는다(범위 안).
            const bool removed =
                static_cast<bool>(self.pending_.TryRemoveAt(0, result.consumedCommands));
            (void)removed;
        }
    }
}

} // namespace rocks::audio
