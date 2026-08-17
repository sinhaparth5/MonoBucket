<script lang="ts">
	// A single headline number. The value animates between readings so a change
	// during the 5s refresh is visible rather than a silent swap — the whole
	// point of a live dashboard is noticing that something moved.
	import { prefersReducedMotion, Tween } from 'svelte/motion';
	import { cubicOut } from 'svelte/easing';

	interface Props {
		label: string;
		value: number;
		format: (value: number) => string;
		hint?: string;
		/// Literal Tailwind class. Left unset the tile inherits body colour, which
		/// is right for most tiles: colour here should mean something.
		valueClass?: string;
	}

	let { label, value, format, hint, valueClass = '' }: Props = $props();

	// Starts at zero so the first paint counts up into place; every later reading
	// animates from whatever was on screen.
	const counter = new Tween(0, { duration: 500, easing: cubicOut });

	$effect(() => {
		// Someone who has asked the OS for less motion did not ask for a spinning
		// odometer either.
		counter.set(value, { duration: prefersReducedMotion.current ? 0 : 500 });
	});
</script>

<div class="border-base-300 bg-base-100 rounded-box flex flex-col gap-1 border p-4">
	<span class="text-base-content/70 text-xs font-medium tracking-wide uppercase">{label}</span>
	<span class="text-3xl font-semibold tabular-nums {valueClass}">{format(counter.current)}</span>
	{#if hint}
		<span class="text-base-content/50 text-xs">{hint}</span>
	{/if}
</div>
