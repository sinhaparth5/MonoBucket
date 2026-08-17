<script lang="ts">
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { fly } from 'svelte/transition';
	import { api, ApiError, type Overview, type Sample, type Series } from '$lib/api';
	import {
		formatBytes,
		formatClock,
		formatCount,
		formatDuration,
		formatPercent,
		formatRate,
		plural
	} from '$lib/format';
	import SeriesChart from '$lib/components/SeriesChart.svelte';
	import ThroughputChart from '$lib/components/ThroughputChart.svelte';
	import StatTile from '$lib/components/StatTile.svelte';
	import Icon from '$lib/components/Icon.svelte';

	let overview = $state<Overview | null>(null);
	let series = $state<Series | null>(null);
	let error = $state('');
	let refreshing = $state(false);
	let updatedAt = $state<Date | null>(null);

	// Matches the server's sampling cadence. Polling faster would redraw the same
	// ring; polling slower would leave the newest sample on screen for longer
	// than it was current.
	const REFRESH_MS = 5000;

	// Shapes for the first paint, before the first response arrives.
	const TILE_PLACEHOLDERS = [0, 1, 2, 3];
	const CHART_PLACEHOLDERS = [0, 1, 2];

	async function refresh() {
		if (refreshing) return;
		refreshing = true;
		try {
			const [nextOverview, nextSeries] = await Promise.all([api.overview(), api.series()]);
			overview = nextOverview;
			series = nextSeries;
			error = '';
			updatedAt = new Date();
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			error = cause instanceof ApiError ? cause.message : 'could not refresh';
		} finally {
			refreshing = false;
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
	const throughput = $derived(
		samples.map((sample) => ({
			t: new Date(sample.atMs),
			ingress: rate(sample, sample.bytesIn),
			egress: rate(sample, sample.bytesOut)
		}))
	);
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

<div class="flex flex-col gap-6">
	<header
		class="bg-neutral text-neutral-content relative isolate min-h-72 overflow-hidden rounded-box shadow-xl"
	>
		<img
			src="/images/dashboard-fabric.webp"
			alt="Colorful data paths flowing into a luminous storage core"
			width="1600"
			height="593"
			class="absolute inset-0 size-full object-cover object-[68%_center] sm:object-center"
			fetchpriority="high"
			decoding="async"
		/>
		<div
			class="from-neutral via-neutral/90 to-neutral/35 absolute inset-0 bg-gradient-to-r sm:via-neutral/75 sm:to-transparent"
		></div>
		<div class="from-neutral/10 to-neutral/60 absolute inset-0 bg-gradient-to-b"></div>

		<div class="relative flex min-h-72 max-w-2xl flex-col justify-between gap-8 p-6 sm:p-8">
			<div class="flex max-w-xl flex-col items-start gap-3">
				<span
					class="badge border-neutral-content/15 bg-neutral-content/10 text-neutral-content gap-2 backdrop-blur"
				>
					<span class="bg-success size-1.5 rounded-full"></span>
					Live telemetry
				</span>
				<div>
					<h1 class="text-3xl font-bold tracking-tight sm:text-4xl">Storage, in motion.</h1>
					<p class="text-neutral-content/65 mt-2 max-w-lg text-sm leading-6 sm:text-base">
						A live view of traffic, memory, cache, and capacity across your MonoBucket instance.
					</p>
				</div>
			</div>

			<div class="flex flex-wrap items-end justify-between gap-4">
				<div class="flex flex-wrap items-center gap-2">
					{#if overview}
						<span
							class="badge badge-sm border-neutral-content/15 bg-neutral-content/10 text-neutral-content gap-1.5 font-mono backdrop-blur"
						>
							<span class="bg-success size-1.5 rounded-full"></span>
							up {formatDuration(overview.server.uptimeSeconds)}
						</span>
						<span
							class="badge badge-sm border-neutral-content/15 bg-neutral-content/10 text-neutral-content font-mono backdrop-blur"
							>{overview.storage.engine}</span
						>
						<span
							class="badge badge-sm border-neutral-content/15 bg-neutral-content/10 text-neutral-content font-mono backdrop-blur"
							>{overview.server.region}</span
						>
						<span
							class="badge badge-sm border-neutral-content/15 bg-neutral-content/10 text-neutral-content font-mono backdrop-blur"
						>
							:{overview.server.s3Port} · :{overview.server.consolePort}
						</span>
					{/if}
				</div>
				<div class="flex items-center gap-3">
					{#if updatedAt}
						<span class="text-neutral-content/50 text-xs" aria-live="polite">
							Updated {formatClock(updatedAt.getTime())}
						</span>
					{/if}
					<button class="btn btn-primary btn-sm gap-2" onclick={refresh} disabled={refreshing}>
						<Icon name="refresh" class="size-4 {refreshing ? 'animate-spin' : ''}" />
						Refresh
					</button>
				</div>
			</div>
		</div>
	</header>

	{#if error}
		<div role="alert" class="alert alert-error alert-soft" in:fly={{ y: -6, duration: 200 }}>
			<Icon name="warning" />
			<span>{error}</span>
		</div>
	{/if}

	{#if !overview}
		<div class="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
			{#each TILE_PLACEHOLDERS as index (index)}
				<div class="skeleton rounded-box h-24"></div>
			{/each}
		</div>
		<div class="skeleton rounded-box h-32"></div>
		<div class="grid gap-4 lg:grid-cols-3">
			{#each CHART_PLACEHOLDERS as index (index)}
				<div class="skeleton rounded-box h-72"></div>
			{/each}
		</div>
	{:else}
		<section class="grid gap-4 sm:grid-cols-2 xl:grid-cols-4" aria-label="Key storage metrics">
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

		<section
			class="panel surface-raised flex flex-col gap-4 p-5"
			aria-labelledby="disk-capacity-heading"
		>
			<div class="flex flex-wrap items-center justify-between gap-2">
				<h2
					id="disk-capacity-heading"
					class="text-base-content/70 flex items-center gap-1.5 text-xs font-medium tracking-wide uppercase"
				>
					<Icon name="disk" class="size-3.5" />
					Disk capacity
				</h2>
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

		<section class="grid gap-4 xl:grid-cols-3" aria-label="Traffic charts">
			<div class="xl:col-span-2">
				<ThroughputChart points={throughput} />
			</div>
			<SeriesChart
				label="Request rate"
				icon="overview"
				points={requestRate}
				headline={latest ? formatRate(rate(latest, latest.requests)) : '0/s'}
				hint="S3 listener"
				format={(value) => formatRate(value)}
				height="h-64 sm:h-72"
				featured
			/>
		</section>

		<section class="grid gap-4 md:grid-cols-2 xl:grid-cols-4" aria-label="Runtime charts">
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

		<section class="grid gap-4 lg:grid-cols-2">
			<div class="panel flex flex-col gap-4 p-5">
				<h2
					class="text-base-content/70 flex items-center gap-1.5 text-xs font-semibold tracking-wide uppercase"
				>
					<Icon name="overview" class="size-3.5" />
					Responses since start
				</h2>

				{#if mixTotal === 0}
					<div
						class="border-base-300 bg-base-200/35 grid min-h-36 overflow-hidden rounded-xl border border-dashed sm:grid-cols-[9rem_1fr]"
					>
						<img
							src="/images/telemetry-idle.webp"
							alt="A quiet telemetry beacon waiting for its first signal"
							width="560"
							height="560"
							class="h-36 w-full object-cover sm:h-full"
							loading="lazy"
							decoding="async"
						/>
						<div class="flex flex-col items-start justify-center gap-2 p-5">
							<span class="badge badge-primary badge-soft badge-sm gap-1.5">
								<span class="bg-primary size-1.5 rounded-full"></span>
								Listening
							</span>
							<div>
								<h3 class="font-semibold">Waiting for the first S3 request</h3>
								<p class="text-base-content/55 mt-1 text-sm leading-5">
									Response health will appear here as traffic reaches the listener.
								</p>
							</div>
						</div>
					</div>
				{:else}
					<div
						class="bg-base-300 flex h-3 w-full gap-0.5 overflow-hidden rounded-full"
						role="img"
						aria-label="Response mix: {responseMix
							.map((part) => `${part.label} ${formatCount(part.value)}`)
							.join(', ')}"
					>
						{#each responseMix as part (part.label)}
							{#if part.value > 0}
								<span
									class="{part.tone} h-full transition-all duration-500"
									style="width: {(part.value / mixTotal) * 100}%"
									title="{part.label}: {formatCount(part.value)}"
									aria-hidden="true"
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

			<div class="panel flex flex-col gap-4 p-5">
				<h2
					class="text-base-content/70 flex items-center gap-1.5 text-xs font-semibold tracking-wide uppercase"
				>
					<Icon name="disk" class="size-3.5" />
					Metadata engine
				</h2>
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
