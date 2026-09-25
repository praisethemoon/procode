/* The process.
 *
 * THE WORKING DIRECTORY DECIDES WHICH PROJECT THIS IS, and it is the one piece
 * of configuration that cannot be got wrong quietly. §1.4 finds the project
 * store by walking up from the working directory, like `.git` — so an agent
 * host that starts this server somewhere other than the workspace gets a
 * server that searches the global tier and files into it, and every answer it
 * gives is plausible. `cwd` is therefore passed explicitly rather than left to
 * `Kb`'s default, so that there is a line to read when somebody asks which
 * store was being spoken to.
 *
 * `KB_BIN` NAMES THE BINARY AND NOTHING ELSE DOES. A bare `kb` is resolved
 * through PATH by the platform's own exec; there is no search here to get
 * wrong. `KB_STORE` is not read here at all — it is the CLI's own way of
 * moving the global tier and it reaches the child through the inherited
 * environment, which is where a variable the store owns belongs.
 *
 * THE EXIT CODE IS ABOUT THIS PROCESS, NOT ABOUT THE STORE. A refused tool
 * call is a result; a stream that could not be framed is a failure to run.
 */

import { Kb } from "kb-js";

import { Server } from "./server";
import { serve } from "./transport";

export async function main(): Promise<number> {
    const kb = new Kb({ bin: process.env["KB_BIN"], cwd: process.cwd() });
    let fatal = false;
    await serve(process.stdin, process.stdout, new Server(kb).dispatch, {
        onError: (error: Error): void => {
            fatal = true;
            process.stderr.write(`kb-mcp: ${error.message}\n`);
        },
    });
    return fatal ? 1 : 0;
}

/* Started directly rather than imported: `bin/kb-mcp` calls `main()` itself,
 * and this is for `node out/main.js`. */
if (require.main === module) {
    main().then(
        (code) => {
            process.exitCode = code;
        },
        (e: unknown) => {
            process.stderr.write(`kb-mcp: ${e instanceof Error ? e.message : String(e)}\n`);
            process.exitCode = 2;
        },
    );
}
