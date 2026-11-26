#pragma once

#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetImporter.h>
#include <AssetTestFixtures/TestAssets.generated.h>
#include <Core/Text/NumberText.h>

// 시험 임포터다(Docs/Design/AssetSystem 7.1절 18 A2f — 서비스 시험과 명령 시험이 같이
// 쓴다). 원본은 줄마다 `<이름>=<값>`인 텍스트다:
//   value=<정수>              주 출력 TestAsset::value = 정수 × 옵션 scale
//   label=<텍스트>            주 출력 TestAsset::label
//   bulk=<16진 byte>          주 출력 TestAsset::pixels(저장하지 않은 bulk)
//   outputs=<key>:<타입>,…    추가 출력(타입
//   TestAsset·BaseGameAsset·DerivedGameAsset·Asset·TestPart) reference=<key>           주 출력
//   TestAsset::hard = 그 출력 warn=<코드 번호>          Warn(그 코드, 7) nomain=1 주 출력을 만들지
//   않는다 mainType=<타입>           주 출력을 그 타입으로 청한다(Output 규칙 시험) bulkForeign=1
//   추가 출력이 아닌 객체에 AddBulk tags=<key>|<value>,… 임포터 태그(A2f3a — 값에 `,`·`|` 없음)
//   keeptags=1 옛 기록 태그(PreviousTags)를 다시 더한다
//   open=<AssetId>            OpenAsset(그 id)로 연 대표 에셋을 TestAsset::hard에 건다(A2f3c —
//   reference보다 앞선다, 실패하면 그 오류로 끝낸다)
//   openmember=<번호>         OpenAsset(MemberAsset(번호))를 hard에 건다(A2f3d 묶음 — open보다
//   앞선다)
//   edit=<AssetId>,…          EditAsset(그 id)로 연 TestAsset의 value를 1 늘린다(A2f4 — 같은 id를
//   다시 적으면 또 늘린다, 실패하면 그 오류로 끝낸다)
//   editmember=<번호>         EditAsset(MemberAsset(번호))(A2f4 거부 시험)
// 옵션 TestImportOptions{scale, failAt}: failAt 1이면 원본을 읽은 뒤 Fail(InvalidSource,
// InvalidArgument, 5), 2면 주 출력을 만든 뒤 Failure(Busy)(진단 없이), 3이면 모든 출력 뒤 Fail.
// 역할 변형(roles px·nx)은 두 원본의 value를 더한다.
namespace rocks::import_test {

using asset_fixture::TestAsset;
using asset_fixture::TestImportOptions;
using rocks::asset_test::IdOf;
using rocks::asset_test::Pointer;

inline constexpr Guid TestImporterId{0x494d50540001ull, 1};
inline constexpr Guid CubeImporterId{0x494d50540001ull, 2};
inline constexpr Guid PlainImporterId{0x494d50540001ull, 3};
inline constexpr StringView TestImportExtensions[] = {"trock"};
inline constexpr StringView CubeImportRoles[] = {"px", "nx"};
inline constexpr StringView CubeImportExtensions[] = {"tcube"};
inline constexpr uint32 TestImporterVersion = 4;

// 원본 텍스트의 `<이름>=` 줄 값이다(없으면 false).
inline bool FindLine(Span<const uint8> bytes, StringView name, StringView& value) noexcept {
    const StringView text(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    usize start = 0;
    for (usize index = 0; index <= text.ByteCount(); ++index) {
        if (index < text.ByteCount() && text.Data()[index] != '\n') {
            continue;
        }

        const StringView line = text.SliceBytes(start, index - start);
        if (line.ByteCount() > name.ByteCount() && line.StartsWith(name) &&
            line.Data()[name.ByteCount()] == '=') {
            value = line.SliceBytes(name.ByteCount() + 1, line.ByteCount() - name.ByteCount() - 1);
            return true;
        }

        start = index + 1;
    }

    return false;
}

inline TypeId TypeNamed(StringView name) noexcept {
    if (name == "BaseGameAsset") {
        return IdOf<asset_fixture::BaseGameAsset>();
    }

    if (name == "DerivedGameAsset") {
        return IdOf<asset_fixture::DerivedGameAsset>();
    }

    if (name == "Asset") {
        return GeneratedType<Asset>::Id();
    }

    if (name == "TestPart") {
        return IdOf<asset_fixture::TestPart>();
    }

    if (name == "Options") {
        return GeneratedType<TestImportOptions>::Id();
    }

    if (name == "Unknown") {
        return Guid{0x7777, 3};
    }

    return IdOf<TestAsset>();
}

class TestImporter final : public AssetImporter {
public:
    explicit TestImporter(const AssetImporterInfo& info) noexcept : info_(info) {
    }

    const AssetImporterInfo& Info() const noexcept override {
        return info_;
    }

    Result<void> Import(AssetImportContext& context) noexcept override {
        ++calls;
        const TestImportOptions* options = static_cast<const TestImportOptions*>(context.Options());
        const int32 scale = options != nullptr ? options->scale : 1;
        const int32 failAt = options != nullptr ? options->failAt : 0;
        if (failAt == 1) {
            return context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, 5);
        }

        // 받은 원본 역할 순서를 남긴다(`role,role,…` — 가변 원본 순서 시험, A2f3b).
        lastRolesBytes = 0;
        for (const AssetImportSource& source : context.Sources()) {
            for (usize at = 0;
                 at < source.role.ByteCount() && lastRolesBytes + 1 < sizeof(lastRoles); ++at) {
                lastRoles[lastRolesBytes++] = source.role.Data()[at];
            }

            if (lastRolesBytes + 1 < sizeof(lastRoles)) {
                lastRoles[lastRolesBytes++] = ',';
            }
        }

        int64 total = 0;
        for (const AssetImportSource& source : context.Sources()) {
            StringView text;
            if (FindLine(source.bytes, "value", text)) {
                auto parsed = TryParseInt64(text);
                total += parsed ? parsed.Value() : 0;
            }
        }

        const Span<const uint8> first = context.Sources()[0].bytes;
        StringView option;
        TypeId mainType = info_.outputType;
        if (FindLine(first, "mainType", option)) {
            mainType = TypeNamed(option);
        }

        ObjectHandle main{};
        if (!FindLine(first, "nomain", option)) {
            auto made = context.Output({}, mainType);
            if (!made) {
                return Result<void>::Failure(made.Error());
            }

            main = made.Value();
            TestAsset* asset = Pointer<TestAsset>(context.Registry(), main);
            asset->value = static_cast<int32>(total * scale);
            if (FindLine(first, "label", option) && !asset->label.TryAssign(option)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            if (FindLine(first, "bulk", option)) {
                Array<uint8> raw(context.Registry().ObjectAllocator());
                for (usize index = 0; index + 1 < option.ByteCount(); index += 2) {
                    auto digit = [](char c) noexcept {
                        return static_cast<uint8>(c <= '9' ? c - '0' : c - 'a' + 10);
                    };
                    if (!raw.TryPushBack(static_cast<uint8>(digit(option.Data()[index]) * 16 +
                                                            digit(option.Data()[index + 1])))) {
                        return Result<void>::Failure(ErrorCode::OutOfMemory);
                    }
                }

                auto bulk = context.AddBulk(main, rocks::Move(raw));
                if (!bulk) {
                    return Result<void>::Failure(bulk.Error());
                }

                asset->pixels = bulk.Value();
            }
        }

        if (failAt == 2) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        // 추가 출력.
        ObjectHandle referenced{};
        StringView reference;
        const bool wantsReference = FindLine(first, "reference", reference);
        if (FindLine(first, "outputs", option)) {
            usize start = 0;
            for (usize index = 0; index <= option.ByteCount(); ++index) {
                if (index < option.ByteCount() && option.Data()[index] != ',') {
                    continue;
                }

                const StringView item = option.SliceBytes(start, index - start);
                start = index + 1;
                usize colon = 0;
                while (colon < item.ByteCount() && item.Data()[colon] != ':') {
                    ++colon;
                }

                const StringView key = item.SliceBytes(0, colon);
                const StringView type =
                    colon < item.ByteCount()
                        ? item.SliceBytes(colon + 1, item.ByteCount() - colon - 1)
                        : StringView("TestAsset");
                auto made = context.Output(key, TypeNamed(type));
                if (!made) {
                    return Result<void>::Failure(made.Error());
                }

                if (TypeNamed(type) == IdOf<TestAsset>()) {
                    Pointer<TestAsset>(context.Registry(), made.Value())->value =
                        static_cast<int32>(total);
                }

                if (wantsReference && key == reference) {
                    referenced = made.Value();
                }
            }
        }

        // 다른 에셋 참조(A2f3c UXR10)·묶음 구성원(A2f3d UXR11).
        lastOpenError = ErrorCode::None;
        lastGroupSize = context.GroupSize();
        lastGroupIndex = context.GroupIndex();
        if (FindLine(first, "openmember", option)) {
            auto index = TryParseInt64(option);
            if (!index || index.Value() < 0) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            auto opened = context.OpenAsset(context.MemberAsset(static_cast<usize>(index.Value())));
            if (!opened) {
                lastOpenError = opened.Error();
                return Result<void>::Failure(opened.Error());
            }

            referenced = opened.Value();
        } else if (FindLine(first, "open", option)) {
            auto id = AssetId::TryParse(option);
            if (!id) {
                return Result<void>::Failure(id.Error());
            }

            auto opened = context.OpenAsset(id.Value());
            if (!opened) {
                lastOpenError = opened.Error();
                return Result<void>::Failure(opened.Error());
            }

            referenced = opened.Value();
        }

        // 다른 에셋 고치기(A2f4).
        lastEditError = ErrorCode::None;
        if (FindLine(first, "editmember", option)) {
            auto index = TryParseInt64(option);
            if (!index || index.Value() < 0) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            auto edited = context.EditAsset(context.MemberAsset(static_cast<usize>(index.Value())));
            if (!edited) {
                lastEditError = edited.Error();
                return Result<void>::Failure(edited.Error());
            }
        }

        if (FindLine(first, "edit", option)) {
            usize start = 0;
            for (usize index = 0; index <= option.ByteCount(); ++index) {
                if (index < option.ByteCount() && option.Data()[index] != ',') {
                    continue;
                }

                auto id = AssetId::TryParse(option.SliceBytes(start, index - start));
                start = index + 1;
                if (!id) {
                    return Result<void>::Failure(id.Error());
                }

                auto edited = context.EditAsset(id.Value());
                if (!edited) {
                    lastEditError = edited.Error();
                    return Result<void>::Failure(edited.Error());
                }

                TestAsset* target = Pointer<TestAsset>(context.Registry(), edited.Value());
                if (target == nullptr) {
                    return Result<void>::Failure(ErrorCode::TypeMismatch);
                }

                ++target->value;
            }
        }

        // 임포트가 정하는 필드는 매번 모두 쓴다(참조가 없으면 null).
        if (!main.IsNull()) {
            auto set = context.Registry().SetReference(
                context.Access(), main, Pointer<TestAsset>(context.Registry(), main)->hard,
                referenced);
            if (!set) {
                return Result<void>::Failure(set.Error());
            }
        }

        if (FindLine(first, "bulkForeign", option)) {
            auto bulk =
                context.AddBulk(ObjectHandle{}, Array<uint8>(context.Registry().ObjectAllocator()));
            if (!bulk) {
                return Result<void>::Failure(bulk.Error());
            }
        }

        if (FindLine(first, "warn", option)) {
            auto code = TryParseInt64(option);
            if (auto warned =
                    context.Warn(static_cast<AssetDiagnosticCode>(code ? code.Value() : 0), 7);
                !warned) {
                return warned;
            }
        }

        // 임포터 태그(A2f3a UXR8).
        lastPreviousTags = context.PreviousTags().Size();
        if (FindLine(first, "keeptags", option)) {
            for (const ImportTag& tag : context.PreviousTags()) {
                if (auto added = context.AddTag(tag.key.View(), tag.value.View()); !added) {
                    return added;
                }
            }
        }

        if (FindLine(first, "tags", option)) {
            usize start = 0;
            for (usize index = 0; index <= option.ByteCount(); ++index) {
                if (index < option.ByteCount() && option.Data()[index] != ',') {
                    continue;
                }

                const StringView item = option.SliceBytes(start, index - start);
                start = index + 1;
                usize bar = 0;
                while (bar < item.ByteCount() && item.Data()[bar] != '|') {
                    ++bar;
                }

                const StringView value = bar < item.ByteCount()
                                             ? item.SliceBytes(bar + 1, item.ByteCount() - bar - 1)
                                             : StringView();
                if (auto added = context.AddTag(item.SliceBytes(0, bar), value); !added) {
                    return added;
                }
            }
        }

        if (failAt == 3) {
            return context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, 6);
        }

        lastReimport = context.IsReimport();
        return Result<void>::Success();
    }

    AssetImporterInfo info_;
    uint32 calls = 0;
    // 마지막으로 성공한 Import가 재임포트였는지다.
    bool lastReimport = false;
    // 마지막 Import가 받은 옛 기록 태그 수다(A2f3a).
    usize lastPreviousTags = 0;
    // 마지막 Import의 OpenAsset 오류다(None이면 성공이거나 열지 않음 — A2f3c).
    ErrorCode lastOpenError = ErrorCode::None;
    // 마지막 Import의 EditAsset 오류다(None이면 성공이거나 고치지 않음 — A2f4).
    ErrorCode lastEditError = ErrorCode::None;
    // 마지막 Import가 본 묶음 구성원 수·자기 번호다(A2f3d).
    usize lastGroupSize = 0;
    usize lastGroupIndex = 0;
    // 마지막 Import가 받은 원본 역할 순서(`role,` 반복)다.
    char lastRoles[512] = {};
    usize lastRolesBytes = 0;

    StringView LastRoles() const noexcept {
        return StringView(lastRoles, lastRolesBytes);
    }
};

inline AssetImporterInfo TestImporterInfo() noexcept {
    AssetImporterInfo info;
    info.id = TestImporterId;
    info.name = "TestImporter";
    info.version = TestImporterVersion;
    info.outputType = IdOf<TestAsset>();
    info.extensions = TestImportExtensions;
    info.optionsType = GeneratedType<TestImportOptions>::Id();
    return info;
}

// 역할 둘(px·nx)·옵션 없음·기본 확장자 tcube다.
inline AssetImporterInfo CubeImporterInfo() noexcept {
    AssetImporterInfo info;
    info.id = CubeImporterId;
    info.name = "CubeImporter";
    info.version = 1;
    info.outputType = IdOf<TestAsset>();
    info.roles = CubeImportRoles;
    info.extensions = CubeImportExtensions;
    return info;
}

} // namespace rocks::import_test
