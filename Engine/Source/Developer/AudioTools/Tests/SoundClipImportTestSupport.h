#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundClipImportOptions.generated.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/Test.h>

// SoundClipImporter 시험 지원이다(Docs/Design/AudioSystem 3.36.5절): AudioAssets 타입 등록, 가짜
// 임포트 문맥(출력·bulk·경고·실패를 모은다), WAV 원본 생성기.
namespace rocks::sound_import_test {

struct SoundEnv {
    SoundEnv() noexcept
        : types(allocator), registration(types, allocator),
          ready(RegisterAudioAssetsTypes(registration).IsOk()),
          objects(allocator, types, ContextId{0x534e4449ull, 1}) {
    }

    ~SoundEnv() noexcept {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    bool ready = false;
    ObjectRegistry objects;
};

struct SeenWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    uint64 detail = 0;
};

// 임포터 시험 문맥이다. 새 임포트는 Output이 새 SoundClip을, 재임포트는 existing을 준다.
class FakeSoundImportContext final : public AssetImportContext {
public:
    FakeSoundImportContext(SoundEnv& env, Span<const AssetImportSource> sources,
                           const void* options, ObjectHandle existing = {}) noexcept
        : bulk(env.allocator), warnings(env.allocator), env_(&env),
          access_(env.objects.OwnerAccess()), sources_(sources), options_(options),
          existing_(existing), roots_(env.allocator) {
    }

    Span<const AssetImportSource> Sources() const noexcept override {
        return sources_;
    }

    const void* Options() const noexcept override {
        return options_;
    }

    bool IsReimport() const noexcept override {
        return !existing_.IsNull();
    }

    ObjectRegistry& Registry() noexcept override {
        return env_->objects;
    }

    const ObjectAccessContext& Access() const noexcept override {
        return access_;
    }

    Result<ObjectHandle> Output(StringView key, TypeId type) noexcept override {
        if (!key.IsEmpty() || !output.IsNull()) {
            return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
        }

        if (!existing_.IsNull()) {
            output = existing_;
            return Result<ObjectHandle>::Success(output);
        }

        auto made = env_->objects.NewObject<SoundClip>(access_, type);
        if (!made) {
            return Result<ObjectHandle>::Failure(made.Error());
        }

        output = made.Value().Handle();
        if (auto pushed = roots_.TryPushBack(Move(made.Value())); !pushed) {
            return Result<ObjectHandle>::Failure(pushed.Error());
        }

        return Result<ObjectHandle>::Success(output);
    }

    Result<BulkDataRef> AddBulk(ObjectHandle target, Array<uint8>&& raw) noexcept override {
        if (target != output) {
            return Result<BulkDataRef>::Failure(ErrorCode::InvalidArgument);
        }

        BulkDataRef ref;
        ref.bulkId = 1;
        ref.rawBytes = raw.Size();
        ref.sha256 = Sha256::Hash(raw.View());
        bulk = Move(raw);
        return Result<BulkDataRef>::Success(ref);
    }

    Result<void> Warn(AssetDiagnosticCode code, uint64 detail) noexcept override {
        return warnings.TryPushBack(SeenWarning{code, detail});
    }

    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept override {
        failCode = code;
        failError = error;
        failDetail = detail;
        return Result<void>::Failure(error);
    }

    Result<void> AddTag(StringView, StringView) noexcept override {
        return Result<void>::Success();
    }

    Span<const ImportTag> PreviousTags() const noexcept override {
        return {};
    }

    Result<ObjectHandle> OpenAsset(AssetId) noexcept override {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    Result<ObjectHandle> EditAsset(AssetId) noexcept override {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    usize GroupSize() const noexcept override {
        return 1;
    }

    usize GroupIndex() const noexcept override {
        return 0;
    }

    AssetId MemberAsset(usize) const noexcept override {
        return {};
    }

    SoundClip* Imported() noexcept {
        auto borrowed = env_->objects.Resolve<SoundClip>(access_, output);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    ObjectHandle output{};
    Array<uint8> bulk;
    Array<SeenWarning> warnings;
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    ErrorCode failError = ErrorCode::None;
    uint64 failDetail = 0;

private:
    SoundEnv* env_;
    ObjectAccessContext access_;
    Span<const AssetImportSource> sources_;
    const void* options_;
    ObjectHandle existing_;
    Array<RootRef<SoundClip>> roots_;
};

// little-endian byte 생성기다.
struct Bytes {
    explicit Bytes(Allocator& allocator) noexcept : data(allocator) {
    }

    void U8(uint32 value) noexcept {
        CHECK(data.TryPushBack(static_cast<uint8>(value)));
    }

    void Le16(uint32 value) noexcept {
        U8(value);
        U8(value >> 8);
    }

    void Le24(uint32 value) noexcept {
        Le16(value);
        U8(value >> 16);
    }

    void Le32(uint32 value) noexcept {
        Le16(value);
        Le16(value >> 16);
    }

    void Tag(const char (&tag)[5]) noexcept {
        for (uint32 index = 0; index < 4; ++index) {
            U8(static_cast<uint8>(tag[index]));
        }
    }

    void Append(Span<const uint8> bytes) noexcept {
        for (const uint8 value : bytes) {
            U8(value);
        }
    }

    Span<const uint8> View() const noexcept {
        return Span<const uint8>(data.Data(), data.Size());
    }

    Array<uint8> data;
};

// WAV 하나다. samples는 교차(interleaved) 표본의 원시 byte다.
struct WavSpec {
    uint32 formatTag = 1;
    uint32 channels = 1;
    uint32 sampleRate = 48000;
    uint32 bitsPerSample = 16;
    // smpl loop([start, end] 포함 — 파일 규칙)와 cue 하나, 모르는 chunk를 더할지다.
    bool loop = false;
    uint32 loopStart = 0;
    uint32 loopEndInclusive = 0;
    bool cue = false;
    uint32 cueId = 7;
    uint32 cueFrame = 0;
    bool unknownChunk = false;
};

inline void BuildWav(Bytes& out, const WavSpec& spec, Span<const uint8> samples) noexcept {
    Bytes body(out.data.GetAllocator());
    body.Tag("WAVE");
    body.Tag("fmt ");
    body.Le32(16);
    body.Le16(spec.formatTag);
    body.Le16(spec.channels);
    body.Le32(spec.sampleRate);
    const uint32 block = spec.channels * spec.bitsPerSample / 8;
    body.Le32(spec.sampleRate * block);
    body.Le16(block);
    body.Le16(spec.bitsPerSample);
    if (spec.unknownChunk) {
        body.Tag("abcd");
        body.Le32(4);
        body.Le32(0);
    }

    if (spec.loop) {
        body.Tag("smpl");
        body.Le32(36 + 24);
        for (uint32 index = 0; index < 7; ++index) {
            body.Le32(0);
        }

        body.Le32(1);
        body.Le32(0);
        body.Le32(0);
        body.Le32(0);
        body.Le32(spec.loopStart);
        body.Le32(spec.loopEndInclusive);
        body.Le32(0);
        body.Le32(0);
    }

    if (spec.cue) {
        body.Tag("cue ");
        body.Le32(4 + 24);
        body.Le32(1);
        body.Le32(spec.cueId);
        body.Le32(spec.cueFrame);
        body.Tag("data");
        body.Le32(0);
        body.Le32(0);
        body.Le32(spec.cueFrame);
    }

    body.Tag("data");
    body.Le32(static_cast<uint32>(samples.Size()));
    body.Append(samples);
    if ((samples.Size() & 1) != 0) {
        body.U8(0);
    }

    out.Tag("RIFF");
    out.Le32(static_cast<uint32>(body.data.Size()));
    out.Append(body.View());
}

} // namespace rocks::sound_import_test
