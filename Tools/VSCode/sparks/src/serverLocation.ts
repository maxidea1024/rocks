// 언어 서버 원본 찾기(Docs/Design/SparksTools.ko.md 12.3절). 찾은 파일은 실행하지 않는 원본이다 — 실행은 serverSlots.ts의
// 복사본 칸으로 한다(12.3.1절).

import * as fs from "fs";
import * as path from "path";

// 찾은 원본 경로 또는 못 찾은 이유(영어 — 알림에 그대로 쓴다)다.
export type ServerLookup = { path: string } | { error: string };

// Rocks.Build EnginePlanning.DefaultOutput의 구성 이름 순서다(수정 시각이 같으면 앞의 것).
const Configurations = ["debug", "development", "release"];

const RepositoryMarker = "Rocks.proj";

export const BuildCommand = "dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=SparksLsp";

export function serverExecutableName(): string {
    return process.platform === "win32" ? "SparksLsp.exe" : "SparksLsp";
}

// Rocks 출력 폴더 이름의 플랫폼 칸이다(windows·macos·linux).
function platformName(): string {
    switch (process.platform) {
        case "win32":
            return "windows";
        case "darwin":
            return "macos";
        default:
            return "linux";
    }
}

function isFile(candidate: string): boolean {
    try {
        return fs.statSync(candidate).isFile();
    } catch {
        return false;
    }
}

// folder에서 위로 올라가며 Rocks.proj가 있는 첫 폴더(Rocks 저장소 뿌리)다.
function findRepositoryRoot(folder: string): string | undefined {
    let current = path.resolve(folder);
    for (;;) {
        if (isFile(path.join(current, RepositoryMarker))) {
            return current;
        }

        const parent = path.dirname(current);
        if (parent === current) {
            return undefined;
        }

        current = parent;
    }
}

// 저장소 뿌리의 SparksLsp 빌드 중 수정 시각이 가장 늦은 것(같으면 구성 순서의 앞)이다.
function newestBuild(root: string): string | undefined {
    let best: string | undefined;
    let bestTime = -Infinity;
    for (const configuration of Configurations) {
        const candidate = path.join(root, "out", `dotnet-${platformName()}-sparkslsp-${configuration}`, "bin", serverExecutableName());
        let time: number;
        try {
            const stat = fs.statSync(candidate);
            if (!stat.isFile()) {
                continue;
            }

            time = stat.mtimeMs;
        } catch {
            continue;
        }

        if (time > bestTime) {
            best = candidate;
            bestTime = time;
        }
    }

    return best;
}

// configured는 sparks.server.path 설정(빈 글자면 자동 찾기), folders는 작업 공간 폴더 경로(순서대로)다.
export function findServer(configured: string, folders: readonly string[]): ServerLookup {
    if (configured.length > 0) {
        if (!path.isAbsolute(configured) && folders.length === 0) {
            return { error: `sparks.server.path '${configured}' is relative but no workspace folder is open.` };
        }

        const resolved = path.isAbsolute(configured) ? configured : path.resolve(folders[0], configured);
        return isFile(resolved) ? { path: resolved } : { error: `The Sparks language server '${resolved}' (sparks.server.path) does not exist.` };
    }

    const roots: string[] = [];
    for (const folder of folders) {
        const root = findRepositoryRoot(folder);
        if (root !== undefined && !roots.includes(root)) {
            roots.push(root);
        }
    }

    for (const root of roots) {
        const build = newestBuild(root);
        if (build !== undefined) {
            return { path: build };
        }
    }

    const where = roots.length > 0 ? `under ${roots.map((root) => path.join(root, "out")).join(", ")}` : "(no Rocks repository contains the workspace)";
    return { error: `The Sparks language server was not found ${where}. Build it with '${BuildCommand}' or set sparks.server.path.` };
}
