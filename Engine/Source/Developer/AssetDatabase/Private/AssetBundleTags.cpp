#include <AssetDatabase/AssetBundleTags.h>

#include <AssetRuntime/PackageTags.h>
#include <Core/Containers/Array.h>
#include <Core/Text/StringBuilder.h>
#include <ReflectionText/TextVisit.h>

// bundle 태그 수집 구현이다(Docs/Design/AssetSystem 10.2절 "A4e 구현 세부" — A4e2b). 순회
// 이벤트로 경로의 필드 단계를 쌓고, 소프트 참조 값마다 그 단계들의 목록 교집합으로 태그를 만든다.
namespace rocks {

namespace {

// 태그 키·값 한 줄의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize BundleTagBytes = 128;

// 경로의 필드 한 단계다: 그 필드의 bundle 목록(비면 제한 없음), EditorOnly 경로인지, 그 필드 값
// 안에서 열린 묶음(struct·배열·집합·사전) 수다. 묶음이 모두 닫히거나 단일 값이 끝나면 단계를
// 내린다.
struct BundleFrame {
    Span<const char> bundles;
    bool excluded = false;
    uint32 open = 0;
};

// 스키마가 bundle 목록이나 InstancedStruct(내용 struct가 자기 목록을 가질 수 있다)를 담을 수
// 있는지다(대상 struct 멤버로 내려간다). 아니면 순회하지 않는다 — 저장마다 모든 에셋을 순회하지
// 않는다. fieldStruct는 schemaIndex 멤버의 대상 struct view를 준다.
template <class TFieldStruct>
bool MayCarryBundles(const TypeSchema& schema, const TFieldStruct& fieldStruct) noexcept {
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& field = schema.fields[index];
        const ValueKind target = FieldTargetType(field).kind;
        if (field.assetBundles.Size() != 0 || target == ValueKind::InstancedStruct) {
            return true;
        }

        if (target == ValueKind::Struct) {
            const StructView view = fieldStruct(index);
            if (MayCarryBundles(view.Schema(), [&view](usize member) noexcept {
                    return view.FieldStruct(member);
                })) {
                return true;
            }
        }
    }

    return false;
}

// list(쉼표 목록)에 name이 있는지다.
bool ListContains(Span<const char> list, StringView name) noexcept {
    usize start = 0;
    for (usize end = 0; end <= list.Size(); ++end) {
        if (end < list.Size() && list[end] != ',') {
            continue;
        }

        if (StringView(list.Data() + start, end - start) == name) {
            return true;
        }

        start = end + 1;
    }

    return false;
}

class BundleVisitor final : public reflection_text::ValueVisitor {
public:
    BundleVisitor(Allocator& allocator, AssetTagSink& sink) noexcept
        : allocator_(allocator), sink_(sink), frames_(allocator) {
    }

    Result<void> BeginStruct() noexcept override {
        Open();
        return Result<void>::Success();
    }

    Result<void> EndStruct() noexcept override {
        Close();
        return Result<void>::Success();
    }

    Result<void> Field(StringView name, const FieldSchema& field, uint32 flags) noexcept override {
        (void)name;
        return frames_.TryPushBack(
            BundleFrame{field.assetBundles, HasFlag(flags, FieldFlags::EditorOnly), 0});
    }

    Result<void> Value(const FieldValue& value) noexcept override {
        Result<void> added = Soft(value);
        Complete();
        return added;
    }

    Result<void> Absent() noexcept override {
        Complete();
        return Result<void>::Success();
    }

    Result<void> Bytes(Span<const uint8> bytes) noexcept override {
        (void)bytes;
        Complete();
        return Result<void>::Success();
    }

    Result<void> BeginArray(usize count) noexcept override {
        (void)count;
        Open();
        return Result<void>::Success();
    }

    Result<void> EndArray() noexcept override {
        Close();
        return Result<void>::Success();
    }

    Result<void> BeginSet(usize count) noexcept override {
        (void)count;
        Open();
        return Result<void>::Success();
    }

    Result<void> EndSet() noexcept override {
        Close();
        return Result<void>::Success();
    }

    Result<void> BeginMap(usize count) noexcept override {
        (void)count;
        Open();
        return Result<void>::Success();
    }

    // Map 키도 소프트 참조면 구성원이다(UE도 키를 순회한다). 키 뒤에 그 항목의 값이 오므로 단계를
    // 내리지 않는다.
    Result<void> MapKey(const FieldValue& key) noexcept override {
        return Soft(key);
    }

    Result<void> EndMap() noexcept override {
        Close();
        return Result<void>::Success();
    }

    // 내용 struct가 뒤따른다(BeginStruct가 단계를 연다).
    Result<void> Instanced(TypeId type, StringView toolName) noexcept override {
        (void)type;
        (void)toolName;
        return Result<void>::Success();
    }

private:
    // 대상 뿌리 struct(단계 없음)는 세지 않는다.
    void Open() noexcept {
        if (!frames_.IsEmpty()) {
            ++frames_[frames_.Size() - 1].open;
        }
    }

    void Close() noexcept {
        if (!frames_.IsEmpty() && --frames_[frames_.Size() - 1].open == 0) {
            (void)frames_.TryPopBack();
        }
    }

    // 단일 값이 끝났다 — 그 값이 필드 값 자체(열린 묶음 없음)면 단계를 내린다.
    void Complete() noexcept {
        if (!frames_.IsEmpty() && frames_[frames_.Size() - 1].open == 0) {
            (void)frames_.TryPopBack();
        }
    }

    // null 아닌 소프트 참조면 실효 bundle(경로 목록들의 교집합)마다 태그를 더한다.
    Result<void> Soft(const FieldValue& value) noexcept {
        if (value.kind != ValueKind::SoftObjectReference || value.guid == Guid{}) {
            return Result<void>::Success();
        }

        const BundleFrame* first = nullptr;
        for (const BundleFrame& frame : frames_) {
            if (frame.excluded) {
                return Result<void>::Success();
            }

            if (first == nullptr && frame.bundles.Size() != 0) {
                first = &frame;
            }
        }

        if (first == nullptr) {
            return Result<void>::Success();
        }

        usize start = 0;
        for (usize end = 0; end <= first->bundles.Size(); ++end) {
            if (end < first->bundles.Size() && first->bundles[end] != ',') {
                continue;
            }

            const StringView name(first->bundles.Data() + start, end - start);
            start = end + 1;
            bool everywhere = true;
            for (const BundleFrame& frame : frames_) {
                everywhere =
                    everywhere && (frame.bundles.Size() == 0 || ListContains(frame.bundles, name));
            }

            if (!everywhere) {
                continue;
            }

            StringBuilder<BundleTagBytes> key(allocator_);
            StringBuilder<BundleTagBytes> member(allocator_);
            if (auto made = key.TryAppendFormat("{}{}", AssetBundleTagPrefix, name); !made) {
                return made;
            }

            if (auto made = member.TryAppendFormat("{}", AssetId{value.guid}); !made) {
                return made;
            }

            if (auto added = sink_.Add(key.View(), member.View()); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    Allocator& allocator_;
    AssetTagSink& sink_;
    Array<BundleFrame> frames_;
};

} // namespace

Result<void> CollectAssetBundleTags(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    ObjectHandle object, Allocator& allocator,
                                    AssetTagSink& sink) noexcept {
    auto type = registry.AcquireType(access, object);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    const TypeLease& lease = type.Value();
    if (!MayCarryBundles(lease.Schema(),
                         [&lease](usize index) noexcept { return lease.FieldStruct(index); })) {
        return Result<void>::Success();
    }

    BundleVisitor visitor(allocator, sink);
    reflection_text::TextCodecOptions options;
    // 저장되는 값만 본다(Report는 Transient만 뺀다 — EditorOnly는 수집기가 경로로 거른다).
    options.use = reflection_text::TextUse::Report;
    const auto target = reflection_text::AssignmentTarget::Live(registry, access, object);
    return reflection_text::VisitValues(target, visitor, options);
}

} // namespace rocks
