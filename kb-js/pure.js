/* `kb-js/pure` for a resolver that does not read `exports` maps.
 *
 * TypeScript's `node10` resolution — which is what a CommonJS extension host
 * project uses — looks for `kb-js/pure.js` on disk and ignores the `exports`
 * field entirely. A bundler reads the map. Both find this file, and this file
 * is the only place the two answers can differ, which is why it is two lines
 * rather than a copy of anything.
 */
module.exports = require("./out/pure.js");
