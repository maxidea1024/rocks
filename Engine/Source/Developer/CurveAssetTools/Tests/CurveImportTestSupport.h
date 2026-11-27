#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <CurveAssets/CurveAssetsModule.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 커브 임포터 시험 지원이다(Docs/Design/CurveAssets 7.4절 CurveImport_): 타입·이름 pool·Registry
// 환경과 출력·경고·실패를 모으는 가짜 임포트 문맥.
namespace rocks::curve_import_test {

// allocator는 실패를 주입할 수 있다(failing — 기본은 실패 없음). 이름 pool·타입·Registry가 모두
// 쓴다.
struct CurveEnv {
    CurveEnv() noexcept
        : failing(system), allocator(failing), names(allocator), types(allocator),
          registration(types, allocator),
          ready(names.Initialize().IsOk() && RegisterCurveAssetsTypes(registration).IsOk()),
          objects(allocator, types, ContextId{0x43414933ull, 1}, Options(names)) {
    }

    ~CurveEnv() noexcept {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
    }

    static RegistryOptions Options(NamePool& pool) noexcept {
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    Name Of(StringView text) noexcept {
        auto name = names.TryIntern(text);
        return name ? name.Value() : Name{};
    }

    SystemAllocator system;
    test::NFailAllocator failing;
    Allocator& allocator;
    NamePool names;
    TypeRegistry types;
    ModuleRegistration registration;
    bool ready = false;
    ObjectRegistry objects;
};

struct SeenWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    uint64 detail = 0;
};

// 가짜 임포트 문맥이다. 새 임포트는 Output이 새 출력 객체를, 재임포트는 existing을 준다.
class FakeCurveImportContext final : public AssetImportContext {
public:
    FakeCurveImportContext(CurveEnv& env, StringView path, Span<const uint8> bytes,
                           const void* options, ObjectHandle existing = {}) noexcept
        : warnings(env.allocator), env_(&env), access_(env.objects.OwnerAccess()),
          source_{DefaultImportSourceRole, path, bytes}, options_(options), existing_(existing),
          roots_(env.allocator) {
    }

    Span<const AssetImportSource> Sources() const noexcept override {
        return Span<const AssetImportSource>(&source_, 1);
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

        auto made = env_->objects.NewObject<Asset>(access_, type);
        if (!made) {
            return Result<ObjectHandle>::Failure(made.Error());
        }

        output = made.Value().Handle();
        if (auto pushed = roots_.TryPushBack(Move(made.Value())); !pushed) {
            return Result<ObjectHandle>::Failure(pushed.Error());
        }

        return Result<ObjectHandle>::Success(output);
    }

    Result<BulkDataRef> AddBulk(ObjectHandle, Array<uint8>&&) noexcept override {
        return Result<BulkDataRef>::Failure(ErrorCode::Unsupported);
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

    // 출력 객체를 T로 빌려 fn(T&)를 부른다. 출력이 없거나 T가 아니면 거짓이다.
    template <class T, class Fn>
    bool With(Fn&& fn) noexcept {
        auto borrowed = env_->objects.ResolveForWrite<T>(access_, output);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return false;
        }

        fn(*value.Value());
        return true;
    }

    ObjectHandle output{};
    Array<SeenWarning> warnings;
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    ErrorCode failError = ErrorCode::None;
    uint64 failDetail = 0;

private:
    CurveEnv* env_;
    ObjectAccessContext access_;
    AssetImportSource source_;
    const void* options_;
    ObjectHandle existing_;
    Array<RootRef<Asset>> roots_;
};

// 텍스트의 byte다.
inline Span<const uint8> BytesOf(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

} // namespace rocks::curve_import_test
