<script lang="ts">
	import { goto } from '$app/navigation';
	import { page } from '$app/state';
	import { resolve } from '$app/paths';
	import { fade, fly } from 'svelte/transition';
	import { api, ApiError, type ObjectDetail, type Presigned, type StoredObject } from '$lib/api';
	import { formatBytes, formatTimestamp, leafName, plural } from '$lib/format';
	import { copyText, objectUrl, s3Endpoint } from '$lib/s3url';

	let { data } = $props();

	// S3 has no directories. `/` as the delimiter is what turns a flat keyspace
	// into something a person can walk, and the server rolls the keys up for us
	// so the browser never holds more than one level.
	const DELIMITER = '/';
	const PAGE_SIZE = 200;

	const bucket = $derived(page.params.name ?? '');
	const prefix = $derived(page.url.searchParams.get('prefix') ?? '');

	let prefixes = $state<string[]>([]);
	let objects = $state<StoredObject[]>([]);
	let nextAfter = $state('');
	let truncated = $state(false);
	let loading = $state(true);
	let loadingMore = $state(false);
	let error = $state('');

	let detail = $state<ObjectDetail | null>(null);
	let detailError = $state('');
	let detailDialog: HTMLDialogElement;

	let pendingDelete = $state<StoredObject | null>(null);
	let deleteDialog: HTMLDialogElement;

	// Whether this bucket's objects answer an unsigned GET. It changes what the
	// URL below actually does, so it is shown next to it rather than left for
	// someone to discover with a 403.
	let publicRead = $state(false);

	let urlField = $state<HTMLInputElement | undefined>();
	let copied = $state(false);

	const detailUrl = $derived(detail ? objectUrl(data.session, bucket, detail.key) : '');

	async function copyUrl() {
		copied = await copyText(detailUrl, urlField);
		setTimeout(() => (copied = false), 1500);
	}

	// Four choices rather than a number field: the server's range is 1 second to
	// 7 days, and nobody wants a link that lives for 41 minutes.
	const EXPIRY_CHOICES = [
		{ label: '15 minutes', seconds: 900 },
		{ label: '1 hour', seconds: 3600 },
		{ label: '24 hours', seconds: 86400 },
		{ label: '7 days', seconds: 604800 }
	];

	let expirySeconds = $state(3600);
	let signing = $state(false);
	let signed = $state<Presigned | null>(null);
	let signError = $state('');
	let signedField = $state<HTMLInputElement | undefined>();
	let copiedSigned = $state(false);

	async function generateLink() {
		if (!detail) return;
		signing = true;
		signError = '';
		try {
			signed = await api.presign({
				bucket,
				key: detail.key,
				// The endpoint the console itself reached, because the host is
				// inside the signature and the server's own is usually 0.0.0.0.
				...s3Endpoint(data.session, bucket),
				expiresSeconds: expirySeconds
			});
		} catch (cause) {
			signError = cause instanceof ApiError ? cause.message : 'could not sign a link';
		} finally {
			signing = false;
		}
	}

	async function copySigned() {
		if (!signed) return;
		copiedSigned = await copyText(signed.url, signedField);
		setTimeout(() => (copiedSigned = false), 1500);
	}

	async function loadFirstPage(currentBucket: string, currentPrefix: string) {
		loading = true;
		try {
			const result = await api.objects({
				bucket: currentBucket,
				prefix: currentPrefix,
				delimiter: DELIMITER,
				limit: PAGE_SIZE
			});
			prefixes = result.prefixes;
			objects = result.objects;
			publicRead = result.publicRead;
			truncated = result.truncated;
			nextAfter = result.nextAfter;
			error = '';
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			prefixes = [];
			objects = [];
			truncated = false;
			error = cause instanceof ApiError ? cause.message : 'could not list objects';
		} finally {
			loading = false;
		}
	}

	// Reruns whenever the bucket or the prefix changes, which is how walking into
	// a folder works: navigation rewrites the query and this reacts to it.
	$effect(() => {
		loadFirstPage(bucket, prefix);
	});

	async function loadMore() {
		if (!truncated || loadingMore) return;
		loadingMore = true;
		try {
			const result = await api.objects({
				bucket,
				prefix,
				delimiter: DELIMITER,
				after: nextAfter,
				limit: PAGE_SIZE
			});
			// Appended rather than replaced: continuation pages are the rest of the
			// same listing, not a new one.
			prefixes = [...prefixes, ...result.prefixes];
			objects = [...objects, ...result.objects];
			truncated = result.truncated;
			nextAfter = result.nextAfter;
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not load more objects';
		} finally {
			loadingMore = false;
		}
	}

	// Walking a folder stays on this route and only rewrites the query, so the
	// back button retraces the path a person actually took.
	function navigate(nextPrefix: string) {
		const target = new URL(page.url);
		if (nextPrefix) target.searchParams.set('prefix', nextPrefix);
		else target.searchParams.delete('prefix');
		// resolve() maps a route id to a path and has no query-string form, so it
		// has nothing to contribute here: the route is unchanged and the path came
		// from page.url, which SvelteKit already resolved.
		// eslint-disable-next-line svelte/no-navigation-without-resolve
		goto(target);
	}

	async function openDetail(object: StoredObject) {
		detail = null;
		detailError = '';
		copied = false;
		// A link minted for the last object would be a link to the wrong thing.
		signed = null;
		signError = '';
		copiedSigned = false;
		detailDialog.showModal();
		try {
			detail = await api.object(bucket, object.key);
		} catch (cause) {
			detailError = cause instanceof ApiError ? cause.message : 'could not read the object';
		}
	}

	async function confirmDelete() {
		if (!pendingDelete) return;
		try {
			await api.deleteObject(bucket, pendingDelete.key);
			deleteDialog.close();
			pendingDelete = null;
			await loadFirstPage(bucket, prefix);
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not delete the object';
			deleteDialog.close();
		}
	}

	/// Each crumb is the prefix up to and including its own delimiter, so
	/// clicking one lists exactly that level.
	const crumbs = $derived.by(() => {
		if (!prefix) return [];
		const parts = prefix.split(DELIMITER).filter(Boolean);
		let walked = '';
		return parts.map((part) => {
			walked += part + DELIMITER;
			return { label: part, prefix: walked };
		});
	});

	const totalBytes = $derived(objects.reduce((sum, object) => sum + object.size, 0));
</script>

<svelte:head><title>{bucket} · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-5">
	<div class="flex flex-col gap-2">
		<div class="breadcrumbs py-0 text-sm">
			<ul>
				<li><a href={resolve('/buckets')}>Buckets</a></li>
				<li>
					<button class="link link-hover" onclick={() => navigate('')}>{bucket}</button>
				</li>
				{#each crumbs as crumb (crumb.prefix)}
					<li>
						<button class="link link-hover" onclick={() => navigate(crumb.prefix)}>
							{crumb.label}
						</button>
					</li>
				{/each}
			</ul>
		</div>

		<div class="flex flex-wrap items-baseline justify-between gap-3">
			<h1 class="text-2xl font-semibold">{crumbs.at(-1)?.label ?? bucket}</h1>
			<span class="text-base-content/60 text-xs">
				{plural(prefixes.length, 'folder')} · {plural(objects.length, 'object')} · {formatBytes(
					totalBytes
				)}{truncated ? ' so far' : ''}
			</span>
		</div>
	</div>

	{#if error}
		<div role="alert" class="alert alert-error alert-soft" in:fly={{ y: -6, duration: 200 }}>
			<span>{error}</span>
		</div>
	{/if}

	{#if loading}
		<div class="skeleton rounded-box h-64"></div>
	{:else if prefixes.length === 0 && objects.length === 0}
		<div class="border-base-300 rounded-box border border-dashed p-8">
			<p class="text-base-content/70">
				Nothing here. Upload with <code class="font-mono">aws s3 cp</code> against the S3 port — the console's
				own uploader is not built yet.
			</p>
		</div>
	{:else}
		<div class="border-base-300 bg-base-100 rounded-box overflow-x-auto border">
			<table class="table table-sm table-pin-rows">
				<thead>
					<tr>
						<th>Name</th>
						<th class="w-32">Size</th>
						<th class="w-48">Modified</th>
						<th class="w-0"></th>
					</tr>
				</thead>
				<tbody>
					{#each prefixes as folder (folder)}
						<tr in:fade={{ duration: 120 }}>
							<td colspan="3">
								<button
									class="link link-hover flex items-center gap-2 font-medium"
									onclick={() => navigate(folder)}
								>
									<svg
										class="text-base-content/40 size-4 shrink-0"
										viewBox="0 0 24 24"
										fill="none"
										stroke="currentColor"
										stroke-width="2"
									>
										<path
											d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"
											stroke-linejoin="round"
										/>
									</svg>
									{leafName(folder, prefix)}
								</button>
							</td>
							<td></td>
						</tr>
					{/each}

					{#each objects as object (object.key)}
						<tr in:fade={{ duration: 120 }}>
							<td class="max-w-md">
								<button
									class="link link-hover block truncate text-left font-mono text-xs"
									title={object.key}
									onclick={() => openDetail(object)}
								>
									{leafName(object.key, prefix)}
								</button>
							</td>
							<td>{formatBytes(object.size)}</td>
							<td class="text-base-content/70">{formatTimestamp(object.lastModifiedMs)}</td>
							<td>
								<button
									class="btn btn-ghost btn-xs text-error"
									onclick={() => {
										pendingDelete = object;
										deleteDialog.showModal();
									}}
								>
									Delete
								</button>
							</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</div>

		{#if truncated}
			<button class="btn btn-sm self-start" onclick={loadMore} disabled={loadingMore}>
				{#if loadingMore}<span class="loading loading-spinner loading-xs"></span>{/if}
				Load more
			</button>
		{/if}
	{/if}
</div>

<dialog bind:this={detailDialog} class="modal">
	<div class="modal-box max-w-2xl">
		{#if detailError}
			<div role="alert" class="alert alert-error alert-soft"><span>{detailError}</span></div>
		{:else if !detail}
			<div class="flex items-center gap-2 py-8">
				<span class="loading loading-spinner loading-sm"></span>
				<span class="text-base-content/60 text-sm">Reading metadata…</span>
			</div>
		{:else}
			<h2 class="font-mono text-sm break-all">{detail.key}</h2>

			<div class="mt-4 flex flex-col gap-1">
				<div class="flex items-center justify-between gap-2">
					<span class="text-base-content/70 text-xs font-medium tracking-wide uppercase">
						Object URL
					</span>
					{#if publicRead}
						<span class="badge badge-xs badge-success badge-soft">anonymous read is on</span>
					{:else}
						<span class="badge badge-xs badge-ghost">needs a signed request</span>
					{/if}
				</div>
				<div class="join w-full">
					<input
						bind:this={urlField}
						class="input join-item w-full font-mono text-xs"
						readonly
						value={detailUrl}
						aria-label="Object URL"
						onfocus={(event) => event.currentTarget.select()}
					/>
					<button class="btn join-item" type="button" onclick={copyUrl}>
						{copied ? 'Copied' : 'Copy'}
					</button>
				</div>
				<p class="text-base-content/50 text-xs">
					{#if publicRead}
						Opens in a browser as-is. Turn anonymous read off on the Buckets page and this becomes a
						403.
					{:else}
						This bucket is private, so the URL needs SigV4 credentials — paste it into a client, not
						the address bar.
					{/if}
				</p>
			</div>

			<div class="mt-4 flex flex-col gap-1">
				<div class="flex items-center justify-between gap-2">
					<span class="text-base-content/70 text-xs font-medium tracking-wide uppercase">
						Presigned link
					</span>
					<span class="badge badge-xs badge-ghost">read only, expires</span>
				</div>
				<div class="join w-full">
					<select
						class="select join-item"
						bind:value={expirySeconds}
						onchange={() => (signed = null)}
						aria-label="How long the link stays valid"
					>
						{#each EXPIRY_CHOICES as choice (choice.seconds)}
							<option value={choice.seconds}>{choice.label}</option>
						{/each}
					</select>
					<button
						class="btn btn-primary join-item"
						type="button"
						onclick={generateLink}
						disabled={signing}
					>
						{#if signing}<span class="loading loading-spinner loading-xs"></span>{/if}
						Generate
					</button>
				</div>

				{#if signError}
					<p class="text-error text-xs">{signError}</p>
				{:else if signed}
					<div class="join w-full" transition:fly={{ y: -4, duration: 150 }}>
						<input
							bind:this={signedField}
							class="input join-item w-full font-mono text-xs"
							readonly
							value={signed.url}
							aria-label="Presigned URL"
							onfocus={(event) => event.currentTarget.select()}
						/>
						<button class="btn join-item" type="button" onclick={copySigned}>
							{copiedSigned ? 'Copied' : 'Copy'}
						</button>
					</div>
					<p class="text-base-content/50 text-xs">
						Opens in any browser until {formatTimestamp(signed.expiresAtMs)}. The method is inside
						the signature, so it grants a GET and nothing else.
					</p>
				{:else}
					<p class="text-base-content/50 text-xs">
						Shares one private object for a while without making the whole bucket public. Signed on
						the server — the console holds a session, never an S3 secret.
					</p>
				{/if}
			</div>

			<div class="mt-4 overflow-x-auto">
				<table class="table table-xs">
					<tbody>
						<tr
							><td class="text-base-content/60 w-40">Size</td><td>{formatBytes(detail.size)}</td
							></tr
						>
						<tr>
							<td class="text-base-content/60">Content type</td>
							<td class="font-mono text-xs">{detail.contentType}</td>
						</tr>
						<tr>
							<td class="text-base-content/60">Modified</td>
							<td>{formatTimestamp(detail.lastModifiedMs)}</td>
						</tr>
						<tr>
							<td class="text-base-content/60">ETag</td>
							<td class="font-mono text-xs break-all">{detail.etag}</td>
						</tr>
						<tr>
							<td class="text-base-content/60">SHA-256</td>
							<td class="font-mono text-xs break-all">{detail.sha256}</td>
						</tr>
					</tbody>
				</table>
			</div>

			{#if Object.keys(detail.userMetadata).length > 0}
				<h3 class="text-base-content/70 mt-4 text-xs font-medium tracking-wide uppercase">
					User metadata
				</h3>
				<div class="mt-1 overflow-x-auto">
					<table class="table table-xs">
						<tbody>
							{#each Object.entries(detail.userMetadata) as [name, value] (name)}
								<tr>
									<td class="text-base-content/60 w-40 font-mono text-xs">{name}</td>
									<td class="font-mono text-xs break-all">{value}</td>
								</tr>
							{/each}
						</tbody>
					</table>
				</div>
			{/if}
		{/if}

		<div class="modal-action">
			<button class="btn btn-sm" type="button" onclick={() => detailDialog.close()}>Close</button>
		</div>
	</div>
	<form method="dialog" class="modal-backdrop"><button>close</button></form>
</dialog>

<dialog bind:this={deleteDialog} class="modal">
	<div class="modal-box max-w-md">
		<h2 class="text-lg font-medium">Delete object</h2>
		<p class="text-base-content/70 mt-2 font-mono text-xs break-all">{pendingDelete?.key}</p>
		<p class="text-base-content/70 mt-2 text-sm">
			The payload is unlinked once the metadata naming it is gone. There is no undo.
		</p>
		<div class="modal-action">
			<button class="btn btn-sm" type="button" onclick={() => deleteDialog.close()}>Cancel</button>
			<button class="btn btn-error btn-sm" type="button" onclick={confirmDelete}>Delete</button>
		</div>
	</div>
	<form method="dialog" class="modal-backdrop"><button>close</button></form>
</dialog>
