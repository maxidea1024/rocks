#pragma once

#include <Base/InitializerList.h>
#include <Core/Memory/Allocator.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <stdio.h>
#include <string.h>

// 명령·쿠킹 단계가 함께 쓰는 관리자 준비다(모듈 안 전용): 옵션 기록 리플렉션 문맥(키 6행)과
// 실행 파일 옆 워커 찾기.
namespace rocks::shader::compile_support {

// 실행 파일 옆의 ShaderCompileWorker를 out(NUL 종료)에 둔다. 없으면 false다.
template <usize Bytes>
bool FindWorkerNextToExecutable(char (&out)[Bytes]) noexcept {
    usize bytes = 0;
    if (!platform::GetExecutablePath(out, sizeof(out), bytes)) {
        return false;
    }

    while (bytes > 0 && out[bytes - 1] != '/' && out[bytes - 1] != '\\') {
        --bytes;
    }

    for (const char* name : {"ShaderCompileWorker.exe", "ShaderCompileWorker"}) {
        const int written = snprintf(out + bytes, sizeof(out) - bytes, "%s", name);
        bool exists = false;
        if (written > 0 && platform::PhysicalFileSystem().FileExists(out, strlen(out), exists) &&
            exists) {
            return true;
        }
    }

    return false;
}

// 옵션 기록 리플렉션 문맥이다(RegisterShaderCompilerTypes를 등록한 TypeRegistry). 만든 스레드에서
// 파괴한다.
struct KeyReflection {
    KeyReflection(Allocator& allocator, uint32 contextId) noexcept
        : types(allocator), registration(types, allocator),
          objects(allocator, types, {contextId, 1}), access(objects.OwnerAccess()),
          allocator_(allocator) {
        registered = static_cast<bool>(RegisterShaderCompilerTypes(registration));
    }

    ~KeyReflection() noexcept {
        (void)objects.Shutdown(objects.OwnerAccess());
        (void)registration.Unregister();
        (void)types.Shutdown();
    }

    KeyReflection(const KeyReflection&) = delete;
    KeyReflection& operator=(const KeyReflection&) = delete;

    ShaderKeyReflection Reflection() noexcept {
        return ShaderKeyReflection{&objects, access, &types, &allocator_};
    }

    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    bool registered = false;

private:
    Allocator& allocator_;
};

} // namespace rocks::shader::compile_support
