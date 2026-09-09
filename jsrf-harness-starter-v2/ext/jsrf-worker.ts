/**
 * JSRF Harness Starter v2 — thin Pi adapter.
 *
 * Environment:
 *   JSRF_HARNESS_RUN=/absolute/path/to/runs/<run-id>
 *
 * The deterministic policy engine (policy/cli.py) is authoritative.
 * This adapter only:
 *   1. resolves the active run + project root from the run manifest,
 *   2. registers the eight jsrf_* tools,
 *   3. calls setActiveTools() so the worker sees only those tools,
 *   4. shells out to policy/cli.py BEFORE every action and performs the
 *      underlying action only on an ALLOWED decision.
 *
 * No budget or scope logic is duplicated in TypeScript.
 *
 * Launch:
 *   JSRF_HARNESS_RUN=... pi --no-extensions -e ext/jsrf-worker.ts
 *
 * Test inside Pi with a synthetic prepared run (see README.md).
 */

import { execFile, spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import { dirname, isAbsolute, join, resolve } from "node:path";
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { withFileMutationQueue } from "@earendil-works/pi-coding-agent";
import { Type } from "typebox";

// Directory of this file. Works whether pi bundles the extension as CJS or ESM.
const here = (() => {
	try {
		return __dirname;
	} catch {
		return process.cwd();
	}
})();

const RESTRICTED_TOOLS = [
	"jsrf_read",
	"jsrf_search",
	"jsrf_patch",
	"jsrf_build",
	"jsrf_run_capture",
	"jsrf_test",
	"jsrf_write_evidence",
	"jsrf_block",
] as const;

interface RunContext {
	runDir: string;
	manifestPath: string;
	projectRoot: string;
	task: Record<string, any>;
	manifest: Record<string, any>;
}

interface PolicyDecision {
	allowed: boolean;
	message: string;
	counters: Record<string, number>;
}

function fail(text: string): any {
	return {
		content: [{ type: "text" as const, text }],
		details: { ok: false },
	};
}

function ok(text: string): any {
	return { content: [{ type: "text" as const, text }], details: { ok: true } };
}

function runPolicyCli(runDir: string, args: string[]): Promise<PolicyDecision> {
	const cli = join(here, "..", "policy", "cli.py");
	return new Promise((resolvePromise, rejectPromise) => {
		execFile(
			"python3",
			[cli, "--run", runDir, ...args],
			{ maxBuffer: 10 * 1024 * 1024, timeout: 30_000 },
			(err, stdout) => {
				if (err && err.code !== 3) {
					rejectPromise(err);
					return;
				}
				try {
					resolvePromise(JSON.parse(stdout));
				} catch {
					rejectPromise(new Error(`policy CLI returned unparseable output: ${stdout}`));
				}
			},
		);
	});
}

/**
 * Normalize a user-supplied path into a project-root-relative path.
 * The policy engine itself rejects absolute paths and .. escapes; this only
 * makes the common case (absolute path under the project root) work.
 */
function toRelPath(p: string, projectRoot: string): string {
	let path = p.trim();
	if (path.startsWith("@")) path = path.slice(1);
	if (!isAbsolute(path)) return path;
	const abs = resolve(path);
	if (abs === projectRoot) return ".";
	if (abs.startsWith(projectRoot + "/")) return abs.slice(projectRoot.length + 1);
	return abs; // engine will deny (absolute) — surfaced verbatim
}

function truncate(text: string, maxChars: number): string {
	if (text.length <= maxChars) return text;
	return text.slice(0, maxChars) + `\n... [truncated, ${text.length} chars total]`;
}

function loadRun(): RunContext | string {
	const runDirRaw = process.env.JSRF_HARNESS_RUN;
	if (!runDirRaw) {
		return "jsrf-worker: JSRF_HARNESS_RUN is not set. Launch with JSRF_HARNESS_RUN=<run dir> (harness.py prepare prints it).";
	}
	const runDir = resolve(runDirRaw);
	const manifest = JSON.parse(readFileSync(join(runDir, "manifest.json"), "utf-8"));
	const task = JSON.parse(readFileSync(join(runDir, "task.json"), "utf-8"));
	return {
		runDir,
		manifestPath: join(runDir, "manifest.json"),
		projectRoot: manifest.project_root,
		task,
		manifest,
	};
}

export default function jsrfWorkerExtension(pi: ExtensionAPI) {
	const run = loadRun();
	if (typeof run === "string") {
		pi.on("session_start", (_event, ctx) => {
			ctx.ui.notify(run, "error");
		});
		return;
	}

	const { runDir, projectRoot, task, manifest } = run;

	const authorize = (args: string[]) => runPolicyCli(runDir, args);

	// --- command helpers -------------------------------------------------

	async function runCommand(command: string, timeoutMs: number, signal?: AbortSignal): Promise<string> {
		return new Promise((resolveP, rejectP) => {
			const child = spawnBash(command, timeoutMs, signal);
			const parts: string[] = [];
			child.stdout?.on("data", (d) => parts.push(String(d)));
			child.stderr?.on("data", (d) => parts.push(String(d)));
			child.on("error", rejectP);
			child.on("close", (code) => {
				const out = parts.join("").trim();
				resolveP(`[exit ${code}]\n${truncate(out || "(no output)", 8000)}`);
			});
		});
	}

	async function searchDir(rel: string, query: string): Promise<string> {
		const abs = join(projectRoot, rel);
		const st = await stat(abs).catch(() => null);
		const files: string[] = [];
		if (st?.isDirectory()) {
			// bounded: files directly in the allowed directory, no recursion
			const entries = await readdir(abs, { withFileTypes: true });
			for (const e of entries) if (e.isFile()) files.push(join(rel, e.name));
		} else if (st) {
			files.push(rel);
		} else {
			return `no such file or directory: ${rel}`;
		}
		const lines: string[] = [];
		const needle = query.toLowerCase();
		for (const f of files) {
			const text = await readFile(join(projectRoot, f), "utf-8").catch(() => null);
			if (!text) continue;
			text.split("\n").forEach((line, i) => {
				if (line.toLowerCase().includes(needle)) {
					lines.push(`${f}:${i + 1}:${line.trim().slice(0, 200)}`);
					if (lines.length >= 100) throw new Error("search limit reached");
				}
			});
		}
		return lines.length ? lines.join("\n") : `no matches for "${query}" in ${rel}`;
	}

	// --- tool factories ---------------------------------------------------

	pi.registerTool({
		name: "jsrf_read",
		label: "jsrf_read",
		description: "Bounded read of a file allowed by the active task. Policy-charged before reading.",
		parameters: Type.Object({
			path: Type.String({ description: "Project-relative (or absolute under project root) file path" }),
			offset: Type.Optional(Type.Number({ description: "1-indexed first line to read" })),
			limit: Type.Optional(Type.Number({ description: "Number of lines to read" })),
		}),
		async execute(_toolCallId, params: any) {
			const rel = toRelPath(params.path, projectRoot);
			const offset = Math.max(1, params.offset ?? 1);
			const limit = Math.max(1, params.limit ?? 40);
			const decision = await authorize(["read", "--path", rel, "--lines", String(limit)]);
			if (!decision.allowed) return fail(decision.message);
			const text = await readFile(join(projectRoot, rel), "utf-8").catch(
				(e: Error) => `read error: ${e.message}`,
			);
			const lines = text.split("\n").slice(offset - 1, offset - 1 + limit);
			return ok(
				`${decision.message}\n${rel}:${offset}\n` +
					lines.map((l, i) => `${offset + i}: ${l}`).join("\n"),
			);
		},
	});

	pi.registerTool({
		name: "jsrf_search",
		label: "jsrf_search",
		description: "Simple case-insensitive substring search within one allowed file or directory (non-recursive).",
		parameters: Type.Object({
			query: Type.String({ description: "Substring to search for" }),
			path: Type.Optional(Type.String({ description: "Allowed file or directory, default '.'" })),
		}),
		async execute(_toolCallId, params: any) {
			const rel = toRelPath(params.path ?? ".", projectRoot);
			const decision = await authorize(["search", "--query", params.query, "--path", rel]);
			if (!decision.allowed) return fail(decision.message);
			try {
				return ok(`${decision.message}\n${await searchDir(rel, params.query)}`);
			} catch (e) {
				return fail(`search error: ${(e as Error).message}`);
			}
		},
	});

	pi.registerTool({
		name: "jsrf_patch",
		label: "jsrf_patch",
		description:
			"Replace exact text in an allowed_write_file. oldText must match exactly and be unique in the file.",
		parameters: Type.Object({
			path: Type.String({ description: "Project-relative path of the file to modify" }),
			oldText: Type.String({ description: "Exact text to replace (must be unique in the file)" }),
			newText: Type.String({ description: "Replacement text" }),
		}),
		async execute(_toolCallId, params: any) {
			const rel = toRelPath(params.path, projectRoot);
			const decision = await authorize(["write", "--path", rel]);
			if (!decision.allowed) return fail(decision.message);
			const abs = join(projectRoot, rel);
			const result = await withFileMutationQueue(abs, async () => {
				const text = await readFile(abs, "utf-8").catch(() => null);
				if (text === null) return `patch error: cannot read ${rel}`;
				const count = text.split(params.oldText).length - 1;
				if (count === 0) return "patch error: oldText not found";
				if (count > 1) return `patch error: oldText matches ${count} locations; must be unique`;
				await writeFile(abs, text.replace(params.oldText, params.newText), "utf-8");
				return `${decision.message}\npatched ${rel} (1 replacement)`;
			});
			return result.startsWith("patch error") ? fail(result) : ok(result);
		},
	});

	async function executeCommandTool(
		kind: "build" | "runtime" | "test",
		command: string,
		timeoutMs: number,
		signal?: AbortSignal,
	) {
		const decision = await authorize(["command", "--kind", kind, "--command", command]);
		if (!decision.allowed) return fail(decision.message);
		const output = await runCommand(command, timeoutMs, signal);
		return ok(`${decision.message}\n${output}`);
	}

	pi.registerTool({
		name: "jsrf_build",
		label: "jsrf_build",
		description: "Run the build command defined by the active task/manifest. No free-form commands.",
		parameters: Type.Object({}),
		async execute(_toolCallId, _params: any, signal?: AbortSignal) {
			const command = task.build_command ?? manifest.build_command;
			if (!command) return fail("no build_command defined in task or manifest; nothing to run");
			return executeCommandTool("build", command, 600_000, signal);
		},
	});

	pi.registerTool({
		name: "jsrf_run_capture",
		label: "jsrf_run_capture",
		description: "Run the runtime command defined by the active task/manifest with a bounded timeout.",
		parameters: Type.Object({}),
		async execute(_toolCallId, _params: any, signal?: AbortSignal) {
			const command = task.runtime_command ?? manifest.runtime_command;
			if (!command) return fail("no runtime_command defined in task or manifest; nothing to run");
			const timeoutMs = Number(task.runtime_timeout_ms ?? manifest.runtime_timeout_ms ?? 30_000);
			return executeCommandTool("runtime", command, timeoutMs, signal);
		},
	});

	pi.registerTool({
		name: "jsrf_test",
		label: "jsrf_test",
		description: "Run the test command defined by the active task/manifest.",
		parameters: Type.Object({}),
		async execute(_toolCallId, _params: any, signal?: AbortSignal) {
			const command = task.test_command ?? manifest.test_command;
			if (!command) return fail("no test_command defined in task or manifest; nothing to run");
			return executeCommandTool("test", command, 300_000, signal);
		},
	});

	pi.registerTool({
		name: "jsrf_write_evidence",
		label: "jsrf_write_evidence",
		description: "Write evidence/artifact content to an allowed_write_file (overwrites).",
		parameters: Type.Object({
			path: Type.String({ description: "Project-relative path of the evidence file" }),
			content: Type.String({ description: "Full content to write" }),
		}),
		async execute(_toolCallId, params: any) {
			const rel = toRelPath(params.path, projectRoot);
			const decision = await authorize(["write", "--path", rel]);
			if (!decision.allowed) return fail(decision.message);
			const abs = join(projectRoot, rel);
			const result = await withFileMutationQueue(abs, async () => {
				await mkdir(dirname(abs), { recursive: true });
				await writeFile(abs, params.content, "utf-8");
				return `${decision.message}\nwrote ${rel} (${params.content.length} chars)`;
			});
			return ok(result);
		},
	});

	pi.registerTool({
		name: "jsrf_block",
		label: "jsrf_block",
		description:
			"Terminate the task as BLOCKED with a reason. A bounded BLOCKED result is a successful outcome. Do no other work after calling this.",
		parameters: Type.Object({
			reason: Type.String({ description: "Why the task is blocked (what evidence is missing / what scope is needed)" }),
		}),
		async execute(_toolCallId, params: any) {
			const decision = await authorize(["block", "--reason", params.reason]);
			const text = `${decision.message}\nTerminal status: BLOCKED — stop all work now.`;
			return decision.allowed ? ok(text) : fail(text);
		},
	});

	// --- activation --------------------------------------------------------

	// Diagnostic: /jsrf-active-tools prints the active tool set to the terminal.
	pi.registerCommand("jsrf-active-tools", {
		description: "Print the currently active tools (jsrf-worker diagnostic)",
		handler: async (_args, ctx) => {
			const active = pi.getActiveTools();
			const line = `active tools: [${active.join(", ")}]`;
			ctx.ui.notify(line, "info");
			ctx.ui.setStatus("jsrf", line); // persists in the footer for mechanical inspection
		},
	});

	pi.on("session_start", (_event, ctx) => {
		pi.setActiveTools([...RESTRICTED_TOOLS]);
		const active = pi.getActiveTools();
		ctx.ui.notify(
			`jsrf-worker: run=${runDir} active tools=[${active.join(", ")}]`,
			active.length === RESTRICTED_TOOLS.length ? "info" : "warning",
		);
	});
}

// Local spawn helper (bash -c with hard timeout kill).
function spawnBash(command: string, timeoutMs: number, signal?: AbortSignal) {
	const child = spawn("bash", ["-c", command], { cwd: join(process.cwd()) });
	const timer = setTimeout(() => {
		child.kill("SIGKILL");
	}, timeoutMs);
	signal?.addEventListener("abort", () => child.kill("SIGKILL"), { once: true });
	child.on("close", () => clearTimeout(timer));
	return child;
}
