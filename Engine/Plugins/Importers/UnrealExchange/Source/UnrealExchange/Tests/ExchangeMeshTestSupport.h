#pragma once

#include "ExchangeTestSupport.h"

#include <Core/Text/Json.h>
#include <Mesh/MeshBuildSettings.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 교환 메시 계열 변환 시험 공용 도구다(Docs/Design/UnrealExchange 5.5절 12번·6.4절 10번): 메시 값
// struct 타입을 가진 Registry 환경, golden 묶음 blob 읽기, 문서 경로 편집(변형 입력 — golden 문서를
// 다시 쓴 사본)과 시험 blob 더하기.
namespace rocks::exchange_test {

// float 비교 허용 오차다.
inline constexpr double Tolerance = 1.0e-4;

inline bool Near(double left, double right) noexcept {
    const double difference = left - right;
    return difference < Tolerance && difference > -Tolerance;
}

// Mesh 값 struct 타입과 Name pool을 가진 Registry 환경이다(ImportJson 대상 — 산 객체는 없다).
struct MeshEnv {
    explicit MeshEnv(Allocator& allocator) noexcept
        : types(allocator), modules(types, allocator), names(allocator),
          objects(allocator, types, ContextId{0x534dull, 1}, Options(names)),
          access(objects.OwnerAccess()) {
        registered = static_cast<bool>(RegisterMeshTypes(modules));
    }

    ~MeshEnv() {
        CHECK(objects.Shutdown(access));
        CHECK(names.Shutdown());
        CHECK(modules.Unregister());
        CHECK(types.Shutdown());
    }

    static RegistryOptions Options(NamePool& pool) noexcept {
        (void)pool.Initialize();
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    ObjectRegistry objects;
    ObjectAccessContext access;
    bool registered = false;
};

inline uint32 FindAsset(const ExchangeBundle& bundle, StringView path) noexcept {
    for (uint32 index = 0; index < bundle.Assets().Size(); ++index) {
        if (bundle.Assets()[index].objectPath == path) {
            return index;
        }
    }

    return ExchangeNoIndex;
}

inline bool LoadBlobs(const ExchangeBundle& bundle, uint32 asset, Array<Array<uint8>>& bytes,
                      Array<Span<const uint8>>& spans) noexcept {
    ExchangeDiagnostics diagnostics(bytes.GetAllocator());
    for (const ExchangeBlobRef& blob : bundle.Assets()[asset].blobs) {
        auto made = bytes.TryEmplaceBack(bytes.GetAllocator());
        if (!made || !LoadExchangeBlob(bundle, asset, blob, *made.Value(), diagnostics)) {
            return false;
        }
    }

    for (const Array<uint8>& item : bytes) {
        if (!spans.TryPushBack(item.View())) {
            return false;
        }
    }

    return true;
}

// 문서 경로 편집 하나다. path는 `.`로 이은 키·배열 번호이고 json은 그 자리에 쓸 JSON 원문이다(비면
// 그 멤버를 뺀다). path가 `<배열 경로>.+`이면 배열 끝에 덧붙인다.
struct JsonEdit {
    StringView path;
    StringView json;
};

inline Result<void> WriteEdited(JsonWriter& writer, JsonValue value, StringView path,
                                Span<const JsonEdit> edits, Allocator& allocator) noexcept;

// json 원문을 값 하나로 쓴다.
inline Result<void> WriteText(JsonWriter& writer, StringView json, Allocator& allocator) noexcept {
    JsonDocument document(allocator);
    if (!document.Parse(json).Succeeded()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return WriteEdited(writer, document.Root(), "<none>", {}, allocator);
}

inline const JsonEdit* FindEdit(Span<const JsonEdit> edits, StringView path) noexcept {
    for (const JsonEdit& edit : edits) {
        if (edit.path == path) {
            return &edit;
        }
    }

    return nullptr;
}

inline Result<void> ChildPath(StringView path, StringView key, StringBuilderBase& out) noexcept {
    if (!path.IsEmpty()) {
        if (auto appended = out.TryAppend(path); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('.'); !appended) {
            return appended;
        }
    }

    return out.TryAppend(key);
}

inline Result<void> WriteEdited(JsonWriter& writer, JsonValue value, StringView path,
                                Span<const JsonEdit> edits, Allocator& allocator) noexcept {
    if (const JsonEdit* edit = FindEdit(edits, path); edit != nullptr) {
        return WriteText(writer, edit->json, allocator);
    }

    if (value.IsArray()) {
        if (auto begun = writer.BeginArray(); !begun) {
            return begun;
        }

        for (usize index = 0; index < value.Size(); ++index) {
            StringBuilder<16> key(allocator);
            StringBuilder<256> child(allocator);
            if (!key.TryAppendFormat("{}", index) || !ChildPath(path, key.View(), child)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            const JsonEdit* edit = FindEdit(edits, child.View());
            if (edit != nullptr && edit->json.IsEmpty()) {
                continue;
            }

            if (auto written = WriteEdited(writer, value.At(index), child.View(), edits, allocator);
                !written) {
                return written;
            }
        }

        StringBuilder<256> append(allocator);
        if (!ChildPath(path, "+", append)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        for (const JsonEdit& edit : edits) {
            if (edit.path == append.View()) {
                if (auto written = WriteText(writer, edit.json, allocator); !written) {
                    return written;
                }
            }
        }

        return writer.EndArray();
    }

    if (value.IsObject()) {
        if (auto begun = writer.BeginObject(); !begun) {
            return begun;
        }

        for (usize index = 0; index < value.Size(); ++index) {
            StringBuilder<256> child(allocator);
            if (!ChildPath(path, value.KeyAt(index), child)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            const JsonEdit* edit = FindEdit(edits, child.View());
            if (edit != nullptr && edit->json.IsEmpty()) {
                continue;
            }

            if (auto keyed = writer.Key(value.KeyAt(index)); !keyed) {
                return keyed;
            }

            if (auto written =
                    WriteEdited(writer, value.ValueAt(index), child.View(), edits, allocator);
                !written) {
                return written;
            }
        }

        // 없는 키를 더하는 편집(`<경로>.<새 키>` — 객체 바로 아래만)이다.
        for (const JsonEdit& edit : edits) {
            if (edit.json.IsEmpty() || edit.path.ByteCount() <= path.ByteCount() ||
                !edit.path.StartsWith(path, StringComparison::CaseSensitive)) {
                continue;
            }

            StringView rest =
                edit.path.SliceBytes(path.ByteCount(), edit.path.ByteCount() - path.ByteCount());
            if (!path.IsEmpty()) {
                if (rest.Data()[0] != '.') {
                    continue;
                }

                rest = rest.SliceBytes(1, rest.ByteCount() - 1);
            }

            bool nested = false;
            for (usize at = 0; at < rest.ByteCount(); ++at) {
                nested = nested || rest.Data()[at] == '.';
            }

            if (nested || rest == "+" || value.Find(rest).IsValid()) {
                continue;
            }

            if (auto keyed = writer.Key(rest); !keyed) {
                return keyed;
            }

            if (auto written = WriteText(writer, edit.json, allocator); !written) {
                return written;
            }
        }

        return writer.EndObject();
    }

    return writer.Value(value);
}

// value를 편집해 compact 원문으로 out에 쓴다.
inline bool ToText(JsonValue value, Span<const JsonEdit> edits, Allocator& allocator,
                   StringBuilder<>& out) noexcept {
    out.Clear();
    JsonWriter writer(out);
    return WriteEdited(writer, value, "", edits, allocator) && writer.Finish();
}

// 편집한 정적 메시 교환 에셋이다(blob은 base 그대로 + extra).
struct EditedAsset {
    explicit EditedAsset(Allocator& allocator) noexcept
        : document(allocator), asset(allocator), blobs(allocator), extra(allocator) {
    }

    JsonDocument document;
    ExchangeAsset asset;
    Array<Span<const uint8>> blobs;
    Array<Array<uint8>> extra;
};

inline bool Edit(Allocator& allocator, const ExchangeAsset& base,
                 Span<const Span<const uint8>> blobs, Span<const JsonEdit> edits,
                 EditedAsset& out) noexcept {
    StringBuilder<> text(allocator);
    if (!ToText(base.document->Root(), edits, allocator, text) ||
        !out.document.Parse(text.View()).Succeeded()) {
        return false;
    }

    out.asset.objectPath = base.objectPath;
    out.asset.className = base.className;
    out.asset.exchangeClass = base.exchangeClass;
    out.asset.state = base.state;
    out.asset.settings = out.document.Root().Find("settings");
    out.asset.data = out.document.Root().Find("data");
    return out.asset.blobs.TryAppend(base.blobs.View()) && out.blobs.TryAppend(blobs);
}

// 시험이 만든 blob을 더하고 그 참조 원문({blob, bytes, layout, components, count})을 reference에
// 쓴다.
inline bool AddBlob(EditedAsset& edited, Span<const uint8> bytes, ExchangeBlobLayout layout,
                    uint32 components, uint64 count, StringBuilder<>& reference) noexcept {
    auto copy = edited.extra.TryEmplaceBack(edited.extra.GetAllocator());
    if (!copy || !copy.Value()->TryAppend(bytes)) {
        return false;
    }

    ExchangeBlobRef blob;
    blob.digest = Sha256::Hash(bytes);
    blob.bytes = bytes.Size();
    blob.count = count;
    blob.components = components;
    blob.layout = layout;
    char hex[Sha256Digest::HexBytes + 1];
    blob.digest.ToHex(hex);
    reference.Clear();
    return edited.asset.blobs.TryPushBack(blob) && edited.blobs.TryPushBack(copy.Value()->View()) &&
           reference.TryAppendFormat(
               R"({{"blob": "{}", "bytes": {}, "layout": "{}", "components": {}, "count": {}}})",
               StringView(hex, Sha256Digest::HexBytes), bytes.Size(),
               StringView(ExchangeBlobLayoutName(layout)), components, count);
}

} // namespace rocks::exchange_test
