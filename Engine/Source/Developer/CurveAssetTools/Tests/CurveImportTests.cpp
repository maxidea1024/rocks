#include "CurveImportTestSupport.h"

#include <Base/InitializerList.h>
#include <CurveAssetTools/CurveAssetImporterSet.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/CurveTableAsset.generated.h>
#include <CurveAssets/CurveTableImportOptions.generated.h>
#include <CurveAssets/FloatCurveAsset.generated.h>
#include <CurveAssets/VectorCurveAsset.generated.h>
#include <string.h>

// 커브 임포터 시험이다(Docs/Design/CurveAssets 5.1절 — CA3): 등록, CSV 표(Plain·Tangent·건너뜀
// 경고· 실패 사유), 인코딩(BOM·UTF-16·잘못된 byte), JSON 표(원소·항목 건너뜀·문서 오류·값 종류),
// 단일 커브 CSV(칸 수 경고·재임포트가 지키는 필드), 결정성.
using namespace rocks;
using namespace rocks::curve_import_test;

namespace curve_import_tests_file {

namespace {

struct Run {
    Run(CurveEnv& env, AssetImporter& importer, StringView text,
        const CurveTableImportOptions* options = nullptr, ObjectHandle existing = {}) noexcept
        : context(env, "Data.csv", BytesOf(text), options, existing) {
        result = importer.Import(context);
    }

    Run(CurveEnv& env, AssetImporter& importer, Span<const uint8> bytes) noexcept
        : context(env, "Data.csv", bytes, nullptr) {
        result = importer.Import(context);
    }

    FakeCurveImportContext context;
    Result<void> result = Result<void>::Success();
};

bool WarnedLines(const Run& run, std::initializer_list<uint64> lines) noexcept {
    if (run.context.warnings.Size() != lines.size()) {
        return false;
    }

    usize index = 0;
    for (const uint64 line : lines) {
        const SeenWarning& warning = run.context.warnings[index++];
        if (warning.code != AssetDiagnosticCode::CurveImportRowSkipped || warning.detail != line) {
            return false;
        }
    }

    return true;
}

} // namespace

// 등록: 다섯 임포터, 이름, 기본 확장자 없음(CAD6), null 상태.
TEST(CurveImport_RegisterSet) {
    CurveEnv env;
    REQUIRE(env.ready);
    AssetImporterTable table(env.allocator, env.types);
    CurveAssetImporterSet set;
    Array<AssetImporterRegistration> tokens(env.allocator);
    REQUIRE(RegisterCurveAssetImporters(&set, table, tokens));
    CHECK(tokens.Size() == 5 && table.Count() == 5);
    CHECK(table.FindByName("curve_table_csv") == &set.tableCsv);
    CHECK(table.FindByName("curve_table_json") == &set.tableJson);
    CHECK(table.FindByName("color_curve_csv") == &set.colorCsv);
    CHECK(table.FindByExtension("csv") == nullptr && table.FindByExtension("json") == nullptr);
    CHECK(RegisterCurveAssetImporters(nullptr, table, tokens).Error() ==
          ErrorCode::InvalidArgument);
    // 부분 실패: 다섯째 이름이 이미 있으면 앞의 넷도 풀리고 tokens는 그대로다.
    {
        AssetImporterTable other(env.allocator, env.types);
        CurveCsvImporter taken(CurveCsvOutput::Color);
        auto first = other.Register(taken);
        REQUIRE(first);
        CurveAssetImporterSet again;
        Array<AssetImporterRegistration> fresh(env.allocator);
        CHECK(RegisterCurveAssetImporters(&again, other, fresh).Error() ==
              ErrorCode::AlreadyExists);
        CHECK(fresh.Size() == 0 && other.Count() == 1);
    }

    // 두 번 등록하면 AlreadyExists이고 tokens는 그대로다.
    CHECK(RegisterCurveAssetImporters(&set, table, tokens).Error() == ErrorCode::AlreadyExists);
    CHECK(tokens.Size() == 5 && table.Count() == 5);
}

// 5.1.2절: CSV 표 — Plain(Linear·Constant)·Tangent(Cubic, 자동 접선), 숫자 칸 trim, 시간 열 0개.
TEST(CurveImport_TableCsvModes) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveTableCsvImporter importer;
    const StringView text = "---,1, 2 ,3\nHealth,10,20,30\nArmor,1,4,9\n";
    {
        Run run(env, importer, text);
        REQUIRE(run.result);
        CHECK(run.context.warnings.Size() == 0);
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.mode == CurveTableMode::Plain && table.RowCount() == 2);
            const PlainCurve* health = table.FindPlainRow(env.Of("Health"));
            REQUIRE(health != nullptr);
            CHECK(health->interp == PlainCurveInterp::Linear && health->KeyCount() == 3);
            CHECK(table.TryEvaluateRow(env.Of("Health"), 1.5f).Value() == 15.0f);
            CHECK(table.TryEvaluateRow(env.Of("Armor"), 2.0f).Value() == 4.0f);
        }));
    }

    {
        CurveTableImportOptions options;
        options.interp = CurveInterp::Constant;
        Run run(env, importer, text, &options);
        REQUIRE(run.result);
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.FindPlainRow(env.Of("Health"))->interp == PlainCurveInterp::Constant);
            CHECK(table.TryEvaluateRow(env.Of("Health"), 1.5f).Value() == 10.0f);
        }));
    }

    {
        CurveTableImportOptions options;
        options.interp = CurveInterp::Cubic;
        Run run(env, importer, text, &options);
        REQUIRE(run.result);
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.mode == CurveTableMode::Tangent);
            const TangentCurve* armor = table.FindTangentRow(env.Of("Armor"));
            REQUIRE(armor != nullptr && armor->KeyCount() == 3);
            // 기준: 같은 키를 손으로 넣고 자동 접선을 계산한 커브와 키가 같다.
            TangentCurve expected(env.allocator);
            for (const float time : {1.0f, 2.0f, 3.0f}) {
                auto key = expected.TryAddKey(time, time * time);
                REQUIRE(key);
                REQUIRE(expected.TrySetKeyInterp(key.Value(), CurveInterp::Cubic));
            }

            expected.AutoSetTangents();
            for (usize index = 0; index < 3; ++index) {
                CHECK(armor->Keys()[index].interp == CurveInterp::Cubic);
                CHECK(armor->Keys()[index].arriveTangent == expected.Keys()[index].arriveTangent);
                CHECK(armor->Keys()[index].leaveTangent == expected.Keys()[index].leaveTangent);
            }
        }));
    }

    {
        // 시간 열 0개: 행은 키 없는 커브다(UE와 같다).
        Run run(env, importer, "---\nA\nB\n");
        REQUIRE(run.result);
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.RowCount() == 2 && table.FindPlainRow(env.Of("A"))->KeyCount() == 0);
        }));
    }

    {
        // 이름 중간의 금지 문자는 빠진다("A B" → "AB" — UE MakeValidName).
        Run run(env, importer, "---,1\n\"A B\",1\n");
        REQUIRE(run.result);
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.FindPlainRow(env.Of("AB")) != nullptr);
        }));
    }
}

// 재임포트: 실패하면 기존 표가 그대로이고, 모드를 바꾸는 성공은 반대쪽 맵을 비운다.
TEST(CurveImport_TableReimport) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveTableCsvImporter importer;
    CurveTableImportOptions cubic;
    cubic.interp = CurveInterp::Cubic;
    auto existing = env.objects.NewObject<CurveTableAsset>(env.objects.OwnerAccess(),
                                                           TypeIdOf<CurveTableAsset>());
    REQUIRE(existing);
    const ObjectHandle handle = existing.Value().Handle();
    {
        Run seed(env, importer, "---,1,2\nOld,5,6\n", &cubic, handle);
        REQUIRE(seed.result);
    }

    {
        Run bad(env, importer, "---,1,2\nNew,1,x\n", nullptr, handle);
        CHECK(bad.result.Error() == ErrorCode::InvalidArgument);
        auto borrowed = env.objects.Resolve<CurveTableAsset>(env.objects.OwnerAccess(), handle);
        REQUIRE(borrowed);
        const CurveTableAsset& table = *borrowed.Value().Get().Value();
        CHECK(table.mode == CurveTableMode::Tangent &&
              table.FindTangentRow(env.Of("Old")) != nullptr);
    }

    Run plain(env, importer, "---,1\nNew,3\n", nullptr, handle);
    REQUIRE(plain.result);
    CHECK(plain.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
        CHECK(table.mode == CurveTableMode::Plain && table.tangentRows.IsEmpty());
        CHECK(table.RowCount() == 1 && table.FindPlainRow(env.Of("New")) != nullptr);
    }));
}

// 할당 실패: 시도마다 OutOfMemory로 실패하거나 성공하고, 실패하면 출력이 없다.
TEST(CurveImport_AllocationFailure) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveTableCsvImporter importer;
    const StringView text = "---,1,2,3\nA,1,2,3\nB,4,5,6\n";
    usize failures = 0;
    bool succeeded = false;
    for (uint64 attempt = 1; attempt <= 256 && !succeeded; ++attempt) {
        env.failing.FailOnAttempt(attempt);
        Run run(env, importer, text);
        env.failing.DisableFailure();
        if (run.result) {
            succeeded = true;
            CHECK(run.context.With<CurveTableAsset>(
                [&](CurveTableAsset& table) noexcept { CHECK(table.RowCount() == 2); }));
        } else {
            ++failures;
            CHECK(run.result.Error() == ErrorCode::OutOfMemory);
            CHECK(run.context.failCode == AssetDiagnosticCode::None);
        }
    }

    CHECK(succeeded && failures > 0);
}

// 5.1.2절: 건너뛰는 행(이름 없음·금지 문자만·None·중복·열 수 불일치)의 경고와 줄 번호, 실패 사유.
TEST(CurveImport_TableCsvSkipsAndFailures) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveTableCsvImporter importer;
    {
        Run run(env, importer,
                "---,1,2\nA,1,2\n,3,4\n\n\" \",5,6\nNone,7,8\nA,9,9\nB,1\nC,1,2,3\nD,2,4\n");
        REQUIRE(run.result);
        // 줄 3(이름 없음), 5(공백뿐 — 빼면 빈 이름), 6(None), 7(중복), 8·9(열 수).
        CHECK(WarnedLines(run, {3, 5, 6, 7, 8, 9}));
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.RowCount() == 2 && table.FindPlainRow(env.Of("D")) != nullptr);
            CHECK(table.TryEvaluateRow(env.Of("A"), 2.0f).Value() == 2.0f);
        }));
    }

    struct Failure {
        StringView text;
        uint64 line;
    };
    const Failure failures[] = {
        {"---,1,2\n", 1},            // 행 부족
        {"", 0},                     // 빈 문서
        {"---,1,x\nA,1,2\n", 1},     // 시간 숫자 아님
        {"---,1,1\nA,1,2\n", 1},     // 시간 중복
        {"---,1,inf\nA,1,2\n", 1},   // 비유한 시간
        {"---,1,2\n\nA,1,two\n", 3}, // 값 숫자 아님(줄 번호는 빈 줄 포함)
        {"---,1,2\nA,1,nan\n", 2},   // 비유한 값
    };
    for (const Failure& failure : failures) {
        Run run(env, importer, failure.text);
        CHECK(run.result.Error() == ErrorCode::InvalidArgument);
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidSource);
        CHECK(run.context.failDetail == failure.line);
        CHECK(run.context.output.IsNull());
    }

    CurveTableImportOptions bad;
    bad.interp = static_cast<CurveInterp>(9);
    Run run(env, importer, "---,1\nA,1\n", &bad);
    CHECK(run.result.Error() == ErrorCode::InvalidArgument);
    CHECK(run.context.failCode == AssetDiagnosticCode::InvalidImportOption);
}

// 5.1.1절: UTF-8 BOM·UTF-16 LE·BE BOM은 같은 결과, 잘못된 UTF-8·홀수 UTF-16은 CorruptData.
TEST(CurveImport_Encodings) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveTableCsvImporter importer;
    const char text[] = "---,1,2\nA,3,4\n";
    const usize length = sizeof(text) - 1;
    uint8 utf8[3 + sizeof(text)] = {0xEF, 0xBB, 0xBF};
    memcpy(utf8 + 3, text, length);
    uint8 little[2 + 2 * sizeof(text)] = {0xFF, 0xFE};
    uint8 big[2 + 2 * sizeof(text)] = {0xFE, 0xFF};
    for (usize index = 0; index < length; ++index) {
        little[2 + 2 * index] = static_cast<uint8>(text[index]);
        big[3 + 2 * index] = static_cast<uint8>(text[index]);
    }

    const Span<const uint8> inputs[] = {Span<const uint8>(utf8, 3 + length),
                                        Span<const uint8>(little, 2 + 2 * length),
                                        Span<const uint8>(big, 2 + 2 * length)};
    for (const Span<const uint8> bytes : inputs) {
        Run run(env, importer, bytes);
        REQUIRE(run.result);
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.TryEvaluateRow(env.Of("A"), 2.0f).Value() == 4.0f);
        }));
    }

    const uint8 badUtf8[] = {'-', ',', '1', '\n', 'A', 0xC3, 0x28, ',', '1', '\n'};
    const uint8 oddUtf16[] = {0xFF, 0xFE, 'A', 0, 'B'};
    const uint8 loneSurrogate[] = {0xFF, 0xFE, 0x00, 0xD8, 'A', 0};
    for (const Span<const uint8> bytes : {Span<const uint8>(badUtf8), Span<const uint8>(oddUtf16),
                                          Span<const uint8>(loneSurrogate)}) {
        Run run(env, importer, bytes);
        CHECK(run.result.Error() == ErrorCode::CorruptData);
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidSource);
    }
}

// 5.1.3절: JSON 표 — 값 종류(숫자·숫자 문자열·bool), 원소·항목 건너뜀(번호), 같은 시간 행 전체
// 건너뜀, 문서 오류.
TEST(CurveImport_TableJson) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveTableJsonImporter importer;
    {
        Run run(env, importer,
                "[{\"Name\":\"A\",\"1\":10,\"2\":\"20\",\"3\":true},"
                " 5,"
                " {\"1\":1},"
                " {\"Name\":\"A\",\"1\":1},"
                " {\"Name\":\"B\",\"x\":1,\"2\":null,\"3\":3},"
                " {\"Name\":\"C\",\"1\":1,\"1.0\":2}]");
        REQUIRE(run.result);
        // 2(객체 아님), 3(Name 없음), 4(중복 이름), 5(항목 둘 — 키 x·값 null), 6(같은 시간 — 행
        // 전체).
        CHECK(WarnedLines(run, {2, 3, 4, 5, 5, 6}));
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.RowCount() == 2 && table.FindRow(env.Of("C")) == nullptr);
            CHECK(table.TryEvaluateRow(env.Of("A"), 2.0f).Value() == 20.0f);
            CHECK(table.TryEvaluateRow(env.Of("A"), 3.0f).Value() == 1.0f);
            const PlainCurve* b = table.FindPlainRow(env.Of("B"));
            REQUIRE(b != nullptr);
            CHECK(b->KeyCount() == 1 && table.TryEvaluateRow(env.Of("B"), 3.0f).Value() == 3.0f);
        }));
    }

    for (const StringView text :
         {StringView("[{"), StringView("{\"Name\":\"A\"}"), StringView("[]"),
          StringView("[{\"Name\":\"A\",\"1\":1,\"1\":2}]")}) {
        Run run(env, importer, text);
        CHECK(run.result.Error() == ErrorCode::InvalidArgument);
        CHECK(run.context.failCode == AssetDiagnosticCode::InvalidSource);
        // 문서 오류의 detail은 0이다(원소 번호가 아니다).
        CHECK(run.context.failDetail == 0);
        CHECK(run.context.output.IsNull());
    }

    {
        // 이름 키는 대소문자를 무시하고 뒤의 것이 이긴다. 숫자·bool 이름, bool 거짓 값, IsNumeric
        // 규칙 (지수·공백 문자열은 건너뜀, "."은 0), 대소문자만 다른 이름은 중복이다.
        Run run(
            env, importer,
            "[{\"name\":\"Hp\",\"1\":5},"
            " {\"NAME\":\"X\",\"Name\":\"Mp\",\"1\":false,\"2\":\".\",\"3\":\"1e2\",\"4\":\" 5\"},"
            " {\"Name\":7,\"1\":\"-2.5\"},"
            " {\"Name\":true,\"1\":1},"
            " {\"Name\":\"hp\",\"1\":9}]");
        REQUIRE(run.result);
        // 2(항목 둘 — "1e2"·" 5"), 5(대소문자만 다른 중복 이름).
        CHECK(WarnedLines(run, {2, 2, 5}));
        CHECK(run.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
            CHECK(table.RowCount() == 4);
            CHECK(table.TryEvaluateRow(env.Of("Hp"), 1.0f).Value() == 5.0f);
            const PlainCurve* mp = table.FindPlainRow(env.Of("Mp"));
            REQUIRE(mp != nullptr);
            CHECK(mp->KeyCount() == 2 && table.FindRow(env.Of("X")) == nullptr);
            CHECK(table.TryEvaluateRow(env.Of("Mp"), 1.0f).Value() == 0.0f);
            CHECK(table.TryEvaluateRow(env.Of("Mp"), 2.0f).Value() == 0.0f);
            CHECK(table.TryEvaluateRow(env.Of("7"), 1.0f).Value() == -2.5f);
            CHECK(table.FindRow(env.Of("true")) != nullptr);
        }));
    }
}

// 5.1.4절: 단일 커브 CSV — 스칼라·벡터·색, 칸 수 경고와 부분 채널, 칸 2개 미만 건너뜀, 키 Linear,
// 빈 문서 실패, 재임포트가 기본값·외삽·색 조정·eventCurve를 지킨다.
TEST(CurveImport_SingleCurveCsv) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveCsvImporter floats(CurveCsvOutput::Float);
    CurveCsvImporter vectors(CurveCsvOutput::Vector);
    CurveCsvImporter colors(CurveCsvOutput::Color);
    {
        Run run(env, floats, "0,1\n1,3\n2,5,9\n");
        REQUIRE(run.result);
        CHECK(WarnedLines(run, {3}));
        CHECK(run.context.With<FloatCurveAsset>([](FloatCurveAsset& asset) noexcept {
            CHECK(asset.curve.KeyCount() == 3 && asset.Evaluate(0.5f) == 2.0f);
            CHECK(asset.curve.Keys()[0].interp == CurveInterp::Linear);
        }));
    }

    {
        Run run(env, vectors, "0,1,2,3\n1,4\n5\n2,7,8,9\n");
        REQUIRE(run.result);
        // 줄 2(칸 부족 — x만), 줄 3(칸 2개 미만 — 건너뜀).
        CHECK(WarnedLines(run, {2, 3}));
        CHECK(run.context.With<VectorCurveAsset>([](VectorCurveAsset& asset) noexcept {
            CHECK(asset.x.KeyCount() == 3 && asset.y.KeyCount() == 2 && asset.z.KeyCount() == 2);
        }));
    }

    {
        Run run(env, colors, "");
        CHECK(run.result.Error() == ErrorCode::InvalidArgument);
        Run bad(env, colors, "0,1,x\n");
        CHECK(bad.result.Error() == ErrorCode::InvalidArgument && bad.context.failDetail == 1);
    }

    // 재임포트: 임포트가 정하지 않는 필드는 그대로다.
    auto existing = env.objects.NewObject<ColorCurveAsset>(env.objects.OwnerAccess(),
                                                           TypeIdOf<ColorCurveAsset>());
    REQUIRE(existing);
    {
        auto writable = env.objects.ResolveForWrite<ColorCurveAsset>(env.objects.OwnerAccess(),
                                                                     existing.Value().Handle());
        REQUIRE(writable);
        ColorCurveAsset& asset = *writable.Value().Get().Value();
        asset.adjustHue = 45.0f;
        asset.r.defaultValue = 0.25f;
        asset.r.postExtrapolation = CurveExtrapolation::Linear;
        REQUIRE(asset.r.TryAddKey(9.0f, 9.0f));
    }

    Run reimport(env, colors, "0,1,0,0,1\n1,0,1,0,1\n", nullptr, existing.Value().Handle());
    REQUIRE(reimport.result);
    CHECK(reimport.context.With<ColorCurveAsset>([](ColorCurveAsset& asset) noexcept {
        CHECK(asset.adjustHue == 45.0f && asset.r.defaultValue.Value() == 0.25f);
        CHECK(asset.r.postExtrapolation == CurveExtrapolation::Linear);
        CHECK(asset.r.KeyCount() == 2 && asset.r.Keys()[0].time == 0.0f && asset.a.KeyCount() == 2);
    }));
}

// 결정성: 같은 원본은 같은 결과다.
TEST(CurveImport_Deterministic) {
    CurveEnv env;
    REQUIRE(env.ready);
    CurveTableCsvImporter importer;
    CurveTableImportOptions options;
    options.interp = CurveInterp::Cubic;
    const StringView text = "---,0,1,2,4\nA,0,1,0,3\nB,5,5,6,7\n";
    Run first(env, importer, text, &options);
    Run second(env, importer, text, &options);
    REQUIRE(first.result && second.result);
    TangentCurve copy(env.allocator);
    CHECK(first.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
        REQUIRE(copy.TryCopyFrom(*table.FindTangentRow(env.Of("A"))));
    }));
    CHECK(second.context.With<CurveTableAsset>([&](CurveTableAsset& table) noexcept {
        const TangentCurve* again = table.FindTangentRow(env.Of("A"));
        REQUIRE(again != nullptr && again->KeyCount() == copy.KeyCount());
        // 필드별로 비교한다(키 struct의 padding byte는 비교하지 않는다).
        for (usize index = 0; index < copy.KeyCount(); ++index) {
            const TangentCurveKey& left = again->Keys()[index];
            const TangentCurveKey& right = copy.Keys()[index];
            CHECK(left.time == right.time && left.value == right.value &&
                  left.interp == right.interp && left.arriveTangent == right.arriveTangent &&
                  left.leaveTangent == right.leaveTangent);
        }
    }));
}

} // namespace curve_import_tests_file
