<script lang="ts">
	// One metric over the sampling window. Every graph on the overview is this
	// component with different accessors, so axis formatting, tooltip layout and
	// the empty state are decided once.
	import { Area, Axis, Chart, Highlight, Svg, Tooltip } from 'layerchart';
	import { fade } from 'svelte/transition';
	import { formatClock } from '$lib/format';
	import Icon, { type IconName } from './Icon.svelte';

	interface Point {
		t: Date;
		v: number;
	}

	interface Props {
		points: Point[];
		label: string;
		/// Rendered above the chart. Already formatted — the caller knows whether
		/// its metric is bytes, a rate or a ratio.
		headline?: string;
		hint?: string;
		/// Literal Tailwind classes, not a colour name: the JIT only emits classes
		/// it can see in source, and a name assembled at runtime is invisible to it.
		lineClass?: string;
		areaClass?: string;
		format?: (value: number) => string;
		/// Smallest top of the y axis. An idle metric otherwise scales to a domain
		/// of [0, 1], where four ticks all format to the same string and the axis
		/// reads as broken. Set it to the granularity the metric is measured in —
		/// 1024 for a byte rate, 1 for a count.
		minTop?: number;
		/// For metrics that only take whole values. Evenly spaced fractional ticks
		/// would render as "0 1 1 2 2" once the format rounds them.
		integerTicks?: boolean;
		height?: string;
		icon?: IconName;
		/// Tints the label to match the series. The line already carries the
		/// colour; repeating it on the heading is what lets six charts on one
		/// screen be told apart from across a desk.
		accentClass?: string;
		featured?: boolean;
	}

	let {
		points,
		label,
		headline,
		hint,
		icon,
		accentClass = 'text-primary',
		lineClass = 'stroke-primary',
		areaClass = 'fill-primary/15',
		format = (value: number) => String(Math.round(value)),
		minTop = 1,
		integerTicks = false,
		height = 'h-44',
		featured = false
	}: Props = $props();

	// A single point has no line to draw and an axis with one tick, which reads
	// as a broken chart rather than an empty one.
	const ready = $derived(points.length >= 2);

	const top = $derived(Math.max(minTop, ...points.map((point) => point.v)));

	const ticks = $derived.by(() => {
		if (!integerTicks) return 4;
		const max = Math.ceil(top);
		const step = Math.max(1, Math.ceil(max / 4));
		const values: number[] = [];
		for (let value = 0; value <= max; value += step) values.push(value);
		return values;
	});

	const recent = $derived(points.slice(-12));
</script>

<div class="panel chart-panel interactive-card flex flex-col gap-2 p-5 {featured ? 'lg:p-6' : ''}">
	<div class="flex items-center justify-between gap-3">
		<span class="text-base-content flex items-center gap-2 text-sm font-semibold">
			{#if icon}
				<span class="bg-base-200 grid size-8 place-items-center rounded-lg {accentClass}">
					<Icon name={icon} class="size-4" />
				</span>
			{/if}
			{label}
		</span>
		{#if hint}
			<span class="badge badge-ghost badge-sm text-base-content/55">{hint}</span>
		{/if}
	</div>

	{#if headline}
		<span class="text-2xl font-bold tracking-tight tabular-nums {featured ? 'sm:text-3xl' : ''}">
			{headline}
		</span>
	{/if}

	<div
		class="{height} mt-1"
		role="img"
		aria-label="{label} chart with {points.length} samples. Latest value: {headline ??
			'unavailable'}."
	>
		{#if ready}
			<div class="h-full w-full" in:fade={{ duration: 200 }}>
				<Chart
					data={points}
					x="t"
					y="v"
					yDomain={[0, top]}
					yNice
					padding={{ left: 48, bottom: 22, top: 8, right: 8 }}
					tooltipContext={{ mode: 'bisect-x' }}
				>
					<Svg>
						<Axis
							placement="left"
							grid={{ class: 'stroke-base-300/70' }}
							{ticks}
							format={(value: number) => format(value)}
							tickLabelProps={{ class: 'fill-base-content/50 text-[10px]' }}
							rule={false}
						/>
						<Axis
							placement="bottom"
							ticks={4}
							format={(value: Date) => formatClock(value.getTime())}
							tickLabelProps={{ class: 'fill-base-content/50 text-[10px]' }}
							rule={{ class: 'stroke-base-300' }}
						/>
						<Area line={{ class: `${lineClass} stroke-2` }} class={areaClass} />
						<Highlight points lines={{ class: 'stroke-base-content/30' }} />
					</Svg>

					<Tooltip.Root>
						{#snippet children({ data }: { data: Point })}
							<Tooltip.Header>{formatClock(data.t.getTime())}</Tooltip.Header>
							<Tooltip.List>
								<Tooltip.Item {label} value={format(data.v)} />
							</Tooltip.List>
						{/snippet}
					</Tooltip.Root>
				</Chart>
			</div>
		{:else}
			<div class="text-base-content/40 flex h-full items-center justify-center gap-2 text-xs">
				<span class="loading loading-dots loading-sm"></span>
				collecting samples
			</div>
		{/if}
	</div>

	{#if recent.length > 0}
		<details class="text-base-content/55 group text-xs">
			<summary class="hover:text-primary cursor-pointer list-none font-medium">
				<span class="group-open:hidden">View recent values</span>
				<span class="hidden group-open:inline">Hide recent values</span>
			</summary>
			<div class="mt-2 max-h-36 overflow-auto rounded-lg border border-base-300">
				<table class="table table-xs">
					<thead><tr><th>Time</th><th class="text-right">{label}</th></tr></thead>
					<tbody>
						{#each recent as point (point.t.getTime())}
							<tr>
								<td>{formatClock(point.t.getTime())}</td>
								<td class="text-right font-mono">{format(point.v)}</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		</details>
	{/if}
</div>
