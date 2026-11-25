#include <Base/PlacementNew.h>
#include <Core/Diagnostics/Log.h>
#include <Launch/AndroidActivity.h>
#include <Launch/Application.h>
#include <Platform/ApkFile.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Path.h>
#include <Platform/ProcessEntry.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

#include <android/native_activity.h>
#include <jni.h>
#include <string.h>

// Android 액티비티 층이다(Docs/Design/AndroidPlatform.ko.md 3절). 플랫폼 NativeActivity가
// lib<Program>.so를 싣고 ANativeActivity_onCreate를 UI 스레드에서 부른다. android_native_app_glue는
// 쓰지 않는다(AD10). 콜백은 UI 스레드에서 직접 받아 창 listener에 넘기고, 엔진 본문(Launch 실행 —
// 단계·본문)은 게임 스레드에서 돈다. 앱 기술은 실행 파일 모듈의 EntryPoint.h 매크로가 내보내는
// RocksAndroidApplicationDescription이다(APK가 아닌 콘솔 실행 파일에는 없다 — 약한 참조).

extern "C" __attribute__((weak)) const rocks::launch::ApplicationDescription*
RocksAndroidApplicationDescription() noexcept;

namespace rocks::launch::android {

namespace {

using platform::LockGuard;
using platform::Mutex;
using platform::Thread;
using platform::ThreadStatus;

inline constexpr LogCategory LogAndroidActivity{"Launch.Android"};

// 게임 스레드 스택이다. 데스크톱 주 스레드(8 MB)와 같다. UI 스레드나 pthread 기본(약 1 MB)으로는
// 본문을 감당하지 못한다.
constexpr usize GameThreadStackBytes = usize{8} << 20;
constexpr char GameThreadName[] = "GameThread";

// 명령줄 인자 저장소다(실행 파일 이름 포함). 넘치면 뒤 인자를 버리고 Warning을 남긴다.
constexpr usize MaxArguments = 64;
constexpr usize ArgumentBytes = 16 * 1024;

// 앱 저장소 경로 버퍼다.
constexpr usize PathBytes = 4096;

// Intent extra 이름이다(debuggable 빌드에서만 읽는다 — AD7).
constexpr char ArgumentsExtra[] = "rocks.args";

// ApplicationInfo.FLAG_DEBUGGABLE이다.
constexpr jint FlagDebuggable = 2;

// 액티비티 상태다. 프로세스에 하나다.
struct ActivityState {
    ANativeActivity* activity = nullptr;
    const ApplicationDescription* description = nullptr;
    // listener·window·inputQueue·focused를 지킨다(UI 스레드 콜백과 게임 스레드
    // SetActivityListener).
    Mutex mutex;
    ActivityListener* listener = nullptr;
    ANativeWindow* window = nullptr;
    AInputQueue* inputQueue = nullptr;
    bool focused = false;
    // 게임 스레드와 그 결과다.
    Thread gameThread;
    int exitCode = 0;
    // 명령줄이다. argv[0]은 프로그램 이름이다.
    char argumentStorage[ArgumentBytes]{};
    usize argumentBytes = 0;
    char* argv[MaxArguments + 1]{};
    int argc = 0;
};

alignas(ActivityState) unsigned char stateStorage[sizeof(ActivityState)];
ActivityState* state = nullptr;

// 인자 하나를 저장소에 복사해 argv 끝에 더한다. 자리가 없으면 false다.
bool AddArgument(const char* text, usize bytes) noexcept {
    if (state->argc >= static_cast<int>(MaxArguments) ||
        state->argumentBytes + bytes + 1 > ArgumentBytes) {
        return false;
    }

    char* target = state->argumentStorage + state->argumentBytes;
    memcpy(target, text, bytes);
    target[bytes] = '\0';
    state->argumentBytes += bytes + 1;
    state->argv[state->argc++] = target;
    return true;
}

// JNI 호출 뒤 Java 예외가 있으면 지우고 false다.
bool ClearException(JNIEnv* env) noexcept {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }

    return true;
}

// Java String을 UTF-8로 buffer에 복사한다(NUL 포함). 실패나 넘침은 false다.
bool CopyString(JNIEnv* env, jstring text, char* buffer, usize capacity) noexcept {
    if (text == nullptr) {
        return false;
    }

    const char* chars = env->GetStringUTFChars(text, nullptr);
    if (chars == nullptr) {
        (void)ClearException(env);
        return false;
    }

    const usize bytes = strlen(chars);
    const bool fits = bytes + 1 <= capacity;
    if (fits) {
        memcpy(buffer, chars, bytes + 1);
    }

    env->ReleaseStringUTFChars(text, chars);
    return fits;
}

// getCacheDir().getAbsolutePath()다.
bool ReadCacheDirectory(JNIEnv* env, jobject activity, char* buffer, usize capacity) noexcept {
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getCacheDir = env->GetMethodID(activityClass, "getCacheDir", "()Ljava/io/File;");
    if (!ClearException(env) || getCacheDir == nullptr) {
        return false;
    }

    jobject file = env->CallObjectMethod(activity, getCacheDir);
    if (!ClearException(env) || file == nullptr) {
        return false;
    }

    jclass fileClass = env->GetObjectClass(file);
    jmethodID getAbsolutePath =
        env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    if (!ClearException(env) || getAbsolutePath == nullptr) {
        return false;
    }

    auto path = static_cast<jstring>(env->CallObjectMethod(file, getAbsolutePath));
    return ClearException(env) && CopyString(env, path, buffer, capacity);
}

// getApplicationInfo().flags에 FLAG_DEBUGGABLE이 있는지다. 읽지 못하면 false다(Release처럼 다룬다).
bool IsDebuggable(JNIEnv* env, jobject activity) noexcept {
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getApplicationInfo = env->GetMethodID(activityClass, "getApplicationInfo",
                                                    "()Landroid/content/pm/ApplicationInfo;");
    if (!ClearException(env) || getApplicationInfo == nullptr) {
        return false;
    }

    jobject info = env->CallObjectMethod(activity, getApplicationInfo);
    if (!ClearException(env) || info == nullptr) {
        return false;
    }

    jfieldID flags = env->GetFieldID(env->GetObjectClass(info), "flags", "I");
    if (!ClearException(env) || flags == nullptr) {
        return false;
    }

    return (env->GetIntField(info, flags) & FlagDebuggable) != 0;
}

// getIntent().getStringArrayExtra("rocks.args")를 argv에 더한다. extra가 없으면 false다.
bool ReadIntentArguments(JNIEnv* env, jobject activity) noexcept {
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getIntent =
        env->GetMethodID(activityClass, "getIntent", "()Landroid/content/Intent;");
    if (!ClearException(env) || getIntent == nullptr) {
        return false;
    }

    jobject intent = env->CallObjectMethod(activity, getIntent);
    if (!ClearException(env) || intent == nullptr) {
        return false;
    }

    jmethodID getStringArrayExtra =
        env->GetMethodID(env->GetObjectClass(intent), "getStringArrayExtra",
                         "(Ljava/lang/String;)[Ljava/lang/String;");
    if (!ClearException(env) || getStringArrayExtra == nullptr) {
        return false;
    }

    jstring name = env->NewStringUTF(ArgumentsExtra);
    auto values =
        static_cast<jobjectArray>(env->CallObjectMethod(intent, getStringArrayExtra, name));
    if (!ClearException(env) || values == nullptr) {
        return false;
    }

    const jsize count = env->GetArrayLength(values);
    char argument[PathBytes];
    for (jsize index = 0; index < count; ++index) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
        if (!CopyString(env, value, argument, sizeof(argument)) ||
            !AddArgument(argument, strlen(argument))) {
            Log::Warning(LogAndroidActivity,
                         "Intent argument {Index} was dropped (too long or too many)",
                         static_cast<int32>(index));
        }
    }

    return true;
}

// 게임 스레드다. Launch 실행(단계·본문)을 돌고, 끝나면 액티비티를 끝내 달라고 한다(onDestroy가
// 이어진다).
void RunGameThread(void*) noexcept {
    state->exitCode = RunApplication(*state->description, state->argc, state->argv);
    Log::Info(LogAndroidActivity, "Application finished with exit code {Code}", state->exitCode);
    ANativeActivity_finish(state->activity);
}

// --- 액티비티 콜백(UI 스레드) ---

void OnDestroy(ANativeActivity*) noexcept {
    // 본문이 아직 돌면 종료 요청 다리로 끝내고 기다린다. 같은 프로세스에서 onCreate가 다시 불릴 수
    // 있는데 정적 상태는 재진입을 전제로 하지 않으므로 프로세스를 끝낸다(AD8).
    launch::RequestTermination();
    if (state->gameThread.IsJoinable() && state->gameThread.Join() != ThreadStatus::Success) {
        platform::Fatal("Rocks Android: the game thread could not be joined");
    }

    platform::ExitProcessWithoutTeardown(state->exitCode);
}

void OnNativeWindowCreated(ANativeActivity*, ANativeWindow* window) noexcept {
    LockGuard guard(state->mutex);
    state->window = window;
    if (state->listener != nullptr) {
        state->listener->OnWindowCreated(window);
    }
}

void OnNativeWindowDestroyed(ANativeActivity*, ANativeWindow* window) noexcept {
    LockGuard guard(state->mutex);
    if (state->listener != nullptr) {
        state->listener->OnWindowDestroyed(window);
    }

    state->window = nullptr;
}

void OnNativeWindowResized(ANativeActivity*, ANativeWindow* window) noexcept {
    LockGuard guard(state->mutex);
    if (state->listener != nullptr) {
        state->listener->OnWindowResized(window);
    }
}

void OnWindowFocusChanged(ANativeActivity*, int hasFocus) noexcept {
    LockGuard guard(state->mutex);
    state->focused = hasFocus != 0;
    if (state->listener != nullptr) {
        state->listener->OnFocusChanged(state->focused);
    }
}

void OnInputQueueCreated(ANativeActivity*, AInputQueue* queue) noexcept {
    LockGuard guard(state->mutex);
    state->inputQueue = queue;
    if (state->listener != nullptr) {
        state->listener->OnInputQueueCreated(queue);
    }
}

void OnInputQueueDestroyed(ANativeActivity*, AInputQueue* queue) noexcept {
    LockGuard guard(state->mutex);
    if (state->listener != nullptr) {
        state->listener->OnInputQueueDestroyed(queue);
    }

    state->inputQueue = nullptr;
}

// 수명 콜백이다. 앱 수명 큐(PlatformLayer)는 오디오 AU9에서 연결하고, 지금은 기록만 한다.
void OnStart(ANativeActivity*) noexcept {
    Log::Info(LogAndroidActivity, "Activity started");
}

void OnResume(ANativeActivity*) noexcept {
    Log::Info(LogAndroidActivity, "Activity resumed");
}

void OnPause(ANativeActivity*) noexcept {
    Log::Info(LogAndroidActivity, "Activity paused");
}

void OnStop(ANativeActivity*) noexcept {
    Log::Info(LogAndroidActivity, "Activity stopped");
}

void OnLowMemory(ANativeActivity*) noexcept {
    Log::Warning(LogAndroidActivity, "The system reported low memory");
}

void OnConfigurationChanged(ANativeActivity*) noexcept {
    Log::Info(LogAndroidActivity, "Activity configuration changed");
}

} // namespace

void SetActivityListener(ActivityListener* listener) noexcept {
    if (state == nullptr) {
        return;
    }

    LockGuard guard(state->mutex);
    state->listener = listener;
    if (listener == nullptr) {
        return;
    }

    if (state->window != nullptr) {
        listener->OnWindowCreated(state->window);
    }

    listener->OnFocusChanged(state->focused);
    if (state->inputQueue != nullptr) {
        listener->OnInputQueueCreated(state->inputQueue);
    }
}

ANativeActivity* CurrentActivity() noexcept {
    return state != nullptr ? state->activity : nullptr;
}

} // namespace rocks::launch::android

// 플랫폼 NativeActivity가 lib<Program>.so를 싣고 UI 스레드에서 부르는 진입이다. 콜백을 걸고, 앱
// 저장소 경로·명령줄을 JNI로 읽은 뒤 게임 스레드를 띄운다.
extern "C" ROCKS_DLLEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void*, size_t) {
    using namespace rocks;
    using namespace rocks::launch::android;
    if (state != nullptr) {
        // 같은 프로세스에서 다시 만들어졌다. onDestroy가 프로세스를 끝내므로 오지 않아야 한다(AD8).
        platform::Fatal("Rocks Android: the activity was created twice in one process");
    }

    if (&RocksAndroidApplicationDescription == nullptr) {
        platform::Fatal(
            "Rocks Android: this library has no application description (build it with an "
            "AndroidPackage rule)");
    }

    state = ::new (stateStorage) ActivityState();
    state->activity = activity;
    state->description = RocksAndroidApplicationDescription();
    ANativeActivityCallbacks& callbacks = *activity->callbacks;
    callbacks.onStart = &OnStart;
    callbacks.onResume = &OnResume;
    callbacks.onPause = &OnPause;
    callbacks.onStop = &OnStop;
    callbacks.onDestroy = &OnDestroy;
    callbacks.onWindowFocusChanged = &OnWindowFocusChanged;
    callbacks.onNativeWindowCreated = &OnNativeWindowCreated;
    callbacks.onNativeWindowResized = &OnNativeWindowResized;
    callbacks.onNativeWindowDestroyed = &OnNativeWindowDestroyed;
    callbacks.onInputQueueCreated = &OnInputQueueCreated;
    callbacks.onInputQueueDestroyed = &OnInputQueueDestroyed;
    callbacks.onConfigurationChanged = &OnConfigurationChanged;
    callbacks.onLowMemory = &OnLowMemory;

    // APK 안 파일(assets/)을 읽는 관리자를 등록한다(Platform/ApkFile.h — AndroidPlatform 7절).
    (void)platform::RegisterApkAssetManager(activity->assetManager);

    // 앱 저장소 경로(PlatformLayer RegisterApplicationDirectories)다.
    JNIEnv* env = activity->env;
    char cache[PathBytes];
    if (activity->internalDataPath != nullptr &&
        ReadCacheDirectory(env, activity->clazz, cache, sizeof(cache))) {
        (void)platform::RegisterApplicationDirectories(activity->internalDataPath, cache);
    }

    // 명령줄: 프로그램 이름, 그다음 debuggable이면 Intent extra, 없으면 앱 기술의 기본 인자다.
    const char* name = state->description->name != nullptr ? state->description->name : "Rocks";
    (void)AddArgument(name, strlen(name));
    const bool fromIntent =
        IsDebuggable(env, activity->clazz) && ReadIntentArguments(env, activity->clazz);
    if (!fromIntent) {
        for (const StringView argument : state->description->androidDefaultArguments) {
            (void)AddArgument(argument.Data(), argument.ByteCount());
        }
    }

    platform::ThreadOptions options;
    options.name = GameThreadName;
    options.stackBytes = GameThreadStackBytes;
    if (state->gameThread.Start(&RunGameThread, nullptr, options) !=
        platform::ThreadStatus::Success) {
        platform::Fatal("Rocks Android: the game thread could not be started");
    }
}
