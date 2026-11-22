#pragma once

#include <Core/Text/StringView.h>
#include <Platform/File.h>
#include <Platform/Sync.h>

// AssetNugget 시험 공용 도구다(EngineTests 전용 — 제품 코드가 쓰지 않는다).
namespace rocks::nugget_test {

// 물리 파일 시스템에 넘기되 한 번 hook을 거는 래퍼다: 경로가 suffix로 끝나고 열기 방식이 mode인 첫
// Open 직전에 callback을 부른다(캐시 잠금 밖 backend IO 사이 — 경합 창을 결정적으로 만든다).
// callback 안에서 같은 캐시를 다시 불러도 된다(hook은 부르기 전에 풀린다). 여러 worker가 동시에
// 열어도 callback은 한 번만 불린다(꺼내기는 잠금 안).
class HookFileSystem final : public platform::FileSystem {
public:
    void Arm(StringView pathSuffix, platform::FileOpenMode openMode, void (*function)(void*),
             void* functionContext) noexcept {
        platform::LockGuard lock(gate);
        suffix = pathSuffix;
        mode = openMode;
        callback = function;
        context = functionContext;
    }

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode openMode,
                              platform::FileHandle*& out) noexcept override {
        void (*function)(void*) = nullptr;
        void* functionContext = nullptr;
        {
            platform::LockGuard lock(gate);
            if (callback != nullptr && openMode == mode &&
                StringView(path, pathBytes).EndsWith(suffix, StringComparison::CaseSensitive)) {
                function = callback;
                functionContext = context;
                callback = nullptr;
            }
        }

        if (function != nullptr) {
            function(functionContext);
        }

        return inner->Open(path, pathBytes, openMode, out);
    }

    platform::FileStatus FileExists(const char* path, usize pathBytes,
                                    bool& exists) noexcept override {
        return inner->FileExists(path, pathBytes, exists);
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        return inner->Delete(path, pathBytes);
    }

    platform::FileStatus Stat(const char* path, usize pathBytes,
                              platform::FileInfo& info) noexcept override {
        return inner->Stat(path, pathBytes, info);
    }

    platform::FileStatus MakeDirectory(const char* path, usize pathBytes) noexcept override {
        return inner->MakeDirectory(path, pathBytes);
    }

    platform::FileStatus DeleteDirectory(const char* path, usize pathBytes) noexcept override {
        return inner->DeleteDirectory(path, pathBytes);
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize pathBytes,
                                            platform::DirectoryVisitor visitor,
                                            void* visitorContext) noexcept override {
        return inner->EnumerateDirectory(path, pathBytes, visitor, visitorContext);
    }

    platform::FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner->Move(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Copy(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner->Copy(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Replace(const char* from, usize fromBytes, const char* to,
                                 usize toBytes) noexcept override {
        return inner->Replace(from, fromBytes, to, toBytes);
    }

    platform::FileStatus SetModifiedTime(const char* path, usize pathBytes,
                                         int64 modifiedTicks) noexcept override {
        return inner->SetModifiedTime(path, pathBytes, modifiedTicks);
    }

    platform::FileStatus SyncDirectory(const char* path, usize pathBytes) noexcept override {
        return inner->SyncDirectory(path, pathBytes);
    }

    platform::Mutex gate;
    platform::FileSystem* inner = &platform::PhysicalFileSystem();
    StringView suffix{};
    platform::FileOpenMode mode = platform::FileOpenMode::ReadExisting;
    void (*callback)(void*) = nullptr;
    void* context = nullptr;
};

// hook 안에서 항목 작업을 붙잡는 gate다: callback이 entered를 알리고 release를 기다린다(순서는
// 이벤트로만 — 잠·시간 경합 없음).
struct NuggetGate {
    static void Hold(void* context) noexcept {
        auto* gate = static_cast<NuggetGate*>(context);
        gate->entered.Signal();
        (void)gate->release.Wait();
    }

    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
};

} // namespace rocks::nugget_test
