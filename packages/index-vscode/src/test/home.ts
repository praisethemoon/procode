/* The environment every real kb in these tests runs with: a throwaway HOME.
 * The CLI reads HOME to find ~/.kb/models (index-api §8), so a model installed
 * on the developer's machine must not change what the tests see, and nothing
 * they do may reach the real home directory. */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

const HOME = fs.mkdtempSync(path.join(os.tmpdir(), "knowledge-home-"));
process.on("exit", () => fs.rmSync(HOME, { recursive: true, force: true }));

export const TEST_ENV: NodeJS.ProcessEnv = { ...process.env, HOME };
