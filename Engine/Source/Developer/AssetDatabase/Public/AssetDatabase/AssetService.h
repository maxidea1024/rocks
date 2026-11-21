#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/ProjectLayout.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextDiagnostics.h>

// 에셋 서비스 API다(Docs/Design/AssetSystem.ko.md 13절·7.1절 15 A2c3). 명령행 asset-new·asset-set·
// asset-info·asset-list와 1:1 함수이고 에디터가 나중에 같은 함수를 쓴다. 프로젝트 하나의 색인과
// 에디터 패키지 집합을 소유한다(명령 하나가 서비스 하나를 만들고 끝나면 버린다).
//
// 사용 예(asset-set):
//   AssetServiceConfig config;
//   config.packages.types = &types;
//   config.packages.files = &platform::PhysicalFileSystem();
//   config.packages.hooks = &hooks;
//   config.packages.registryOptions.names = &names;
//   AssetService service(allocator, layout, config);
//   if (auto ready = service.Initialize(); !ready) { ... }
//   AssetDatabaseLock lock;
//   if (auto held = lock.Acquire(layout, waitMilliseconds); !held) { ... }
//   reflection_text::TextDiagnostics diagnostics(allocator);
//   Array<EditorWarning> warnings(allocator);
//   AssetDiagnostic failure;
//   auto set = service.Set(lock, "/Game/Hero", assignments, false, diagnostics, &warnings,
//                          &failure);
namespace rocks {

struct AssetServiceConfig {
    // 에디터 패키지 집합 설정이다. layout·index는 서비스가 채운다(넘긴 값은 쓰지 않는다).
    // registryOptions.names는 Name 필드 대입·출력의 pool이다(없으면 Name 값 대입은 진단
    // Unsupported). read는 색인 Rebuild에도 쓴다.
    EditorPackageSetConfig packages;
};

// asset-info 출력 선택이다(13절).
struct AssetInfoOptions {
    // JSON 출력(ExportJson)이다(기본은 `<경로>=<값>` 줄 — ExportLines).
    bool json = false;
    // 보고용 전체 출력(Report 용도 — Transient만 뺀다)이다(기본은 Edit 용도 — asset-set에 그대로
    // 넣을 수 있다).
    bool all = false;
};

class AssetService {
public:
    // layout은 초기화한 프로젝트 배치다(빌림 — 서비스보다 오래 산다).
    AssetService(Allocator& allocator, const ProjectLayout& layout,
                 const AssetServiceConfig& config) noexcept;
    AssetService(const AssetService&) = delete;
    AssetService& operator=(const AssetService&) = delete;

    // Content를 훑어 색인을 만들고 에디터 패키지 집합을 연다. 다시 부르면 InvalidArgument다.
    // 오류: 설정 오류(타입·파일 시스템 없음 등) InvalidArgument, 색인 Rebuild의 오류(디렉터리
    // 열거 실패), 집합 Initialize의 오류, 할당 실패.
    Result<void> Initialize() noexcept;

    // 색인과 집합이다(Initialize 뒤).
    AssetIndex& Index() noexcept {
        return index_;
    }

    EditorPackageSet& Packages() noexcept {
        return set_;
    }

    // asset-new <Type> <path> [<속성>=<값>…]: path에 type(타입 도구 이름 — 에셋 클래스)의 새
    // 패키지를 만들고, 대입을 공개 전 대표 에셋에 모두 검증·게시한 뒤 함께 공개해 저장한다. 반환은
    // 새 AssetId다. lock은 이 프로젝트의 잡은 쓰기 잠금이다. 새 패키지의 객체는 공개 전이라 Weak
    // 참조와 새 패키지 자신을 가리키는 참조는 풀 수 없다(진단 Unresolved — 만든 뒤 Set으로 넣는다).
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, path 규칙 위반 InvalidPath /
    // InvalidArgument, 모르는 타입 이름 NotFound(진단 None), 에셋 클래스가 아니거나 추상 클래스
    // TypeMismatch / InvalidArgument, EditorPackageSet::Create의 오류(같은 경로 DuplicatePath 등),
    // 대입 오류 InvalidArgument(진단 None — diagnostics에 모두 모인다, 아무것도 만들지 않는다),
    // EditorPackageSet::Save의 오류(만든 패키지는 저장하지 않은 채 집합에 남는다), 할당 실패.
    Result<AssetId> New(const AssetDatabaseLock& lock, StringView type, StringView path,
                        Span<const reflection_text::TextAssignment> assignments,
                        reflection_text::TextDiagnostics& diagnostics,
                        Array<EditorWarning>* warnings = nullptr,
                        AssetDiagnostic* failure = nullptr) noexcept;

    // asset-set <path|AssetId> <속성>=<값>…: 대상을 열고(Hard·EditorOnly 닫힘째) 대입을 모두
    // 검증한 뒤 한꺼번에 게시하고 저장한다. Weak 참조는 대상 패키지 안의 객체만 받는다(5.1절 —
    // 밖은 진단 Unresolved). force는 미해결 import를 null로 저장한다(--force —
    // 경고 DroppedUnresolvedImport). 빈 대입 목록도 저장한다.
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 대상 텍스트 형식(논리 경로 규칙·36자
    // 소문자 AssetId) InvalidPath / InvalidArgument, 열기의 오류(EditorPackageSet::Open), 대입 오류
    // InvalidArgument(진단 None — diagnostics에 모두 모인다, 아무것도 바꾸지 않는다), 게시의 오류,
    // EditorPackageSet::Save의 오류(바뀐 값은 집합에 남는다), 할당 실패.
    Result<void> Set(const AssetDatabaseLock& lock, StringView target,
                     Span<const reflection_text::TextAssignment> assignments, bool force,
                     reflection_text::TextDiagnostics& diagnostics,
                     Array<EditorWarning>* warnings = nullptr,
                     AssetDiagnostic* failure = nullptr) noexcept;

    // asset-info <path|AssetId>: 대상을 열어 대표 에셋의 값을 out 뒤에 쓴다(options — 줄·JSON,
    // Edit·Report). 참조는 논리 경로로 쓴다. 잠금을 잡지 않는다.
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 대상 텍스트 형식 InvalidPath /
    // InvalidArgument, 열기의 오류, 출력의 오류(진단 None — 줄로 쓸 수 없는 값은 Unsupported,
    // 그때는 json을 쓴다), 할당 실패.
    Result<void> Info(StringView target, const AssetInfoOptions& options, String& out,
                      Array<EditorWarning>* warnings = nullptr,
                      AssetDiagnostic* failure = nullptr) noexcept;

    // asset-list [--type=<Type>] [--under=<path>]: 색인 항목을 경로 키 순으로 out에 붙인다. type이
    // 있으면 그 클래스와 파생의 대표 에셋만(등록되지 않은 타입의 항목은 맞지 않는다), under가
    // 있으면 그 논리 디렉터리 아래만(없으면 /Game 전체)이다. 항목 포인터는 색인이 바뀌기 전까지
    // 유효하다.
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 모르는 타입 이름 NotFound(진단 None),
    // 클래스가 아닌 타입 TypeMismatch / InvalidArgument, under가 논리 디렉터리가 아님 InvalidPath /
    // InvalidArgument, 파생 판정의 그 밖 오류(TypeRegistry 종료 뒤 ShuttingDown 등 — 진단 None),
    // 할당 실패. 실패하면 out은 호출 전과 같다.
    Result<void> List(StringView type, StringView under, Array<const AssetIndexEntry*>& out,
                      AssetDiagnostic* failure = nullptr) noexcept;

private:
    // `<path|AssetId>`의 패키지를 열고(열린 패키지면 그대로) 대표 에셋을 준다. id에 패키지 id를
    // 둔다.
    Result<ObjectHandle> OpenTarget(StringView target, AssetId& id, Array<EditorWarning>* warnings,
                                    AssetDiagnostic* failure) noexcept;
    // 타입 도구 이름의 클래스 lease다(에셋 클래스만이면 requireAsset).
    Result<TypeLease> FindClass(StringView name, bool requireAsset,
                                AssetDiagnostic* failure) noexcept;

    Allocator* allocator_;
    AssetIndex index_;
    EditorPackageSet set_;
    PackageReadOptions read_;
    TypeRegistry* types_;
    platform::FileSystem* files_;
    bool initialized_ = false;
};

} // namespace rocks
