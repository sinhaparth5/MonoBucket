<script lang="ts">
	import { Area, Axis, Chart, Highlight, Spline, Svg, Tooltip } from 'layerchart';
	import { fade } from 'svelte/transition';
	import { formatBytes, formatClock } from '$lib/format';
	import Icon from './Icon.svelte';

	interface Point {
		t: Date;
		ingress: number;
		egress: number;
	}

	interface Props {
		points: Point[];
	}

	let { points }: Props = $props();

	const ready = $derived(points.length >= 2);
	const latest = $derived(points.at(-1));
	const top = $derived(Math.max(1024, ...points.flatMap((point) => [point.ingress, point.egress])));
	const recent = $derived(points.slice(-12));
	const series = [
		{ key: 'egress', label: 'Egress', value: 'egress', color: 'var(--color-secondary)' },
		{ key: 'ingress', label: 'Ingress', value: 'ingress', color: 'var(--color-accent)' }
	];

	function rate(value: number): string {
		return `${formatBytes(value, 0)}/s`;
	}
</script>

<section class="panel chart-panel interactive-card flex flex-col gap-4 p-5 lg:p-6">
	<header class="flex flex-wrap items-start justify-between gap-4">
		<div class="flex items-center gap-3">
			<span class="bg-secondary/10 text-secondary grid size-10 place-items-center rounded-xl">
				<Icon name="upload" class="size-5" />
			</span>
			<div>
				<h2 class="font-semibold">Data throughput</h2>
				<p class="text-base-content/50 text-xs">Object payloads over the last twenty minutes</p>
			</div>
		</div>

		<div class="flex gap-5 text-right">
			<div>
				<div class="text-base-content/50 flex items-center justify-end gap-1.5 text-xs">
					<span class="bg-secondary size-2 rounded-full"></span>
					Egress
				</div>
				<div class="font-mono text-sm font-semibold">{latest ? rate(latest.egress) : '0 B/s'}</div>
			</div>
			<div>
				<div class="text-base-content/50 flex items-center justify-end gap-1.5 text-xs">
					<span class="border-accent size-2 rounded-full border-2"></span>
					Ingress
				</div>
				<div class="font-mono text-sm font-semibold">{latest ? rate(latest.ingress) : '0 B/s'}</div>
			</div>
		</div>
	</header>

	<div
		class="h-64 sm:h-72"
		role="img"
		aria-label="Data throughput chart with {points.length} samples. Latest egress is {latest
			? rate(latest.egress)
			: 'unavailable'} and latest ingress is {latest ? rate(latest.ingress) : 'unavailable'}."
	>
		{#if ready}
			<div class="h-full w-full" in:fade={{ duration: 200 }}>
				<Chart
					data={points}
					x="t"
					{series}
					yDomain={[0, top]}
					yNice
					padding={{ left: 52, bottom: 24, top: 12, right: 10 }}
					tooltipContext={{ mode: 'bisect-x' }}
				>
					<Svg>
						<Axis
							placement="left"
							grid={{ class: 'stroke-base-300/70' }}
							ticks={4}
							format={(value: number) => formatBytes(value, 0)}
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
						<Area seriesKey="egress" class="fill-secondary/10" />
						<Spline seriesKey="egress" class="stroke-secondary stroke-[2.5]" fill="none" />
						<Spline
							seriesKey="ingress"
							class="stroke-accent stroke-2"
							fill="none"
							stroke-dasharray="7 5"
						/>
						<Highlight points lines={{ class: 'stroke-base-content/25' }} />
					</Svg>

					<Tooltip.Root>
						{#snippet children({ data }: { data: Point })}
							<Tooltip.Header>{formatClock(data.t.getTime())}</Tooltip.Header>
							<Tooltip.List>
								<Tooltip.Item label="Egress" value={rate(data.egress)} />
								<Tooltip.Item label="Ingress" value={rate(data.ingress)} />
							</Tooltip.List>
						{/snippet}
					</Tooltip.Root>
				</Chart>
			</div>
		{:else}
			<div class="text-base-content/40 flex h-full items-center justify-center gap-2 text-xs">
				<span class="loading loading-dots loading-sm"></span>
				Collecting throughput samples
			</div>
		{/if}
	</div>

	{#if recent.length > 0}
		<details class="text-base-content/55 group text-xs">
			<summary class="hover:text-primary cursor-pointer list-none font-medium">
				<span class="group-open:hidden">View recent values</span>
				<span class="hidden group-open:inline">Hide recent values</span>
			</summary>
			<div class="mt-2 max-h-40 overflow-auto rounded-lg border border-base-300">
				<table class="table table-xs">
					<thead
						><tr
							><th>Time</th><th class="text-right">Ingress</th><th class="text-right">Egress</th
							></tr
						></thead
					>
					<tbody>
						{#each recent as point (point.t.getTime())}
							<tr>
								<td>{formatClock(point.t.getTime())}</td>
								<td class="text-right font-mono">{rate(point.ingress)}</td>
								<td class="text-right font-mono">{rate(point.egress)}</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		</details>
	{/if}
</section>
