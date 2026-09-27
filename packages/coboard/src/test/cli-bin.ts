import * as fs from "node:fs";
import * as path from "node:path";

/* The CLI a test drives, or "" when there is none and the test skips.
 *
 * $LAP_BIN or $KB_BIN when set; else the one built in this repository, by
 * CMake at the root (Visual Studio puts it under Release/) or by make in the
 * CLI's own directory; else the one on PATH. npm does not build the CLIs, so
 * a test must not assume either build ran.
 *
 * The same file sits in each package that drives a CLI, since each compiles
 * its tests on its own. */
export function cliBin(name: "lap" | "kb"): string {
    const set = process.env[name === "lap" ? "LAP_BIN" : "KB_BIN"];
    if (set) {
        return set;
    }
    const exe = process.platform === "win32" ? `${name}.exe` : name;
    const repo = path.resolve(__dirname, "..", "..", "..", "..");
    const built = [
        path.join(repo, "build", "cli", `${name}-cli`, exe),
        path.join(repo, "build", "cli", `${name}-cli`, "Release", exe),
        path.join(repo, "cli", `${name}-cli`, "bin", exe),
    ];
    for (const dir of (process.env["PATH"] ?? "").split(path.delimiter)) {
        if (dir) {
            built.push(path.join(dir, exe));
        }
    }
    return built.find((p) => fs.existsSync(p)) ?? "";
}

/* Why a test that needs the CLI was skipped. */
export function noCli(name: "lap" | "kb"): string {
    const env = name === "lap" ? "LAP_BIN" : "KB_BIN";
    return `${name} not found: build the CLIs with CMake, put ${name} on PATH, or set ${env}`;
}
