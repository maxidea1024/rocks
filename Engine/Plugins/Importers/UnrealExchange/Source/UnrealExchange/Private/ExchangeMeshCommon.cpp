#include "ExchangeMeshCommon.h"

#include <Core/Text/StringBuilder.h>
#include <Mesh/Common/MeshUvDensity.generated.h>

// 교환 메시 계열 변환의 공용 값 읽기와 문맥이다(Docs/Design/UnrealExchange 6.4절 7번).

namespace rocks::exchange_detail {

usize FindAssetBlob(const ExchangeAsset& asset, JsonValue reference) noexcept {
    StringView hex;
    StringView layout;
    uint64 count = 0;
    uint64 components = 0;
    if (!ReadString(Member(reference, "blob"), hex) ||
        !ReadString(Member(reference, "layout"), layout) ||
        !ReadUInt(Member(reference, "count"), count) ||
        !ReadUInt(Member(reference, "components"), components)) {
        return asset.blobs.Size();
    }

    auto digest = Sha256Digest::ParseHex(hex);
    if (!digest) {
        return asset.blobs.Size();
    }

    for (usize index = 0; index < asset.blobs.Size(); ++index) {
        const ExchangeBlobRef& blob = asset.blobs[index];
        if (blob.digest == digest.Value() && blob.count == count && blob.components == components &&
            StringView(ExchangeBlobLayoutName(blob.layout)) == layout) {
            return index;
        }
    }

    return asset.blobs.Size();
}

bool Number(JsonValue value, double& out) noexcept {
    return static_cast<bool>(ReadExchangeNumber(value, out));
}

bool OptNumber(JsonValue value, double& out) noexcept {
    return !value.IsValid() || Number(value, out);
}

bool OptFloat(JsonValue value, float& out) noexcept {
    double read = out;
    if (!OptNumber(value, read)) {
        return false;
    }

    out = static_cast<float>(read);
    return true;
}

bool OptBool(JsonValue value, bool& out) noexcept {
    return !value.IsValid() || ReadBool(value, out);
}

bool OptInt32(JsonValue value, int32& out) noexcept {
    return !value.IsValid() || ReadInt32(value, out);
}

bool OptString(JsonValue value, StringView& out) noexcept {
    return !value.IsValid() || ReadString(value, out);
}

bool OptVector(JsonValue value, Vec3d& out) noexcept {
    if (!value.IsValid()) {
        return true;
    }

    Vec3d read;
    if (!Number(Member(value, "X"), read.x) || !Number(Member(value, "Y"), read.y) ||
        !Number(Member(value, "Z"), read.z)) {
        return false;
    }

    out = read;
    return true;
}

bool OptRotator(JsonValue value, Rotator<double>& out) noexcept {
    if (!value.IsValid()) {
        return true;
    }

    Rotator<double> read;
    if (!Number(Member(value, "Pitch"), read.pitch) || !Number(Member(value, "Yaw"), read.yaw) ||
        !Number(Member(value, "Roll"), read.roll)) {
        return false;
    }

    out = read;
    return true;
}

bool OptQuat(JsonValue value, Quatd& out) noexcept {
    if (!value.IsValid()) {
        return true;
    }

    Quatd read;
    if (!Number(Member(value, "X"), read.x) || !Number(Member(value, "Y"), read.y) ||
        !Number(Member(value, "Z"), read.z) || !Number(Member(value, "W"), read.w)) {
        return false;
    }

    out = read;
    return true;
}

bool OptArray(JsonValue value) noexcept {
    return !value.IsValid() || value.IsArray();
}

bool PerPlatformInt(JsonValue value, int32& out) noexcept {
    return !value.IsValid() || ReadInt32(Member(value, "defaultValue"), out);
}

Result<void> ExchangeMeshContext::Fail(AssetDiagnosticCode code, ErrorCode error,
                                       uint64 detail) noexcept {
    failure.code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
    failure.detail = detail;
    return Result<void>::Failure(error);
}

Result<void> ExchangeMeshContext::Warn(AssetDiagnosticCode code, uint64 detail,
                                       StringView key) noexcept {
    ExchangeSettingWarning warning;
    warning.code = code;
    warning.detail = detail;
    warning.key = key;
    return warnings.TryPushBack(warning);
}

Result<void> ExchangeMeshContext::TranslatedReference(JsonValue value, AssetId& out,
                                                      uint64 detail) noexcept {
    out = AssetId{};
    if (!value.IsValid() || value.IsNull()) {
        return Result<void>::Success();
    }

    if (!value.IsString()) {
        return Corrupt(detail);
    }

    const AssetId* id = references != nullptr ? references->Find(value.GetString()) : nullptr;
    out = id != nullptr ? *id : AssetId{};
    return Result<void>::Success();
}

Result<void> ExchangeMeshContext::DumpReference(JsonValue value, AssetId& out, bool& present,
                                                uint64 detail) noexcept {
    out = AssetId{};
    present = false;
    if (!value.IsValid() || value.IsNull()) {
        return Result<void>::Success();
    }

    StringView path;
    if (!value.IsObject() || value.Size() != 1 || !ReadString(Member(value, "objectPath"), path)) {
        return Corrupt(detail);
    }

    present = true;
    const AssetId* id = references != nullptr ? references->Find(path) : nullptr;
    out = id != nullptr ? *id : AssetId{};
    return Result<void>::Success();
}

Result<void> ExchangeMeshContext::Translate(JsonValue object, Span<const ExchangeSettingRow> rows,
                                            ExchangeClass output, JsonDocument& out) noexcept {
    StringBuilder<> text(allocator);
    JsonWriter writer(text);
    ExchangeSettingInput settings;
    settings.settings = object;
    settings.rows = rows;
    settings.output = output;
    settings.references = references;
    // 번역의 CorruptData만 덤프 내용(값이 행 종류와 맞지 않음)이다. 그 밖(행 표의 결함
    // InvalidArgument, writer 한도·할당)은 코드 없이 그대로다.
    if (auto translated = TranslateExchangeSettings(settings, writer, warnings); !translated) {
        return translated.Error() == ErrorCode::CorruptData
                   ? Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData, 0)
                   : Result<void>::Failure(translated.Error());
    }

    if (auto finished = writer.Finish(); !finished) {
        return Result<void>::Failure(finished.Error());
    }

    const JsonError parsed = out.Parse(text.View());
    if (parsed.code == JsonErrorCode::OutOfMemory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // writer가 쓴 원문이라 읽기 실패는 코드 결함이다(진단 코드 없이).
    return parsed.Succeeded() ? Result<void>::Success()
                              : Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> ExchangeMeshContext::ReadSlots(JsonValue items, Array<StaticMeshImportSlot>& out,
                                            uint64 detail) noexcept {
    if (!OptArray(items)) {
        return Corrupt(detail);
    }

    for (usize index = 0; index < items.Size(); ++index) {
        const JsonValue item = items.At(index);
        StaticMeshImportSlot slot;
        if (!item.IsObject() || !OptString(Member(item, "MaterialSlotName"), slot.slotName) ||
            !OptString(Member(item, "ImportedMaterialSlotName"), slot.importedSlotName)) {
            return Corrupt(detail);
        }

        slot.slotName = WithoutNone(slot.slotName);
        slot.importedSlotName = WithoutNone(slot.importedSlotName);
        if (auto read =
                TranslatedReference(Member(item, "MaterialInterface"), slot.material, detail);
            !read) {
            return read;
        }

        if (auto read = TranslatedReference(Member(item, "OverlayMaterialInterface"),
                                            slot.overlayMaterial, detail);
            !read) {
            return read;
        }

        if (auto read = ImportValue(Member(item, "UVChannelData"), slot.uvDensity, detail); !read) {
            return read;
        }

        if (auto pushed = out.TryPushBack(slot); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// UE EBoneTranslationRetargetingMode 값 이름(BoneTranslationRetarget 순서 — 6.3절 1번)이다.
constexpr StringView RetargetModeNames[] = {"Animation", "Skeleton", "AnimationScaled",
                                            "AnimationRelative", "OrientAndScale"};
// pose 배열 길이(이동·크기 셋, 회전 넷)다.
constexpr usize VectorComponents = 3;
constexpr usize QuaternionComponents = 4;

// count개 숫자 배열이다.
bool Numbers(JsonValue array, double* out, usize count) noexcept {
    if (!array.IsArray() || array.Size() != count) {
        return false;
    }

    for (usize index = 0; index < count; ++index) {
        if (!Number(array.At(index), out[index])) {
            return false;
        }
    }

    return true;
}

Result<void>
ExchangeMeshContext::ReadBones(JsonValue bones, Array<SkeletonImportBone>& out,
                               Array<BoneTranslationRetarget>* retargetModes) noexcept {
    if (!bones.IsArray()) {
        return Corrupt(0);
    }

    for (usize index = 0; index < bones.Size(); ++index) {
        const JsonValue item = bones.At(index);
        const JsonValue pose = Member(item, "pose");
        SkeletonImportBone bone;
        double translation[VectorComponents] = {};
        double rotation[QuaternionComponents] = {};
        double scale[VectorComponents] = {};
        if (!ReadString(Member(item, "name"), bone.name) ||
            !ReadInt32(Member(item, "parent"), bone.parentIndex) ||
            !Numbers(Member(pose, "translation"), translation, VectorComponents) ||
            !Numbers(Member(pose, "rotation"), rotation, QuaternionComponents) ||
            !Numbers(Member(pose, "scale"), scale, VectorComponents)) {
            return Corrupt(index);
        }

        bone.referencePose = Transform(Quatd(rotation[0], rotation[1], rotation[2], rotation[3]),
                                       Vec3d(translation[0], translation[1], translation[2]),
                                       Vec3d(scale[0], scale[1], scale[2]));
        if (retargetModes != nullptr) {
            StringView mode;
            if (!ReadString(Member(item, "retargetMode"), mode)) {
                return Corrupt(index);
            }

            const usize found = FindName(RetargetModeNames, mode);
            if (found == sizeof(RetargetModeNames) / sizeof(RetargetModeNames[0])) {
                return Corrupt(index);
            }

            if (auto pushed =
                    retargetModes->TryPushBack(static_cast<BoneTranslationRetarget>(found));
                !pushed) {
                return pushed;
            }
        }

        if (auto pushed = out.TryPushBack(bone); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> ExchangeMeshContext::ReadBoneSockets(JsonValue items, Array<SkeletonImportSocket>& out,
                                                  uint64 detail) noexcept {
    if (!OptArray(items)) {
        return Corrupt(detail);
    }

    for (usize index = 0; index < items.Size(); ++index) {
        // 같은 하위 객체를 다시 가리키는 항목({subobject}만)은 공유라 받지 않는다.
        const JsonValue properties = Member(items.At(index), "properties");
        SkeletonImportSocket socket;
        if (!properties.IsObject() || !OptString(Member(properties, "SocketName"), socket.name) ||
            !OptString(Member(properties, "BoneName"), socket.boneName) ||
            !OptVector(Member(properties, "RelativeLocation"), socket.relativeLocation) ||
            !OptRotator(Member(properties, "RelativeRotation"), socket.relativeRotation) ||
            !OptVector(Member(properties, "RelativeScale"), socket.relativeScale) ||
            !OptBool(Member(properties, "ForceAlwaysAnimated"), socket.forceAlwaysAnimated)) {
            return Corrupt(detail);
        }

        socket.name = WithoutNone(socket.name);
        socket.boneName = WithoutNone(socket.boneName);
        if (auto pushed = out.TryPushBack(socket); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::exchange_detail
