// 언어 서버 수명(Docs/Design/SparksTools.ko.md 12.3절·12.3.1절): 원본 찾기 → 칸 복사 → 실행, 원본이 바뀌면 새 칸으로 교체.
// 시작·교체·다시 시작은 한 줄로 이어 실행한다(동시에 둘이 돌지 않게).

import * as path from "path";
import * as vscode from "vscode";
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from "vscode-languageclient/node";
import { findServer } from "./serverLocation";
import { cleanSlots, removeOwnServerDirectory, ServerSlot, stageSlot } from "./serverSlots";

// 원본 변경 알림을 묶는 시간이다(링크가 여러 번 쓰는 동안 칸을 여러 번 만들지 않게 — 판정은 칸의 해시 조건이 한다).
const RebuildQuietMilliseconds = 500;

const UpdatedMessageMilliseconds = 3000;

// 서버가 initialize에 답하기를 기다리는 상한이다(넘으면 멈추고 알린다 — 작업 줄이 영구히 막히지 않게).
const StartTimeoutMilliseconds = 30000;

// 실행 중인 서버가 없을 때 "원본이 복사 도중 바뀜"을 다시 시도하는 횟수다(그 뒤에는 알린다).
const StageRetriesWithoutServer = 3;

const OpenSettings = "Open Settings";

export class ServerController implements vscode.Disposable {
    private client: LanguageClient | undefined;
    private running: ServerSlot | undefined;
    private source: string | undefined;
    private watcher: vscode.FileSystemWatcher | undefined;
    private rebuildTimer: NodeJS.Timeout | undefined;
    private queue: Promise<void> = Promise.resolve();
    private reportedError: string | undefined;
    private started = false;
    private stageRetries = 0;
    private disposed = false;

    constructor(private readonly storage: string, private readonly output: vscode.LogOutputChannel) {}

    // 원본을 다시 찾아 서버를 (다시) 시작한다. 시작·설정 변경·명령이 부른다.
    restart(): Promise<void> {
        return this.enqueue(() => this.restartNow());
    }

    async dispose(): Promise<void> {
        this.disposed = true;
        this.clearWatch();
        await this.queue;
        await this.stopClient();
        await cleanSlots(this.storage, undefined);
        await removeOwnServerDirectory(this.storage);
    }

    private enqueue(work: () => Promise<void>): Promise<void> {
        this.queue = this.queue.then(work).catch((error: unknown) => {
            this.output.appendLine(`Sparks: ${(error as Error).message}`);
        });
        return this.queue;
    }

    private async restartNow(): Promise<void> {
        if (this.disposed) {
            return;
        }

        const configuration = vscode.workspace.getConfiguration("sparks");
        const folders = (vscode.workspace.workspaceFolders ?? []).filter((folder) => folder.uri.scheme === "file").map((folder) => folder.uri.fsPath);
        const lookup = findServer(configuration.get<string>("server.path", ""), folders);
        if ("error" in lookup) {
            this.clearWatch();
            await this.stopClient();
            this.source = undefined;
            this.reportError(lookup.error);
            return;
        }

        this.reportedError = undefined;
        this.source = lookup.path;
        // 감시는 매번 다시 만든다(감시하던 폴더가 지워지면 — clean 빌드 — 알림이 끊기므로 다시 시작 명령이 되살린다).
        this.watch(lookup.path);
        this.stageRetries = 0;
        await this.swapTo(true);
    }

    // 원본으로 칸을 만들고, 실행 중인 칸과 다르거나 always면 클라이언트를 새 칸으로 다시 시작한다.
    private async swapTo(always: boolean): Promise<void> {
        if (this.disposed || this.source === undefined) {
            return;
        }

        const staged = await stageSlot(this.source, this.storage);
        if ("sourceChanged" in staged) {
            this.output.appendLine(`Sparks: '${this.source}' changed while it was copied; waiting for the next change.`);
            // 서버가 없으면 다음 알림이 오지 않을 수 있으므로(원본이 이미 다 쓰였다) 몇 번 다시 시도하고, 그래도 안 되면 알린다.
            if (this.client === undefined) {
                if (this.stageRetries < StageRetriesWithoutServer) {
                    ++this.stageRetries;
                    this.scheduleRebuildCheck();
                } else {
                    this.reportError(`Could not copy the Sparks language server '${this.source}' because it kept changing. Run 'Sparks: Restart Language Server' after the build finishes.`);
                }
            }

            return;
        }

        if ("error" in staged) {
            this.reportError(staged.error);
            return;
        }

        this.stageRetries = 0;
        const updated = this.client !== undefined && this.running?.name !== staged.slot.name;
        if (!always && this.running?.name === staged.slot.name) {
            return;
        }

        await this.stopClient();
        if (!(await this.startClient(staged.slot))) {
            return;
        }

        await cleanSlots(this.storage, staged.slot.name);
        if (updated) {
            vscode.window.setStatusBarMessage("Sparks language server updated", UpdatedMessageMilliseconds);
        }
    }

    // 클라이언트를 시작한다. 서버가 답하지 않거나 시작이 실패하면 멈추고 알린 뒤 거짓을 돌려준다.
    private async startClient(slot: ServerSlot): Promise<boolean> {
        const folders = vscode.workspace.workspaceFolders ?? [];
        const serverOptions: ServerOptions = {
            command: slot.executable,
            transport: TransportKind.stdio,
            options: folders.length > 0 && folders[0].uri.scheme === "file" ? { cwd: folders[0].uri.fsPath } : {},
        };
        const clientOptions: LanguageClientOptions = {
            documentSelector: [
                { scheme: "file", language: "sparks" },
                { scheme: "file", pattern: "**/SparksPackage.json5" },
            ],
            initializationOptions: { schemas: vscode.workspace.getConfiguration("sparks").get<string[]>("server.schemas", []) },
            outputChannel: this.output,
        };
        const client = new LanguageClient("sparks", "Sparks", serverOptions, clientOptions);
        this.client = client;
        this.running = slot;
        this.output.appendLine(`Sparks: starting '${slot.executable}' (copied from '${this.source}').`);
        let timer: NodeJS.Timeout | undefined;
        const timeout = new Promise<never>((_, reject) => {
            timer = setTimeout(() => reject(new Error(`The Sparks language server did not answer 'initialize' within ${StartTimeoutMilliseconds / 1000} s.`)),
                StartTimeoutMilliseconds);
        });
        try {
            await Promise.race([client.start(), timeout]);
            this.started = true;
            return true;
        } catch (error) {
            await this.stopClient();
            this.reportError(`Could not start the Sparks language server '${slot.executable}': ${(error as Error).message}`);
            return false;
        } finally {
            clearTimeout(timer);
        }
    }

    private async stopClient(): Promise<void> {
        const client = this.client;
        const started = this.started;
        this.client = undefined;
        this.running = undefined;
        this.started = false;
        if (client !== undefined) {
            // 시작하지 못한 클라이언트도 stop을 불러야 서버 프로세스가 끝난다(그때의 거부는 예상한 것이라 남기지 않는다).
            try {
                await client.stop();
            } catch (error) {
                if (started) {
                    this.output.appendLine(`Sparks: stopping the language server failed: ${(error as Error).message}`);
                }
            }
        }
    }

    // 원본 실행 파일과 기호 파일을 감시한다(작업 공간 밖도 RelativePattern으로 — 12.3.1절).
    private watch(source: string): void {
        this.clearWatch();
        // `SparksLsp`(확장자 없음 — macOS·Linux)와 `SparksLsp.exe`·`SparksLsp.pdb`다.
        const name = path.parse(source).name;
        const pattern = new vscode.RelativePattern(vscode.Uri.file(path.dirname(source)), `{${name},${name}.*}`);
        const watcher = vscode.workspace.createFileSystemWatcher(pattern);
        const changed = () => this.scheduleRebuildCheck();
        watcher.onDidChange(changed);
        watcher.onDidCreate(changed);
        this.watcher = watcher;
    }

    private scheduleRebuildCheck(): void {
        if (this.rebuildTimer !== undefined) {
            clearTimeout(this.rebuildTimer);
        }

        this.rebuildTimer = setTimeout(() => {
            this.rebuildTimer = undefined;
            void this.enqueue(() => this.swapTo(false));
        }, RebuildQuietMilliseconds);
    }

    private clearWatch(): void {
        if (this.rebuildTimer !== undefined) {
            clearTimeout(this.rebuildTimer);
            this.rebuildTimer = undefined;
        }

        this.watcher?.dispose();
        this.watcher = undefined;
    }

    // 같은 오류는 한 번만 알린다(설정을 고치거나 다시 시작하면 다시 찾는다).
    private reportError(message: string): void {
        this.output.appendLine(`Sparks: ${message}`);
        if (this.reportedError === message) {
            return;
        }

        this.reportedError = message;
        void vscode.window.showErrorMessage(message, OpenSettings).then((choice) => {
            if (choice === OpenSettings) {
                void vscode.commands.executeCommand("workbench.action.openSettings", "sparks.server");
            }
        });
    }
}
