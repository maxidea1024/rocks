namespace Rocks.Build;

/// <summary>
/// Android NDK 설정이 아예 없다: <c>--android-ndk</c>, <c>ANDROID_NDK_HOME</c>, <c>ANDROID_NDK_ROOT</c>가 없고 Android SDK 기본 위치에도 NDK가
/// 없다. 설정했지만 망가진 NDK(<c>source.properties</c>·<c>clang++</c>·<c>llvm-ar</c>·<c>ld.lld</c> 없음)는 이 예외가 아니라
/// FileNotFoundException이다 — NDK가 없는 호스트에서만 교차 빌드 시험을 건너뛰고, CI가 고른 NDK가 망가졌으면 시험이 실패한다.
/// </summary>
public sealed class AndroidNdkNotConfiguredException() : Exception("Android NDK not found. Pass --android-ndk or set ANDROID_NDK_HOME.");
