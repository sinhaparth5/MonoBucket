import { prefersReducedMotion } from 'svelte/motion';

// Keep motion decisions in one place so every Svelte transition responds to the
// operating-system preference. CSS handles ordinary hover transitions; these
// helpers cover JS-driven `in:`, `out:`, `transition:` and `animate:` directives.
export function motionDuration(milliseconds: number): number {
	return prefersReducedMotion.current ? 0 : milliseconds;
}

export function motionDistance(pixels: number): number {
	return prefersReducedMotion.current ? 0 : pixels;
}

export function motionDelay(milliseconds: number): number {
	return prefersReducedMotion.current ? 0 : milliseconds;
}
