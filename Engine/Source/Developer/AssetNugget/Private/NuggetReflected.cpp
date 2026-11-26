#include "NuggetInternal.h"

#include <AssetNugget/NuggetReflected.h>
#include <Core/Algorithms.h>
#include <Core/Text/NamePool.h>
#include <Object/ObjectRegistry.h>
#include <ReflectionText/TextVisit.h>

// 리플렉션 값의 너겟 키 기록이다(Docs/Design/AssetSystem 9.1절 결정 2 — NuggetReflected.h).
namespace rocks {

namespace {

using reflection_text::AssignmentTarget;
using reflection_text::TextCodecOptions;
using reflection_text::TextUse;
using reflection_text::ValueVisitor;

// 정렬할 항목 하나다(Set 원소는 key만, Map 항목은 key와 value).
struct SortEntry {
    explicit SortEntry(Allocator& allocator) noexcept : key(allocator), value(allocator) {
    }

    Array<uint8> key;
    Array<uint8> value;
};

// 진행 중인 Map·Set 하나다(그 컨테이너의 항목 인코딩만 모은다 — 9.1절 결정 2).
struct SortFrame {
    explicit SortFrame(Allocator& allocator) noexcept : entries(allocator) {
    }

    bool map = false;
    Array<SortEntry> entries;
};

class KeyVisitor final : public ValueVisitor {
public:
    KeyVisitor(NuggetKeyWriter& root, NamePool* names, Allocator& allocator) noexcept
        : root_(root), names_(names), allocator_(allocator), frames_(allocator) {
    }

    Result<void> BeginStruct() noexcept override {
        Out out(*this, Current());
        out.writer.WriteStructBegin();
        return out.writer.Error();
    }

    Result<void> EndStruct() noexcept override {
        Out out(*this, Current());
        out.writer.WriteStructEnd();
        return out.writer.Error();
    }

    // 키는 이름과 값만 쓴다(필드 메타·유효 플래그는 키 입력이 아니다).
    Result<void> Field(StringView name, const FieldSchema& field, uint32 flags) noexcept override {
        (void)field;
        (void)flags;
        Out out(*this, Current());
        out.writer.WriteField(name);
        return out.writer.Error();
    }

    Result<void> Value(const FieldValue& value) noexcept override {
        // Set 원소는 원소마다 항목 하나다(자리는 BeginSet이 원소 수만큼 잡았다).
        if (!frames_.IsEmpty() && !frames_[frames_.Size() - 1].map) {
            if (auto started = StartEntry(); !started) {
                return started;
            }

            return WriteValue(value, CurrentKey());
        }

        return WriteValue(value, Current());
    }

    Result<void> Absent() noexcept override {
        Out out(*this, Current());
        out.writer.WriteNone();
        return out.writer.Error();
    }

    Result<void> Bytes(Span<const uint8> bytes) noexcept override {
        Out out(*this, Current());
        out.writer.WriteBytes(bytes);
        return out.writer.Error();
    }

    Result<void> BeginArray(usize count) noexcept override {
        Out out(*this, Current());
        out.writer.WriteList(count);
        return out.writer.Error();
    }

    Result<void> EndArray() noexcept override {
        return Result<void>::Success();
    }

    Result<void> BeginSet(usize count) noexcept override {
        return Push(false, count);
    }

    Result<void> EndSet() noexcept override {
        return Pop();
    }

    Result<void> BeginMap(usize count) noexcept override {
        return Push(true, count);
    }

    Result<void> MapKey(const FieldValue& key) noexcept override {
        if (auto started = StartEntry(); !started) {
            return started;
        }

        return WriteValue(key, CurrentKey());
    }

    Result<void> EndMap() noexcept override {
        return Pop();
    }

    Result<void> Instanced(TypeId type, StringView toolName) noexcept override {
        (void)toolName;
        Out out(*this, Current());
        out.writer.WriteGuid(type);
        return out.writer.Error();
    }

private:
    // 기록 대상이다: buffer가 있으면 그 배열에 붙이는 기록기, 없으면 root 기록기.
    struct Out {
        Out(KeyVisitor& visitor, Array<uint8>* buffer) noexcept
            : spare(visitor.allocator_), local(buffer != nullptr ? *buffer : spare),
              writer(buffer != nullptr ? local : visitor.root_) {
        }

        Array<uint8> spare;
        NuggetKeyWriter local;
        NuggetKeyWriter& writer;
    };

    // 기록 대상 버퍼다(Map·Set 밖이면 root 기록기 — nullptr).
    Array<uint8>* Current() noexcept {
        if (frames_.IsEmpty()) {
            return nullptr;
        }

        // 순회 문법상 Map 안 값 이벤트는 늘 MapKey 뒤라 항목이 있다(Set 원소는 Value가 연다).
        SortFrame& frame = frames_[frames_.Size() - 1];
        return &frame.entries[frame.entries.Size() - 1].value;
    }

    Array<uint8>* CurrentKey() noexcept {
        SortFrame& frame = frames_[frames_.Size() - 1];
        return &frame.entries[frame.entries.Size() - 1].key;
    }

    // 항목 하나를 연다(자리는 Push가 원소 수만큼 잡았다 — 순회 문법상 할당하지 않는다. 예약보다
    // 항목이 많으면 순회 결함이고 오류로 멈춘다 — 조용히 앞 항목에 섞지 않는다).
    Result<void> StartEntry() noexcept {
        SortFrame& frame = frames_[frames_.Size() - 1];
        if (frame.entries.Size() == frame.entries.Capacity()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return frame.entries.TryPushBack(SortEntry(allocator_));
    }

    Result<void> Push(bool map, usize count) noexcept {
        SortFrame frame(allocator_);
        frame.map = map;
        if (auto reserved = frame.entries.TryReserve(count); !reserved) {
            return reserved;
        }

        return frames_.TryPushBack(Move(frame));
    }

    // 끝난 Map·Set을 정렬해 바깥 기록 대상에 쓴다.
    Result<void> Pop() noexcept {
        SortFrame frame = Move(frames_[frames_.Size() - 1]);
        (void)frames_.TryPopBack();
        algo::Sort(frame.entries.View(),
                   [](const SortEntry& left, const SortEntry& right) noexcept {
                       return nugget_detail::CompareBytes(left.key.View(), right.key.View()) < 0;
                   });

        Out out(*this, Current());
        if (frame.map) {
            out.writer.WriteMap(frame.entries.Size());
        } else {
            out.writer.WriteSet(frame.entries.Size());
        }

        for (const SortEntry& entry : frame.entries) {
            out.writer.WriteRaw(entry.key.View());
            out.writer.WriteRaw(entry.value.View());
        }

        return out.writer.Error();
    }

    Result<void> WriteValue(const FieldValue& value, Array<uint8>* buffer) noexcept {
        Out out(*this, buffer);
        if (auto encoded = Encode(out.writer, value); !encoded) {
            return encoded;
        }

        return out.writer.Error();
    }

    Result<void> Encode(NuggetKeyWriter& writer, const FieldValue& value) noexcept {
        switch (value.kind) {
        case ValueKind::Bool:
            writer.WriteBool(value.boolean);
            break;
        case ValueKind::Int8:
        case ValueKind::Int16:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::Enum:
            writer.WriteI64(value.signedInteger);
            break;
        case ValueKind::UInt8:
        case ValueKind::UInt16:
        case ValueKind::UInt32:
        case ValueKind::UInt64:
            writer.WriteU64(value.unsignedInteger);
            break;
        case ValueKind::Float32:
            writer.WriteF32(value.float32);
            break;
        case ValueKind::Float64:
            writer.WriteF64(value.float64);
            break;
        case ValueKind::Guid:
        case ValueKind::AssetId:
        case ValueKind::ClassReference:
            writer.WriteGuid(value.guid);
            break;
        case ValueKind::Digest256:
            writer.WriteDigest(value.digest);
            break;
        case ValueKind::Utf8String:
            writer.WriteString(value.string.View());
            break;
        case ValueKind::SoftObjectReference:
            writer.WriteGuid(value.guid);
            writer.WriteString(value.string.View());
            break;
        case ValueKind::Name: {
            if (names_ == nullptr) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            auto lease = names_->Acquire(value.name);
            if (!lease) {
                return Result<void>::Failure(lease.Error());
            }

            // Name은 ASCII 대소문자 무시 동등이고 표시 철자는 먼저 등록한 쪽이라 소문자로 접는다.
            const StringView text = lease.Value().View();
            Array<char> folded(allocator_);
            if (auto sized = folded.TryResize(text.ByteCount()); !sized) {
                return sized;
            }

            for (usize index = 0; index < text.ByteCount(); ++index) {
                const char c = text.Data()[index];
                folded[index] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            }

            writer.WriteString(StringView(folded.Data(), folded.Size()));
            break;
        }
        default:
            // ObjectRef·WeakObjectRef(실행 중 handle)·Text(지역화 원문이 바뀔 수 있다)는 키에 쓸 수
            // 없다.
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        return Result<void>::Success();
    }

    NuggetKeyWriter& root_;
    NamePool* names_;
    Allocator& allocator_;
    Array<SortFrame> frames_;
};

} // namespace

Result<void> WriteReflected(NuggetKeyWriter& writer, const AssignmentTarget& target) noexcept {
    if (auto failed = writer.Error(); !failed) {
        return failed;
    }

    TextCodecOptions options;
    options.use = TextUse::Report;
    KeyVisitor visitor(writer, target.Registry().Names(), target.ValueAllocator());
    auto visited = reflection_text::VisitValues(target, visitor, options);
    if (!visited) {
        // 순회의 오류를 기록기 상태로도 남긴다(Finish가 같은 오류를 돌려준다).
        writer.Fail(visited.Error());
        return visited;
    }

    return writer.Error();
}

} // namespace rocks
