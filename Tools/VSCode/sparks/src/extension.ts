// Sparks VS Code 확장 진입점(Docs/Design/SparksTools.ko.md 12절): 언어 등록·문법은 manifest가, 서버 수명은
// ServerController가 맡는다.

import * as vscode from "vscode";
import { ServerController } from "./serverController";

let controller: ServerController | undefined;

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    const output = vscode.window.createOutputChannel("Sparks", { log: true });
    context.subscriptions.push(output);
    controller = new ServerController(context.globalStorageUri.fsPath, output);
    const active = controller;
    context.subscriptions.push(vscode.commands.registerCommand("sparks.restartServer", () => active.restart()));
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (event.affectsConfiguration("sparks.server")) {
            void active.restart();
        }
    }));
    context.subscriptions.push(vscode.workspace.onDidChangeWorkspaceFolders(() => void active.restart()));
    await active.restart();
}

export async function deactivate(): Promise<void> {
    const active = controller;
    controller = undefined;
    await active?.dispose();
}
