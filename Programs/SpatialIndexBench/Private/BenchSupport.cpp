#include "BenchSupport.h"

#include <Core/Algorithms.h>
#include <Platform/File.h>
#include <Platform/Memory.h>

#include <stdio.h>

namespace rocks::spatialbench {

namespace {

// 할당 머리말이다. 해제 때 요청 크기를 알기 위해 사용자 주소 앞에 둔다.
struct AllocationHeader {
    uint64 bytes;
    uint64 offset;
};

constexpr usize HeaderAlignment = 16;

} // namespace

Result<void*> BenchAllocator::Allocate(usize bytes, usize alignment, uint32) noexcept {
    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    const usize align = alignment < HeaderAlignment ? HeaderAlignment : alignment;
    // 머리말 자리는 정렬 단위 하나다(정렬 ≥ 16이고 머리말은 16 byte).
    const usize total = bytes + align;
    if (total < bytes) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }
    uint8* base = static_cast<uint8*>(platform::AlignedAllocate(total, align));
    if (base == nullptr) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }
    uint8* user = base + align;
    AllocationHeader header{bytes, align};
    platform::MemoryCopy(user - sizeof(AllocationHeader), &header, sizeof(header));
    ++allocations;
    liveBytes += bytes;
    peakBytes = liveBytes > peakBytes ? liveBytes : peakBytes;
    return Result<void*>::Success(user);
}

Result<void> BenchAllocator::Deallocate(void* address) noexcept {
    if (address == nullptr) {
        return Result<void>::Success();
    }
    uint8* user = static_cast<uint8*>(address);
    AllocationHeader header;
    platform::MemoryCopy(&header, user - sizeof(AllocationHeader), sizeof(header));
    ++deallocations;
    liveBytes -= header.bytes;
    platform::AlignedFree(user - header.offset);
    return Result<void>::Success();
}

void BenchAllocator::ResetCounters() noexcept {
    allocations = 0;
    deallocations = 0;
    peakBytes = liveBytes;
}

bool SampleSet::Add(uint64 elapsedNanoseconds, uint32 batch) noexcept {
    if (batch == 0) {
        return true;
    }
    batch_ = batch;
    operations_ += batch;
    total_ += static_cast<double>(elapsedNanoseconds);
    return static_cast<bool>(
        values_.TryPushBack(static_cast<double>(elapsedNanoseconds) / static_cast<double>(batch)));
}

void SampleSet::Clear() noexcept {
    values_.Clear();
    batch_ = 0;
    operations_ = 0;
    total_ = 0.0;
}

SampleSummary SampleSet::Summarize() noexcept {
    SampleSummary summary;
    const usize count = values_.Size();
    summary.samples = static_cast<uint32>(count);
    summary.batch = batch_;
    summary.operations = operations_;
    summary.totalNanoseconds = total_;
    if (count == 0) {
        return summary;
    }
    algo::Sort(values_.View());
    auto rank = [&](uint32 percent) noexcept {
        // 최근접 순위: ceil(p/100 · n) 번째(1부터).
        usize position = (count * percent + 99) / 100;
        position = position == 0 ? 1 : position;
        return values_[position - 1];
    };
    summary.median = rank(50);
    summary.p95 = rank(95);
    summary.p99 = rank(99);
    summary.minimum = values_[0];
    summary.maximum = values_[count - 1];
    double sum = 0.0;
    for (usize index = 0; index < count; ++index) {
        sum += values_[index];
    }
    summary.mean = sum / static_cast<double>(count);
    return summary;
}

void JsonWriter::AppendChar(char value) noexcept {
    if (!text_.TryPushBack(value)) {
        failed_ = true;
    }
}

void JsonWriter::Append(const char* text) noexcept {
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        AppendChar(*cursor);
    }
}

void JsonWriter::Separator() noexcept {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (depth_ == 0) {
        return;
    }
    if (!first_[depth_]) {
        AppendChar(',');
    }
    first_[depth_] = false;
}

void JsonWriter::BeginObject() noexcept {
    Separator();
    AppendChar('{');
    if (depth_ + 1 >= sizeof(first_) / sizeof(first_[0])) {
        failed_ = true;
        return;
    }
    ++depth_;
    first_[depth_] = true;
}

void JsonWriter::EndObject() noexcept {
    --depth_;
    AppendChar('}');
}

void JsonWriter::BeginArray() noexcept {
    Separator();
    AppendChar('[');
    if (depth_ + 1 >= sizeof(first_) / sizeof(first_[0])) {
        failed_ = true;
        return;
    }
    ++depth_;
    first_[depth_] = true;
}

void JsonWriter::EndArray() noexcept {
    --depth_;
    AppendChar(']');
}

void JsonWriter::Key(const char* key) noexcept {
    Separator();
    AppendChar('"');
    Append(key);
    Append("\":");
    afterKey_ = true;
}

void JsonWriter::String(const char* value) noexcept {
    Separator();
    AppendChar('"');
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') {
            AppendChar('\\');
        }
        AppendChar(*cursor);
    }
    AppendChar('"');
}

void JsonWriter::Number(double value) noexcept {
    Separator();
    // NaN·무한대는 JSON에 없으므로 null로 쓴다.
    if (!(value == value) || value > NumericLimits<double>::Max() ||
        value < -NumericLimits<double>::Max()) {
        Append("null");
        return;
    }
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "%.10g", value);
    Append(buffer);
}

void JsonWriter::Integer(uint64 value) noexcept {
    Separator();
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    Append(buffer);
}

void JsonWriter::SignedInteger(int64 value) noexcept {
    Separator();
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    Append(buffer);
}

void JsonWriter::Bool(bool value) noexcept {
    Separator();
    Append(value ? "true" : "false");
}

void JsonWriter::Null() noexcept {
    Separator();
    Append("null");
}

void JsonWriter::Summary(const char* key, const SampleSummary& summary) noexcept {
    Key(key);
    BeginObject();
    FieldInteger("samples", summary.samples);
    FieldInteger("batch", summary.batch);
    FieldInteger("operations", summary.operations);
    Field("totalNs", summary.totalNanoseconds);
    Field("medianNs", summary.median);
    Field("p95Ns", summary.p95);
    Field("p99Ns", summary.p99);
    Field("minNs", summary.minimum);
    Field("maxNs", summary.maximum);
    Field("meanNs", summary.mean);
    EndObject();
}

bool JsonWriter::WriteToFile(const char* path) noexcept {
    if (failed_) {
        return false;
    }
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::CreateAlways)) {
        return false;
    }
    if (!file.WriteAll(text_.Data(), text_.Size()).status) {
        return false;
    }
    return static_cast<bool>(file.Close());
}

} // namespace rocks::spatialbench
