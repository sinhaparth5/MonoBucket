<script lang="ts">
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { fly } from 'svelte/transition';
	import { api, ApiError, type Overview, type Sample, type Series } from '$lib/api';
	import {
		formatBytes,
		formatCount,
		formatDuration,
		formatPercent,
		formatRate,
		plural
	} from '$lib/format';
	import SeriesChart from '$lib/components/SeriesChart.svelte';
	import StatTile from '$lib/components/StatTile.svelte';
	import Icon from '$lib/components/Icon.svelte';

	let overview = $state<Overview | null>(null);
	let series = $state<Series | null>(null);
	let error = $state('');

	// Matches the server's sampling cadence. Polling faster would redraw the same
	// ring; polling slower would leave the newest sample on screen for longer
	// than it was current.
	const REFRESH_MS = 5000;

	// Shapes for the first paint, before the first response arrives.
	const TILE_PLACEHOLDERS = [0, 1, 2, 3];
	const CHART_PLACEHOLDERS = [0, 1, 2, 3];

	async function refresh() {
		try {
			const [nextOverview, nextSeries] = await Promise.all([api.overview(), api.series()]);
			overview = nextOverview;
			series = nextSeries;
			error = '';
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			error = cause instanceof ApiError ? cause.message : 'could not refresh';
		}
	}

	$effect(() => {
		refresh();
		const timer = setInterval(refresh, REFRESH_MS);
		return () => clearInterval(timer);
	});

	const samples = $derived(series?.samples ?? []);

	/// Counters arrive as totals for the interval; a rate is what a graph should
	/// show, and the interval is per-sample rather than assumed because a skipped
	/// tick makes a wider span.
	function rate(sample: Sample, value: number): number {
		return sample.spanMs > 0 ? (value * 1000) / sample.spanMs : 0;
	}

	function points(pick: (sample: Sample) => number) {
		return samples.map((sample) => ({ t: new Date(sample.atMs), v: pick(sample) }));
	}

	const requestRate = $derived(points((s) => rate(s, s.requests)));
	const bytesOutRate = $derived(points((s) => rate(s, s.bytesOut)));
	const bytesInRate = $derived(points((s) => rate(s, s.bytesIn)));
	const resident = $derived(points((s) => s.residentBytes));
	const queueDepth = $derived(points((s) => s.ioQueued + s.ioActive));
	const connections = $derived(points((s) => s.connections));
	const hitRatio = $derived(
		points((s) => {
			const lookups = s.cacheHits + s.cacheMisses;
			return lookups > 0 ? s.cacheHits / lookups : 0;
		})
	);

	const latest = $derived(samples.at(-1));

	const diskUsedFraction = $derived.by(() => {
		const disk = overview?.storage;
		if (!disk || disk.diskTotalBytes === 0) return 0;
		return (disk.diskTotalBytes - disk.diskAvailableBytes) / disk.diskTotalBytes;
	});

	// The response mix is a breakdown, not a trend: four counters that should add
	// up to the request count, where any non-zero in the last two is the story.
	const responseMix = $derived.by(() => {
		const s3 = overview?.s3;
		if (!s3) return [];
		return [
			{ label: '2xx', value: s3.succeeded, tone: 'bg-success', text: 'text-success' },
			{ label: '4xx', value: s3.clientErrors, tone: 'bg-warning', text: 'text-warning' },
			{ label: '5xx', value: s3.serverErrors, tone: 'bg-error', text: 'text-error' },
			{ label: 'shed', value: s3.shed, tone: 'bg-info', text: 'text-info' }
		];
	});

	const mixTotal = $derived(responseMix.reduce((sum, part) => sum + part.value, 0));
</script>

<svelte:head><title>Overview · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-5">
	<div class="flex flex-wrap items-center justify-between gap-3">
		<div class="flex flex-col gap-0.5">
			<h1 class="text-2xl font-semibold tracking-tight">Overview</h1>
			<p class="text-base-content/55 text-xs">
				Sampled every {REFRESH_MS / 1000}s, twenty minutes of history held in memory.
			</p>
		</div>
		{#if overview}
			<div class="flex flex-wrap items-center gap-2">
				<span class="badge badge-sm badge-ghost gap-1.5 font-mono">
					<span class="bg-success size-1.5 rounded-full"></span>
					up {formatDuration(overview.server.uptimeSeconds)}
				</span>
				<span class="badge badge-sm badge-ghost font-mono">{overview.storage.engine}</span>
				<span class="badge badge-sm badge-ghost font-mono">{overview.server.region}</span>
				<span class="badge badge-sm badge-ghost font-mono">
					:{overview.server.s3Port} · :{overview.server.consolePort}
				</span>
			</div>
		{/if}
	</div>

	{#if error}
		<div role="alert" class="alert alert-error alert-soft" in:fly={{ y: -6, duration: 200 }}>
			<Icon name="warning" />
			<span>{error}</span>
		</div>
	{/if}

	{#if !overview}
		<div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
			{#each TILE_PLACEHOLDERS as index (index)}
				<div class="skeleton rounded-box h-24"></div>
			{/each}
		</div>
		<div class="skeleton rounded-box h-24"></div>
		<div class="grid gap-3 lg:grid-cols-2 2xl:grid-cols-3">
			{#each CHART_PLACEHOLDERS as index (index)}
				<div class="skeleton rounded-box h-56"></div>
			{/each}
		</div>
	{:else}
		<section class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
			<StatTile
				label="Objects"
				icon="file"
				tone="primary"
				value={overview.storage.objects}
				format={formatCount}
				hint="{plural(overview.storage.buckets, 'bucket')} · {formatBytes(overview.storage.bytes)}"
			/>
			<StatTile
				label="Connections"
				icon="plug"
				tone="secondary"
				value={overview.server.connections}
				format={formatCount}
				hint="all listeners · {overview.server.workerThreads} workers"
			/>
			<StatTile
				label="Cache hit rate"
				icon="cache"
				tone={overview.cache.healthy ? 'accent' : 'warning'}
				value={overview.cache.hitRatio}
				format={(value) => formatPercent(value)}
				hint="{overview.cache.backend} · {formatBytes(overview.cache.bytes)} of {formatBytes(
					overview.cache.limitBytes
				)}"
				valueClass={overview.cache.healthy ? '' : 'text-warning'}
			/>
			<StatTile
				label="Resident memory"
				icon="memory"
				tone="info"
				value={overview.server.residentBytes}
				format={formatBytes}
				hint="{overview.io.threads} I/O threads · {plural(
					overview.storage.uploads,
					'upload'
				)} in flight"
			/>
		</section>

		<section class="panel flex flex-col gap-3 p-4">
			<div class="flex flex-wrap items-center justify-between gap-2">
				<span
					class="text-base-content/70 flex items-center gap-1.5 text-xs font-medium tracking-wide uppercase"
				>
					<Icon name="disk" class="size-3.5" />
					Disk capacity
				</span>
				<span class="text-base-content/60 text-xs tabular-nums">
					{formatBytes(overview.storage.diskTotalBytes - overview.storage.diskAvailableBytes)} used of
					{formatBytes(overview.storage.diskTotalBytes)}
					<span class="text-base-content/40">({formatPercent(diskUsedFraction, 0)})</span>
				</span>
			</div>
			<progress
				class="progress {diskUsedFraction > 0.9
					? 'progress-error'
					: diskUsedFraction > 0.75
						? 'progress-warning'
						: 'progress-primary'} h-2 w-full"
				value={diskUsedFraction * 100}
				max="100"
			></progress>
			<div class="text-base-content/50 flex flex-wrap gap-x-6 gap-y-1 text-xs">
				<span>{formatBytes(overview.storage.bytes)} in object payloads</span>
				<span
					class={overview.storage.orphanBlobs > 0 ? 'text-warning' : ''}
					title="A number that only climbs means reclamation has stalled."
				>
					{plural(overview.storage.orphanBlobs, 'payload')} awaiting reclamation
				</span>
				<span>{formatBytes(overview.storage.diskAvailableBytes)} available</span>
			</div>
		</section>

		<section class="grid gap-3 lg:grid-cols-2 2xl:grid-cols-3">
			<SeriesChart
				label="Request rate"
				icon="overview"
				points={requestRate}
				headline={latest ? formatRate(rate(latest, latest.requests)) : '0/s'}
				hint="S3 listener"
				format={(value) => formatRate(value)}
			/>
			<SeriesChart
				label="Bytes out"
				icon="upload"
				points={bytesOutRate}
				headline={latest ? `${formatBytes(rate(latest, latest.bytesOut))}/s` : '0 B/s'}
				hint="object payloads only"
				accentClass="text-info"
				lineClass="stroke-info"
				areaClass="fill-info/15"
				format={(value) => formatBytes(value, 0)}
				minTop={1024}
			/>
			<SeriesChart
				label="Bytes in"
				icon="upload"
				points={bytesInRate}
				headline={latest ? `${formatBytes(rate(latest, latest.bytesIn))}/s` : '0 B/s'}
				hint="object payloads only"
				accentClass="text-accent"
				lineClass="stroke-accent"
				areaClass="fill-accent/15"
				format={(value) => formatBytes(value, 0)}
				minTop={1024}
			/>
			<SeriesChart
				label="Cache hit ratio"
				icon="cache"
				points={hitRatio}
				headline={formatPercent(overview.cache.hitRatio)}
				hint="per sample"
				accentClass="text-success"
				lineClass="stroke-success"
				areaClass="fill-success/15"
				format={(value) => formatPercent(value, 0)}
			/>
			<SeriesChart
				label="Resident memory"
				icon="memory"
				points={resident}
				headline={formatBytes(overview.server.residentBytes)}
				hint="process RSS"
				accentClass="text-secondary"
				lineClass="stroke-secondary"
				areaClass="fill-secondary/15"
				format={(value) => formatBytes(value, 0)}
				minTop={1024 * 1024}
			/>
			<SeriesChart
				label="Connections"
				icon="plug"
				points={connections}
				headline={formatCount(overview.server.connections)}
				hint="all listeners"
				accentClass="text-primary"
				lineClass="stroke-primary"
				areaClass="fill-primary/15"
				format={(value) => formatCount(value)}
				integerTicks
			/>
			<SeriesChart
				label="Storage queue"
				icon="disk"
				points={queueDepth}
				headline={formatCount(overview.io.queued + overview.io.active)}
				hint="depth {overview.io.limit} before shedding"
				accentClass="text-warning"
				lineClass="stroke-warning"
				areaClass="fill-warning/15"
				format={(value) => formatCount(value)}
				integerTicks
			/>
		</section>

		<section class="grid gap-3 lg:grid-cols-2">
			<div class="panel flex flex-col gap-3 p-4">
				<span
					class="text-base-content/70 flex items-center gap-1.5 text-xs font-medium tracking-wide uppercase"
				>
					<Icon name="overview" class="size-3.5" />
					Responses since start
				</span>

				{#if mixTotal === 0}
					<div
						class="border-base-300 text-base-content/50 flex flex-col items-center gap-1 rounded-lg border border-dashed py-6 text-sm"
					>
						<Icon name="overview" class="size-5 opacity-40" />
						No S3 requests yet.
					</div>
				{:else}
					<div class="bg-base-300 flex h-2.5 w-full gap-0.5 overflow-hidden rounded-full">
						{#each responseMix as part (part.label)}
							{#if part.value > 0}
								<span
									class="{part.tone} h-full transition-all duration-500"
									style="width: {(part.value / mixTotal) * 100}%"
									title="{part.label}: {formatCount(part.value)}"
								></span>
							{/if}
						{/each}
					</div>
					<div class="grid grid-cols-2 gap-x-6 gap-y-2 sm:grid-cols-4">
						{#each responseMix as part (part.label)}
							<div class="flex flex-col">
								<span class="text-base-content/60 flex items-center gap-1.5 text-xs">
									<span class="{part.tone} size-2 rounded-full"></span>
									{part.label}
								</span>
								<span class="text-lg font-semibold tabular-nums {part.value > 0 ? part.text : ''}">
									{formatCount(part.value)}
								</span>
							</div>
						{/each}
					</div>
				{/if}

				<div
					class="border-base-300 text-base-content/50 flex flex-wrap gap-x-6 gap-y-1 border-t pt-3 text-xs"
				>
					<span>{formatCount(overview.s3.authFailures)} auth failures</span>
					<span>{formatCount(overview.s3.anonymous)} anonymous</span>
					<span>
						{formatBytes(overview.s3.bytesIn)} in / {formatBytes(overview.s3.bytesOut)} out
					</span>
				</div>
			</div>

			<div class="panel flex flex-col gap-3 p-4">
				<span
					class="text-base-content/70 flex items-center gap-1.5 text-xs font-medium tracking-wide uppercase"
				>
					<Icon name="disk" class="size-3.5" />
					Metadata engine
				</span>
				<div class="overflow-x-auto">
					<table class="table table-xs">
						<tbody>
							{#each Object.entries(overview.storage.engineGauges) as [name, value] (name)}
								<tr class="hover:bg-base-200/60">
									<td class="text-base-content/70 font-mono text-xs">
										{name.replaceAll('_', ' ')}
									</td>
									<td class="text-right font-medium">{formatBytes(value)}</td>
								</tr>
							{/each}
							<tr class="hover:bg-base-200/60">
								<td class="text-base-content/70 font-mono text-xs">io completed</td>
								<td class="text-right font-medium">{formatCount(overview.io.completed)}</td>
							</tr>
							<tr class="hover:bg-base-200/60">
								<td class="text-base-content/70 font-mono text-xs">io rejected</td>
								<td class="text-right font-medium {overview.io.rejected > 0 ? 'text-warning' : ''}">
									{formatCount(overview.io.rejected)}
								</td>
							</tr>
						</tbody>
					</table>
				</div>
			</div>
		</section>
	{/if}
</div>
