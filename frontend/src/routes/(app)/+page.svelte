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

	let overview = $state<Overview | null>(null);
	let series = $state<Series | null>(null);
	let error = $state('');

	// Matches the server's sampling cadence. Polling faster would redraw the same
	// ring; polling slower would leave the newest sample on screen for longer
	// than it was current.
	const REFRESH_MS = 5000;

	// Shapes for the first paint, before the first response arrives.
	const TILE_PLACEHOLDERS = [0, 1, 2, 3];
	const CHART_PLACEHOLDERS = [0, 1];

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
			{ label: '2xx', value: s3.succeeded, tone: 'bg-success' },
			{ label: '4xx', value: s3.clientErrors, tone: 'bg-warning' },
			{ label: '5xx', value: s3.serverErrors, tone: 'bg-error' },
			{ label: 'shed', value: s3.shed, tone: 'bg-info' }
		];
	});

	const mixTotal = $derived(responseMix.reduce((sum, part) => sum + part.value, 0));
</script>

<svelte:head><title>Overview · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-5">
	<div class="flex flex-wrap items-baseline justify-between gap-3">
		<h1 class="text-2xl font-semibold">Overview</h1>
		{#if overview}
			<span class="text-base-content/60 text-xs">
				{overview.storage.engine} · {overview.server.region} · up
				{formatDuration(overview.server.uptimeSeconds)}
			</span>
		{/if}
	</div>

	{#if error}
		<div role="alert" class="alert alert-error alert-soft" in:fly={{ y: -6, duration: 200 }}>
			<span>{error}</span>
		</div>
	{/if}

	{#if !overview}
		<div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
			{#each TILE_PLACEHOLDERS as index (index)}
				<div class="skeleton rounded-box h-24"></div>
			{/each}
		</div>
		<div class="grid gap-3 lg:grid-cols-2">
			{#each CHART_PLACEHOLDERS as index (index)}
				<div class="skeleton rounded-box h-56"></div>
			{/each}
		</div>
	{:else}
		<section class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
			<StatTile
				label="Objects"
				value={overview.storage.objects}
				format={formatCount}
				hint={plural(overview.storage.buckets, 'bucket')}
			/>
			<StatTile
				label="Stored"
				value={overview.storage.bytes}
				format={formatBytes}
				hint="{plural(overview.storage.uploads, 'upload')} in progress"
			/>
			<StatTile
				label="Cache hit rate"
				value={overview.cache.hitRatio}
				format={(value) => formatPercent(value)}
				hint="{overview.cache.backend} · {formatBytes(overview.cache.bytes)} of {formatBytes(
					overview.cache.limitBytes
				)}"
				valueClass={overview.cache.healthy ? '' : 'text-warning'}
			/>
			<StatTile
				label="Resident memory"
				value={overview.server.residentBytes}
				format={formatBytes}
				hint="{overview.server.workerThreads} workers · {overview.io.threads} I/O threads"
			/>
		</section>

		<section class="border-base-300 bg-base-100 rounded-box flex flex-col gap-3 border p-4">
			<div class="flex flex-wrap items-baseline justify-between gap-2">
				<span class="text-base-content/70 text-xs font-medium tracking-wide uppercase">
					Disk capacity
				</span>
				<span class="text-base-content/60 text-xs">
					{formatBytes(overview.storage.diskTotalBytes - overview.storage.diskAvailableBytes)} used of
					{formatBytes(overview.storage.diskTotalBytes)}
				</span>
			</div>
			<progress
				class="progress {diskUsedFraction > 0.9
					? 'progress-error'
					: diskUsedFraction > 0.75
						? 'progress-warning'
						: 'progress-primary'} w-full"
				value={diskUsedFraction * 100}
				max="100"
			></progress>
			<div class="text-base-content/50 flex flex-wrap gap-x-6 gap-y-1 text-xs">
				<span>{formatBytes(overview.storage.bytes)} in object payloads</span>
				<span>{plural(overview.storage.orphanBlobs, 'payload')} awaiting reclamation</span>
				<span>{formatBytes(overview.storage.diskAvailableBytes)} available</span>
			</div>
		</section>

		<section class="grid gap-3 lg:grid-cols-2">
			<SeriesChart
				label="Request rate"
				points={requestRate}
				headline={latest ? formatRate(rate(latest, latest.requests)) : '0/s'}
				hint="S3 listener"
				format={(value) => formatRate(value)}
			/>
			<SeriesChart
				label="Bytes out"
				points={bytesOutRate}
				headline={latest ? `${formatBytes(rate(latest, latest.bytesOut))}/s` : '0 B/s'}
				hint="object payloads only"
				lineClass="stroke-info"
				areaClass="fill-info/15"
				format={(value) => formatBytes(value, 0)}
				minTop={1024}
			/>
			<SeriesChart
				label="Bytes in"
				points={bytesInRate}
				headline={latest ? `${formatBytes(rate(latest, latest.bytesIn))}/s` : '0 B/s'}
				hint="object payloads only"
				lineClass="stroke-accent"
				areaClass="fill-accent/15"
				format={(value) => formatBytes(value, 0)}
				minTop={1024}
			/>
			<SeriesChart
				label="Cache hit ratio"
				points={hitRatio}
				headline={formatPercent(overview.cache.hitRatio)}
				hint="per sample"
				lineClass="stroke-success"
				areaClass="fill-success/15"
				format={(value) => formatPercent(value, 0)}
			/>
			<SeriesChart
				label="Resident memory"
				points={resident}
				headline={formatBytes(overview.server.residentBytes)}
				hint="process RSS"
				lineClass="stroke-secondary"
				areaClass="fill-secondary/15"
				format={(value) => formatBytes(value, 0)}
				minTop={1024 * 1024}
			/>
			<SeriesChart
				label="Storage queue"
				points={queueDepth}
				headline={formatCount(overview.io.queued + overview.io.active)}
				hint="depth {overview.io.limit} before shedding"
				lineClass="stroke-warning"
				areaClass="fill-warning/15"
				format={(value) => formatCount(value)}
				integerTicks
			/>
		</section>

		<section class="grid gap-3 lg:grid-cols-2">
			<div class="border-base-300 bg-base-100 rounded-box flex flex-col gap-3 border p-4">
				<span class="text-base-content/70 text-xs font-medium tracking-wide uppercase">
					Responses since start
				</span>

				{#if mixTotal === 0}
					<p class="text-base-content/50 text-sm">No S3 requests yet.</p>
				{:else}
					<div class="bg-base-200 flex h-2 w-full overflow-hidden rounded-full">
						{#each responseMix as part (part.label)}
							{#if part.value > 0}
								<span
									class="{part.tone} h-full transition-all duration-500"
									style="width: {(part.value / mixTotal) * 100}%"
								></span>
							{/if}
						{/each}
					</div>
					<div class="grid grid-cols-2 gap-x-6 gap-y-2 sm:grid-cols-4">
						{#each responseMix as part (part.label)}
							<div class="flex flex-col">
								<span class="flex items-center gap-1.5 text-xs">
									<span class="{part.tone} size-2 rounded-full"></span>
									{part.label}
								</span>
								<span class="font-semibold tabular-nums">{formatCount(part.value)}</span>
							</div>
						{/each}
					</div>
				{/if}

				<div class="text-base-content/50 flex flex-wrap gap-x-6 gap-y-1 text-xs">
					<span>{formatCount(overview.s3.authFailures)} auth failures</span>
					<span>{formatCount(overview.s3.anonymous)} anonymous</span>
					<span
						>{formatBytes(overview.s3.bytesIn)} in / {formatBytes(overview.s3.bytesOut)} out</span
					>
				</div>
			</div>

			<div class="border-base-300 bg-base-100 rounded-box flex flex-col gap-3 border p-4">
				<span class="text-base-content/70 text-xs font-medium tracking-wide uppercase">
					Metadata engine
				</span>
				<div class="overflow-x-auto">
					<table class="table table-xs">
						<tbody>
							{#each Object.entries(overview.storage.engineGauges) as [name, value] (name)}
								<tr>
									<td class="font-mono text-xs">{name.replaceAll('_', ' ')}</td>
									<td class="text-right">{formatBytes(value)}</td>
								</tr>
							{/each}
							<tr>
								<td class="font-mono text-xs">io completed</td>
								<td class="text-right">{formatCount(overview.io.completed)}</td>
							</tr>
							<tr>
								<td class="font-mono text-xs">io rejected</td>
								<td class="text-right {overview.io.rejected > 0 ? 'text-warning' : ''}">
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
