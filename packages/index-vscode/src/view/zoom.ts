/* A document tab's zoom: fixed steps, as a browser's, so − and + always land
 * on the same levels and 100% is one of them. Pure: the page keeps the level,
 * this only says what the next one is. */

export const ZOOM_STEPS = [20, 25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400] as const;
export const ZOOM_DEFAULT = 100;

/* The next step up, or the largest when already there. A level between steps
 * goes to the next step above it. */
export function zoomIn(level: number): number {
    return ZOOM_STEPS.find((s) => s > level) ?? ZOOM_STEPS[ZOOM_STEPS.length - 1];
}

/* The next step down, or the smallest when already there. */
export function zoomOut(level: number): number {
    return [...ZOOM_STEPS].reverse().find((s) => s < level) ?? ZOOM_STEPS[0];
}
