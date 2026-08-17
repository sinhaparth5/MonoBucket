<script lang="ts">
	// A single headline number. The value animates between readings so a change
	// during the 5s refresh is visible rather than a silent swap — the whole
	// point of a live dashboard is noticing that something moved.
	import { prefersReducedMotion, Tween } from 'svelte/motion';
	import { cubicOut } from 'svelte/easing';
	import Icon, { type IconName } from './Icon.svelte';

	interface Props {
		label: string;
		value: number;
		format: (value: number) => string;
		hint?: string;
		icon?: IconName;
		/// Which semantic colour the tile's icon and rule carry. Colour here is
		/// identification, not decoration: four tiles in a row are told apart
		/// faster by hue than by reading four labels.
		tone?: 'primary' | 'secondary' | 'accent' | 'info' | 'success' | 'warning';
		/// Literal Tailwind class for the number itself. Left unset it inherits
		/// body colour, which is right unless the value is the problem.
		valueClass?: string;
	}

	let { label, value, format, hint, icon, tone = 'primary', valueClass = '' }: Props = $props();

	// Enumerated rather than interpolated: Tailwind scans source text for class
	// names, and `bg-${tone}/10` is a string it can never find.
	const TONES = {
		primary: { icon: 'bg-primary/10 text-primary', bar: 'bg-primary' },
		secondary: { icon: 'bg-secondary/10 text-secondary', bar: 'bg-secondary' },
		accent: { icon: 'bg-accent/10 text-accent', bar: 'bg-accent' },
		info: { icon: 'bg-info/10 text-info', bar: 'bg-info' },
		success: { icon: 'bg-success/10 text-success', bar: 'bg-success' },
		warning: { icon: 'bg-warning/10 text-warning', bar: 'bg-warning' }
	} as const;

	// Starts at zero so the first paint counts up into place; every later reading
	// animates from whatever was on screen.
	const counter = new Tween(0, { duration: 500, easing: cubicOut });

	$effect(() => {
		// Someone who has asked the OS for less motion did not ask for a spinning
		// odometer either.
		counter.set(value, { duration: prefersReducedMotion.current ? 0 : 500 });
	});
</script>

<div class="panel interactive-card relative min-h-32 overflow-hidden p-5">
	<span class="absolute inset-x-0 top-0 h-1 {TONES[tone].bar}"></span>
	<div class="flex items-start justify-between gap-4">
		<div class="flex min-w-0 flex-col gap-2">
			<span class="text-base-content/55 text-xs font-semibold tracking-[0.1em] uppercase"
				>{label}</span
			>
			<span class="text-3xl leading-none font-bold tracking-tight tabular-nums {valueClass}">
				{format(counter.current)}
			</span>
			{#if hint}
				<span class="text-base-content/50 truncate text-xs" title={hint}>{hint}</span>
			{/if}
		</div>
		{#if icon}
			<span class="grid size-10 shrink-0 place-items-center rounded-xl {TONES[tone].icon}">
				<Icon name={icon} class="size-5" />
			</span>
		{/if}
	</div>
</div>
