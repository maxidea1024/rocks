#include <Platform/Path.h>
#include <Platform/Win64/ComRef.h>
#include <ShaderToolLoader/ShaderToolLoader.h>
#include <string.h>

// dxcapi.h는 Windows에서 COM 형식(IUnknown·HRESULT·REFCLSID)을 쓰므로 Windows 헤더 뒤에
// include한다. 이 번역 단위가 엔진 경고 규칙으로 경고 0인지가 P3에서 넘어온 확인 항목이다
// (Docs/Design/ShaderSystem.ko.md 8절 진척). 빌드가 WIN32_LEAN_AND_MEAN을 정의하므로 COM 기본
// 형식(IUnknown)과 IStream 헤더를 직접 더한다.
#include <Windows.h>
#include <Unknwn.h>
#include <objidl.h>
#include <dxc/dxcapi.h>

namespace rocks::shader {

namespace {

// 저장소 루트의 하위 경로다(Rocks.Tooling.Common ThirdPartyStore.DefaultRoot와 같다).
constexpr char StoreSuffix[] = "\\Rocks\\ThirdParty";

} // namespace

// Windows 저장소는 %LOCALAPPDATA%(FOLDERID_LocalAppData) 아래다. PlatformBase의 UserCache가 이
// 폴더다(이름과 달리 OS가 지우는 캐시 폴더가 아니며 roaming도 아니다). .NET의
// Environment.SpecialFolder.LocalApplicationData와 같은 위치다.
platform::FileStatus GetShaderToolStoreRoot(char* buffer, usize capacity, usize& bytes) noexcept {
    bytes = 0;
    usize base = 0;
    if (const platform::FileStatus found = platform::GetStandardDirectory(
            platform::StandardDirectory::UserCache, buffer, capacity, base);
        !found) {
        if (found.code == platform::FileCode::Overflow) {
            bytes = base + sizeof(StoreSuffix) - 1;
        }

        return found;
    }

    const usize total = base + sizeof(StoreSuffix) - 1;
    if (total + 1 > capacity) {
        bytes = total;
        return {platform::FileCode::Overflow, 0};
    }

    memcpy(buffer + base, StoreSuffix, sizeof(StoreSuffix));
    bytes = total;
    return {};
}

ShaderToolLoadStatus QueryDxcVersion(const DxcFunctions& functions, uint32& major,
                                     uint32& minor) noexcept {
    major = 0;
    minor = 0;
    ShaderToolLoadStatus status;
    status.tool = DxcToolName;
    void* address = functions.address[DxcFunctions::CreateInstance];
    if (address == nullptr) {
        status.code = ShaderToolLoadCode::WrongState;
        return status;
    }

    const auto create = reinterpret_cast<DxcCreateInstanceProc>(address);
    platform::windows::ComRef<IDxcVersionInfo> info;
    const HRESULT created = create(CLSID_DxcCompiler, IID_PPV_ARGS(info.Out()));
    if (FAILED(created) || !info) {
        status.code = ShaderToolLoadCode::CallFailed;
        status.file.nativeError = static_cast<uint32>(created);
        return status;
    }

    UINT32 versionMajor = 0;
    UINT32 versionMinor = 0;
    const HRESULT read = info->GetVersion(&versionMajor, &versionMinor);
    if (FAILED(read)) {
        status.code = ShaderToolLoadCode::CallFailed;
        status.file.nativeError = static_cast<uint32>(read);
        return status;
    }

    major = versionMajor;
    minor = versionMinor;
    return status;
}

} // namespace rocks::shader
