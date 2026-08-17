<script lang="ts">
	// Reference page for the console's visual language. It is not part of the
	// dashboard; it exists so the theme, type scale and component sizing can be
	// judged side by side in both themes before pages are built on top of them.

	const scale = [
		{ cls: 'text-3xl font-semibold', name: 'text-3xl', px: '30px', use: 'stat values' },
		{ cls: 'text-2xl font-semibold', name: 'text-2xl', px: '24px', use: 'page titles' },
		{ cls: 'text-xl font-semibold', name: 'text-xl', px: '20px', use: 'section headings' },
		{ cls: 'text-lg font-medium', name: 'text-lg', px: '18px', use: 'card and panel titles' },
		{ cls: 'text-base', name: 'text-base', px: '16px', use: 'emphasised body' },
		{ cls: 'text-sm', name: 'text-sm', px: '14px', use: 'body and tables — the default' },
		{ cls: 'text-xs', name: 'text-xs', px: '12px', use: 'captions, timestamps, meta' }
	];

	const roles = [
		{ token: 'base-100', swatch: 'bg-base-100', note: 'page background' },
		{ token: 'base-200', swatch: 'bg-base-200', note: 'panels, table headers' },
		{ token: 'base-300', swatch: 'bg-base-300', note: 'borders, wells' },
		{ token: 'primary', swatch: 'bg-primary', note: 'one action per page' },
		{ token: 'neutral', swatch: 'bg-neutral', note: 'secondary actions' },
		{ token: 'info', swatch: 'bg-info', note: 'cache hits, notices' },
		{ token: 'success', swatch: 'bg-success', note: '2xx, healthy' },
		{ token: 'warning', swatch: 'bg-warning', note: 'SlowDown, degraded' },
		{ token: 'error', swatch: 'bg-error', note: '5xx, destructive' }
	];

	const objects = [
		{ key: 'backups/2026-08-16/pg-dump.tar.zst', size: '4.2 GiB', storage: '3 parts', code: 200 },
		{ key: 'media/originals/IMG_4417.heic', size: '12.8 MiB', storage: 'single', code: 200 },
		{ key: 'logs/ingest/2026-08-17T09.jsonl', size: '0 B', storage: 'single', code: 200 },
		{ key: 'tmp/upload-9f3a2c.part', size: '128.0 MiB', storage: 'in flight', code: 503 }
	];
</script>

<svelte:head><title>Design reference · MonoBucket</title></svelte:head>

<div class="bg-base-100 text-base-content min-h-screen">
	<header class="border-base-300 bg-base-100 sticky top-0 z-10 border-b">
		<div class="mx-auto flex max-w-5xl items-center gap-4 px-6 py-3">
			<span class="text-lg font-semibold">MonoBucket</span>
			<span class="text-base-content/60 text-xs">design reference</span>
			<div class="join ml-auto">
				<input
					type="radio"
					name="console-theme"
					value="corporate"
					aria-label="Light"
					class="theme-controller join-item btn btn-sm"
				/>
				<input
					type="radio"
					name="console-theme"
					value="night"
					aria-label="Dark"
					class="theme-controller join-item btn btn-sm"
				/>
			</div>
		</div>
	</header>

	<main class="mx-auto flex max-w-5xl flex-col gap-10 px-6 py-8">
		<section class="flex flex-col gap-3">
			<h1 class="text-2xl font-semibold">Type scale</h1>
			<p class="text-base-content/70 max-w-prose">
				Inter Variable, latin subset, served from the binary. Body text is 14px so listings stay
				scannable; the root stays at 16px so spacing utilities keep their documented values.
			</p>
			<div class="border-base-300 divide-base-300 divide-y rounded-box border">
				{#each scale as step (step.name)}
					<div class="flex flex-wrap items-baseline gap-x-6 gap-y-1 px-4 py-3">
						<span class="text-base-content/60 font-mono text-xs w-24">{step.name}</span>
						<span class="text-base-content/60 text-xs w-12">{step.px}</span>
						<span class={step.cls}>Objects stored 1,284,905</span>
						<span class="text-base-content/60 ml-auto text-xs">{step.use}</span>
					</div>
				{/each}
			</div>
		</section>

		<section class="flex flex-col gap-3">
			<h2 class="text-xl font-semibold">Colour roles</h2>
			<p class="text-base-content/70 max-w-prose">
				Semantic tokens only. A literal Tailwind colour keeps its value when the theme flips and
				turns unreadable in the other one.
			</p>
			<div class="grid grid-cols-2 gap-3 sm:grid-cols-3">
				{#each roles as role (role.token)}
					<div class="border-base-300 flex items-center gap-3 rounded-box border p-3">
						<span class="border-base-300 size-8 shrink-0 rounded border {role.swatch}"></span>
						<span class="flex flex-col">
							<span class="font-mono text-xs">{role.token}</span>
							<span class="text-base-content/60 text-xs">{role.note}</span>
						</span>
					</div>
				{/each}
			</div>
		</section>

		<section class="flex flex-col gap-3">
			<h2 class="text-xl font-semibold">Numbers</h2>
			<div class="stats border-base-300 w-full border">
				<div class="stat">
					<div class="stat-title">Objects</div>
					<div class="stat-value">1,284,905</div>
					<div class="stat-desc">across 12 buckets</div>
				</div>
				<div class="stat">
					<div class="stat-title">Stored</div>
					<div class="stat-value">8.42 TiB</div>
					<div class="stat-desc">of 16.00 TiB capacity</div>
				</div>
				<div class="stat">
					<div class="stat-title">Cache hit rate</div>
					<div class="stat-value">94.7%</div>
					<div class="stat-desc">last 5 minutes</div>
				</div>
			</div>
		</section>

		<section class="flex flex-col gap-3">
			<h2 class="text-xl font-semibold">Listing</h2>
			<div class="border-base-300 overflow-x-auto rounded-box border">
				<table class="table table-sm">
					<thead>
						<tr>
							<th>Key</th>
							<th>Size</th>
							<th>Storage</th>
							<th>Status</th>
						</tr>
					</thead>
					<tbody>
						{#each objects as object (object.key)}
							<tr>
								<td class="font-mono text-xs">{object.key}</td>
								<td>{object.size}</td>
								<td class="text-base-content/70">{object.storage}</td>
								<td>
									<span
										class="badge badge-sm {object.code >= 500
											? 'badge-warning'
											: 'badge-success'} badge-soft">{object.code}</span
									>
								</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		</section>

		<section class="flex flex-col gap-3">
			<h2 class="text-xl font-semibold">Controls</h2>
			<div class="flex flex-wrap items-center gap-2">
				<button class="btn btn-primary btn-sm">Create bucket</button>
				<button class="btn btn-sm">Upload</button>
				<button class="btn btn-sm btn-ghost">Cancel</button>
				<button class="btn btn-sm btn-error btn-outline">Delete</button>
			</div>
			<div role="alert" class="alert alert-warning alert-soft">
				<span>Queue depth at 92% — new uploads will be answered with 503 SlowDown.</span>
			</div>
		</section>
	</main>
</div>
