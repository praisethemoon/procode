import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

/* Relative asset paths, so the build works at any URL: the project page
 * (/procode/), a custom domain, or a file opened from disk. The screenshots
 * and icons are imported from the repository itself (../assets, the
 * extensions' media), so the page never keeps copies of its own. */
export default defineConfig({
    base: "./",
    plugins: [react()],
});
