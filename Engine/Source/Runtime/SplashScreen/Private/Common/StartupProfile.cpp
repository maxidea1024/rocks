#include "StartupProfile.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/FileError.h>
#include <Core/FileRead.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Platform/PlatformMath.h>

namespace rocks::splash::detail {

namespace {

constexpr StringView ProfileVersionKey = "v";
constexpr StringView ProfileSamplesKey = "samples";
constexpr StringView ProfileTemporarySuffix = ".tmp";

bool ProfileStepIndex(StringView key, uint32& index) noexcept {
    for (uint32 step = 0; step < StartupStepCount; ++step) {
        if (key == StringView(StartupStepName(static_cast<StartupStep>(step)))) {
            index = step;
            return true;
        }
    }

    return false;
}

// 대상 항목 하나를 읽는다. 단계 값은 0 이상 MaxStartupStepSeconds 이하여야 하고 samples는 부호 없는
// 정수다. 모르는 키는 넘기고 다시 쓸 때 버린다(같은 형식 버전 안에서 단계가 늘면 그 단계는 다음
// 성공부터 쌓인다).
bool ProfileReadEntry(JsonValue object, StartupProfileEntry& out) noexcept {
    if (!object.IsObject()) {
        return false;
    }

    for (usize member = 0; member < object.Size(); ++member) {
        const StringView key = object.KeyAt(member);
        const JsonValue value = object.ValueAt(member);
        if (key == ProfileSamplesKey) {
            if (!value.IsNumber()) {
                return false;
            }

            auto samples = value.TryGetUInt64();
            if (!samples) {
                return false;
            }

            out.samples = samples.Value();
            continue;
        }

        uint32 index = 0;
        if (!ProfileStepIndex(key, index)) {
            continue;
        }

        if (!value.IsNumber()) {
            return false;
        }

        auto seconds = value.TryGetDouble();
        if (!seconds || !platform::PlatformMath::IsFinite(seconds.Value()) ||
            seconds.Value() < 0.0 || seconds.Value() > MaxStartupStepSeconds) {
            return false;
        }

        out.seconds[index] = seconds.Value();
        out.has[index] = true;
    }

    return true;
}

// 경로의 부모 폴더 길이다(구분자 앞까지). 구분자가 없으면 0.
usize ProfileParentBytes(StringView path) noexcept {
    for (usize index = path.ByteCount(); index != 0; --index) {
        const char c = path.Data()[index - 1];
        if (c == '/' || c == '\\') {
            return index - 1;
        }
    }

    return 0;
}

// content를 path에 원자적으로 쓴다(임시 파일 → Flush → Replace).
Result<void> ProfileWriteAtomically(platform::FileSystem& files, StringView path,
                                    StringView content, Allocator& allocator) noexcept {
    // 임시 이름에 프로세스 번호를 넣는다(두 실행이 겹쳐도 남의 반쯤 쓴 임시 파일을 게시하지 않게 —
    // 겹치면 마지막 쓰기만 남는 한계는 8.2절).
    StringBuilder<256> temporary(allocator);
    (void)temporary.TryAppendFormat("{}.{}{}", path, platform::CurrentProcessId(),
                                    ProfileTemporarySuffix);
    if (auto built = temporary.Status(); !built) {
        return built;
    }

    platform::File file;
    platform::FileStatus status =
        file.Open(files, temporary.View().Data(), temporary.View().ByteCount(),
                  platform::FileOpenMode::CreateAlways);
    if (!status) {
        return Result<void>::Failure(FileErrorCode(status));
    }

    status = file.WriteAll(content.Data(), content.ByteCount()).status;
    if (status) {
        status = file.Flush();
    }

    const platform::FileStatus closed = file.Close();
    if (status && !closed) {
        status = closed;
    }

    if (status) {
        status = files.Replace(temporary.View().Data(), temporary.View().ByteCount(), path.Data(),
                               path.ByteCount());
    }

    if (!status) {
        (void)files.Delete(temporary.View().Data(), temporary.View().ByteCount());
        return Result<void>::Failure(FileErrorCode(status));
    }

    return Result<void>::Success();
}

} // namespace

StartupProfileLoad LoadStartupProfile(Allocator& allocator, StringView path, StringView target,
                                      StartupProfileEntry& out, String& text) noexcept {
    out = {};
    text.Clear();
    Array<uint8> bytes(allocator);
    bool existed = false;
    if (auto read = ReadWholeFile(platform::PhysicalFileSystem(), path, MaxStartupProfileBytes,
                                  bytes, existed);
        !read) {
        // 너무 큰 파일은 깨진 것으로 보고 덮어쓴다. 그 밖의 IO 오류는 이번 실행에서 쓰지 않는다.
        return read.Error() == ErrorCode::CapacityExceeded ? StartupProfileLoad::Invalid
                                                           : StartupProfileLoad::ReadFailed;
    }

    if (!existed) {
        return StartupProfileLoad::Missing;
    }

    const StringView content(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    JsonDocument document(allocator);
    if (!document.Parse(content).Succeeded()) {
        return StartupProfileLoad::Invalid;
    }

    const JsonValue root = document.Root();
    if (!root.IsObject()) {
        return StartupProfileLoad::Invalid;
    }

    const JsonValue versionValue = root.Find(ProfileVersionKey);
    if (!versionValue.IsValid() || !versionValue.IsNumber()) {
        return StartupProfileLoad::Invalid;
    }

    auto version = versionValue.TryGetUInt64();
    if (!version || version.Value() != StartupProfileVersion) {
        return StartupProfileLoad::Invalid;
    }

    // 파일이 유효하므로 다른 대상 항목을 보존하려고 원문을 먼저 남긴다. 남기지 못하면 덮어쓸 때
    // 다른 대상이 사라지므로 이번 실행은 쓰지 않는다.
    if (!text.TryAppend(content)) {
        text.Clear();
        return StartupProfileLoad::ReadFailed;
    }

    const JsonValue entry = root.Find(target);
    if (entry.IsValid() && !ProfileReadEntry(entry, out)) {
        out = {};
        return StartupProfileLoad::Invalid;
    }

    return StartupProfileLoad::Loaded;
}

void ComputeStartupWeights(const bool (&included)[StartupStepCount],
                           const StartupProfileEntry& saved,
                           double (&weights)[StartupStepCount]) noexcept {
    // 저장값이 있는 단계의 초 합과 그 단계들의 코드 기본값 합(없는 단계를 채울 비율)이다.
    double savedSeconds = 0.0;
    double savedDefaults = 0.0;
    for (uint32 step = 0; step < StartupStepCount; ++step) {
        if (included[step] && saved.has[step]) {
            savedSeconds += saved.seconds[step];
            savedDefaults += DefaultStartupStepWeights[step];
        }
    }

    const bool calibrated = savedSeconds > 0.0 && savedDefaults > 0.0;
    const double scale = calibrated ? savedSeconds / savedDefaults : 1.0;
    double total = 0.0;
    for (uint32 step = 0; step < StartupStepCount; ++step) {
        if (!included[step]) {
            weights[step] = 0.0;
        } else if (calibrated && saved.has[step]) {
            weights[step] = saved.seconds[step];
        } else {
            weights[step] = DefaultStartupStepWeights[step] * scale;
        }

        total += weights[step];
    }

    // 바닥: 각 단계를 합의 StartupStepWeightFloor 이상으로(올린 뒤 합으로 한 번 더).
    for (uint32 pass = 0; pass < 2; ++pass) {
        const double floor = total * StartupStepWeightFloor;
        total = 0.0;
        for (uint32 step = 0; step < StartupStepCount; ++step) {
            if (included[step] && weights[step] < floor) {
                weights[step] = floor;
            }

            total += weights[step];
        }
    }
}

StartupProfileEntry BlendStartupProfile(const StartupProfileEntry& saved,
                                        const StartupStepTimes& measured,
                                        const bool (&entered)[StartupStepCount]) noexcept {
    StartupProfileEntry blended;
    // 수는 상한에서 멈춘다(0으로 돌지 않게).
    blended.samples =
        saved.samples == NumericLimits<uint64>::Max() ? saved.samples : saved.samples + 1;
    for (uint32 step = 0; step < StartupStepCount; ++step) {
        if (!entered[step]) {
            continue;
        }

        const double now = measured.seconds[step] < MaxStartupStepSeconds ? measured.seconds[step]
                                                                          : MaxStartupStepSeconds;
        blended.seconds[step] =
            saved.has[step]
                ? (1.0 - StartupProfileBlend) * saved.seconds[step] + StartupProfileBlend * now
                : now;
        blended.has[step] = true;
    }

    return blended;
}

Result<void> WriteStartupProfile(Allocator& allocator, StringView path, StringView target,
                                 const StartupProfileEntry& entry,
                                 StringView previousText) noexcept {
    StringBuilder<1024> out(allocator);
    JsonWriter writer(out, JsonWriterOptions{});
    // 처음 실패한 쓰기의 오류다(뒤 호출은 쓰지 않는다).
    ErrorCode failed = ErrorCode::None;
    const auto record = [&failed](Result<void> result) noexcept {
        if (failed == ErrorCode::None && !result) {
            failed = result.Error();
        }
    };
    record(writer.BeginObject());
    record(writer.Key(ProfileVersionKey));
    record(writer.UInt64(StartupProfileVersion));
    // 다른 대상 항목은 그대로 옮긴다.
    JsonDocument previous(allocator);
    if (!previousText.IsEmpty() && previous.Parse(previousText).Succeeded() &&
        previous.Root().IsObject()) {
        const JsonValue root = previous.Root();
        for (usize member = 0; failed == ErrorCode::None && member < root.Size(); ++member) {
            const StringView key = root.KeyAt(member);
            if (key == ProfileVersionKey || key == target) {
                continue;
            }

            record(writer.Key(key));
            record(writer.Value(root.ValueAt(member)));
        }
    }

    record(writer.Key(target));
    record(writer.BeginObject());
    for (uint32 step = 0; failed == ErrorCode::None && step < StartupStepCount; ++step) {
        if (!entry.has[step]) {
            continue;
        }

        record(writer.Key(StringView(StartupStepName(static_cast<StartupStep>(step)))));
        record(writer.Double(entry.seconds[step]));
    }

    record(writer.Key(ProfileSamplesKey));
    record(writer.UInt64(entry.samples));
    record(writer.EndObject());
    record(writer.EndObject());
    record(writer.Finish());
    if (failed != ErrorCode::None) {
        return Result<void>::Failure(failed);
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    const usize parent = ProfileParentBytes(path);
    if (parent != 0) {
        String directory(allocator);
        if (auto copied = directory.TryAppend(StringView(path.Data(), parent)); !copied) {
            return copied;
        }

        if (const platform::FileStatus made = platform::MakeDirectoryTree(
                files, directory.View().Data(), directory.View().ByteCount());
            !made) {
            return Result<void>::Failure(FileErrorCode(made));
        }
    }

    return ProfileWriteAtomically(files, path, out.View(), allocator);
}

} // namespace rocks::splash::detail
