// 서버 교체 구조의 칸(Docs/Design/SparksTools.ko.md 12.3.1절): 원본을 확장 저장소의 `server/<pid>/<해시>/`에 복사해 그
// 복사본을 실행한다. 빌드 출력은 잠기지 않으므로 서버가 켜진 채 다시 빌드할 수 있다.

import * as crypto from "crypto";
import * as fs from "fs";
import * as path from "path";

// 칸 이름에 쓰는 SHA-256 hex 글자 수다.
const SlotHashLength = 16;

const StagingName = ".staging";

const SymbolsExtension = ".pdb";

// 실행 파일 권한(POSIX — 복사가 권한을 지킨다는 보장이 없다).
const ExecutableMode = 0o755;

// 게시한 칸이다. executable이 실행할 복사본이다.
export interface ServerSlot {
    readonly name: string;
    readonly executable: string;
}

// 칸을 만들지 못한 이유다. sourceChanged는 원본이 복사 도중 바뀐 것(다음 변경 알림을 기다린다)이다.
export type StageResult = { slot: ServerSlot } | { sourceChanged: true } | { error: string };

async function hashFile(file: string): Promise<string> {
    const hash = crypto.createHash("sha256");
    await new Promise<void>((resolve, reject) => {
        const stream = fs.createReadStream(file);
        stream.on("data", (chunk) => hash.update(chunk));
        stream.on("error", reject);
        stream.on("end", () => resolve());
    });
    return hash.digest("hex");
}

function sameStat(before: fs.Stats, after: fs.Stats): boolean {
    return before.size === after.size && before.mtimeMs === after.mtimeMs;
}

function exists(file: string): boolean {
    try {
        fs.statSync(file);
        return true;
    } catch {
        return false;
    }
}

// 지우기 재시도(Node가 EBUSY·EPERM 등에서 늘어나는 간격으로 다시 한다)다. Windows는 서버가 끝난 직후나 백신 검사 중
// 잠깐 복사본을 잡고 있다 — 그래도 남으면 다음 정리가 지운다(정리는 정확성 조건이 아니다).
const RemoveRetries = 5;
const RemoveRetryMilliseconds = 100;

async function removeQuietly(target: string): Promise<void> {
    try {
        await fs.promises.rm(target, { recursive: true, force: true, maxRetries: RemoveRetries, retryDelay: RemoveRetryMilliseconds });
    } catch {
        // 잠긴 칸(아직 끝나지 않은 서버)은 다음 정리 때 다시 본다.
    }
}

// 이 창(확장 호스트)의 칸 폴더다.
export function ownServerDirectory(storage: string): string {
    return path.join(storage, "server", String(process.pid));
}

// 원본을 여는 중의 일시 오류(링크가 쓰는 중 — 다음 변경 알림을 기다린다)다.
function isTransient(error: unknown): boolean {
    const code = (error as NodeJS.ErrnoException).code;
    return code === "EBUSY" || code === "EPERM" || code === "EACCES" || code === "ENOENT";
}

function sourceUnchanged(source: string, before: fs.Stats): boolean {
    try {
        return sameStat(before, fs.statSync(source));
    } catch {
        return false;
    }
}

// staging 폴더 이름을 칸 이름으로 바꾼다. Windows는 방금 복사한 exe를 백신이 잡고 있으면 EPERM·EACCES·EBUSY로 실패하므로
// 늘어나는 간격으로 다시 한다(rm과 같은 상수).
async function renameWithRetries(from: string, to: string): Promise<void> {
    for (let attempt = 1; ; ++attempt) {
        try {
            fs.renameSync(from, to);
            return;
        } catch (error) {
            const code = (error as NodeJS.ErrnoException).code;
            if (attempt > RemoveRetries || !(code === "EPERM" || code === "EACCES" || code === "EBUSY")) {
                throw error;
            }

            await new Promise((resolve) => setTimeout(resolve, RemoveRetryMilliseconds * attempt));
        }
    }
}

// source를 칸에 복사해 게시한다. 원본의 해시와 복사본의 해시가 같고 복사 전후 원본의 크기·수정 시각이 같을 때만 게시한다.
// 원본 쪽 실패(열기·읽기, 복사 도중 바뀜)는 sourceChanged, 칸 쪽 실패(쓰기·이름 바꾸기)는 error다.
export async function stageSlot(source: string, storage: string): Promise<StageResult> {
    const own = ownServerDirectory(storage);
    const executableName = path.basename(source);
    const symbols = path.join(path.dirname(source), path.parse(source).name + SymbolsExtension);
    let before: fs.Stats;
    let sourceHash: string;
    try {
        before = fs.statSync(source);
        sourceHash = await hashFile(source);
    } catch (error) {
        if (isTransient(error)) {
            return { sourceChanged: true };
        }

        return { error: `Could not read the Sparks language server '${source}': ${(error as Error).message}` };
    }

    const name = sourceHash.slice(0, SlotHashLength);
    const slotDirectory = path.join(own, name);
    const executable = path.join(slotDirectory, executableName);
    if (exists(executable)) {
        return { slot: { name, executable } };
    }

    const staging = path.join(own, StagingName);
    try {
        await removeQuietly(staging);
        fs.mkdirSync(staging, { recursive: true });
        const copied = path.join(staging, executableName);
        fs.copyFileSync(source, copied);
        if (exists(symbols)) {
            fs.copyFileSync(symbols, path.join(staging, path.basename(symbols)));
        }

        if (process.platform !== "win32") {
            fs.chmodSync(copied, ExecutableMode);
        }

        const copiedHash = await hashFile(copied);
        if (copiedHash !== sourceHash || !sourceUnchanged(source, before)) {
            await removeQuietly(staging);
            return { sourceChanged: true };
        }

        await removeQuietly(slotDirectory);
        await renameWithRetries(staging, slotDirectory);
        return { slot: { name, executable } };
    } catch (error) {
        await removeQuietly(staging);
        // 복사 도중 원본이 바뀌었거나 열리지 않으면 원본 쪽, 아니면 칸 쪽 실패다.
        if (!sourceUnchanged(source, before)) {
            return { sourceChanged: true };
        }

        return { error: `Could not copy the Sparks language server '${source}' to '${slotDirectory}': ${(error as Error).message}` };
    }
}

// 이 창의 칸 폴더 전체를 지운다(확장이 끝날 때 — 실패하면 다음 창의 정리가 지운다).
export async function removeOwnServerDirectory(storage: string): Promise<void> {
    await removeQuietly(ownServerDirectory(storage));
}

function processAlive(pid: number): boolean {
    try {
        process.kill(pid, 0);
        return true;
    } catch (error) {
        // EPERM은 살아 있지만 신호 권한이 없는 프로세스다.
        return (error as NodeJS.ErrnoException).code === "EPERM";
    }
}

// 자기 폴더에서 keep 밖의 칸을, 다른 pid 폴더는 그 프로세스가 없을 때만 지운다.
export async function cleanSlots(storage: string, keep: string | undefined): Promise<void> {
    const server = path.join(storage, "server");
    let entries: fs.Dirent[];
    try {
        entries = fs.readdirSync(server, { withFileTypes: true });
    } catch {
        return;
    }

    for (const entry of entries) {
        if (!entry.isDirectory()) {
            continue;
        }

        const pid = Number(entry.name);
        if (!Number.isInteger(pid) || pid <= 0) {
            continue;
        }

        const directory = path.join(server, entry.name);
        if (pid !== process.pid) {
            if (!processAlive(pid)) {
                await removeQuietly(directory);
            }

            continue;
        }

        let slots: string[];
        try {
            slots = fs.readdirSync(directory);
        } catch {
            continue;
        }

        for (const slot of slots) {
            if (slot !== keep) {
                await removeQuietly(path.join(directory, slot));
            }
        }
    }
}
