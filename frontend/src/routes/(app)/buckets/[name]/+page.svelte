<script lang="ts">
	import { goto } from '$app/navigation';
	import { page } from '$app/state';
	import { resolve } from '$app/paths';
	import { fade, fly } from 'svelte/transition';
	import {
		api,
		ApiError,
		type CorsRule,
		type ObjectDetail,
		type Presigned,
		type StoredObject
	} from '$lib/api';
	import { formatBytes, formatTimestamp, leafName, plural } from '$lib/format';
	import { motionDistance, motionDuration } from '$lib/motion';
	import { copyText, objectUrl, s3Endpoint } from '$lib/s3url';
	import Icon from '$lib/components/Icon.svelte';
	import Uploader from '$lib/components/Uploader.svelte';

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

	// --- CORS ----------------------------------------------------------------

	// The form edits comma-separated text rather than the arrays the API takes.
	// A list of chips would look better and would be worse: the values are
	// origins and header names, they are usually pasted in from somewhere else,
	// and a field you can paste a whole list into is the one that gets used.
	type RuleDraft = {
		id: string;
		origins: string;
		methods: string[];
		headers: string;
		expose: string;

		// Not a string: `bind:value` on a number input hands back a number, or
		// null when the box is empty — which is exactly the distinction the rule
		// needs between "no max age" and a value.
		maxAge: number | null;
	};

	const CORS_METHODS = ['GET', 'HEAD', 'PUT', 'POST', 'DELETE'];

	let corsDialog: HTMLDialogElement;
	let corsDrafts = $state<RuleDraft[]>([]);
	let corsLoading = $state(false);
	let corsSaving = $state(false);
	let corsError = $state('');
	let corsSaved = $state(false);
	let corsCount = $state(0);

	function splitList(text: string): string[] {
		return text
			.split(/[\s,]+/)
			.map((entry) => entry.trim())
			.filter(Boolean);
	}

	function toDraft(rule: CorsRule): RuleDraft {
		return {
			id: rule.id,
			origins: rule.allowedOrigins.join(', '),
			methods: [...rule.allowedMethods],
			headers: rule.allowedHeaders.join(', '),
			expose: rule.exposeHeaders.join(', '),
			// An empty box means "no max age", which is what null encodes. Zero
			// stays visible as a zero because it means something different.
			maxAge: rule.maxAgeSeconds
		};
	}

	function fromDraft(draft: RuleDraft): CorsRule {
		return {
			id: draft.id.trim(),
			allowedOrigins: splitList(draft.origins),
			allowedMethods: draft.methods,
			allowedHeaders: splitList(draft.headers),
			exposeHeaders: splitList(draft.expose),
			maxAgeSeconds: Number.isFinite(draft.maxAge) ? draft.maxAge : null
		};
	}

	function blankDraft(): RuleDraft {
		// Pre-filled with the rule a browser app almost always wants first, so
		// the common case is one click and a hostname rather than five fields.
		return {
			id: '',
			origins: '',
			methods: ['GET', 'HEAD'],
			headers: '',
			expose: 'ETag',
			maxAge: null
		};
	}

	async function loadCors() {
		corsLoading = true;
		corsError = '';
		corsSaved = false;
		try {
			const rules = await api.bucketCors(bucket);
			corsCount = rules.length;
			corsDrafts = rules.length > 0 ? rules.map(toDraft) : [blankDraft()];
		} catch (cause) {
			corsError = cause instanceof ApiError ? cause.message : 'could not read the CORS rules';
			corsDrafts = [blankDraft()];
		} finally {
			corsLoading = false;
		}
	}

	function openCors() {
		corsDialog.showModal();
		loadCors();
	}

	function toggleMethod(draft: RuleDraft, method: string, on: boolean) {
		draft.methods = on
			? // Kept in CORS_METHODS order rather than click order, so two rules
				// with the same methods read the same way.
				CORS_METHODS.filter((name) => draft.methods.includes(name) || name === method)
			: draft.methods.filter((name) => name !== method);
	}

	async function saveCors() {
		corsSaving = true;
		corsError = '';
		corsSaved = false;
		try {
			const rules = corsDrafts.map(fromDraft);
			corsCount = (await api.setBucketCors(bucket, rules)).length;
			corsSaved = true;
			setTimeout(() => (corsSaved = false), 2000);
		} catch (cause) {
			corsError = cause instanceof ApiError ? cause.message : 'could not save the CORS rules';
		} finally {
			corsSaving = false;
		}
	}

	async function clearCors() {
		corsSaving = true;
		corsError = '';
		try {
			await api.clearBucketCors(bucket);
			corsCount = 0;
			corsDrafts = [blankDraft()];
			corsSaved = true;
			setTimeout(() => (corsSaved = false), 2000);
		} catch (cause) {
			corsError = cause instanceof ApiError ? cause.message : 'could not clear the CORS rules';
		} finally {
			corsSaving = false;
		}
	}

	// Loaded once alongside the listing so the header can say whether this
	// bucket has any rules without the editor being opened.
	$effect(() => {
		api
			.bucketCors(bucket)
			.then((rules) => (corsCount = rules.length))
			.catch(() => (corsCount = 0));
	});

	// --- Bucket policy -------------------------------------------------------

	// A textarea rather than a builder. A policy is an IAM document with a
	// published grammar, it is usually pasted in from somewhere that already has
	// one, and a form that could only express the shapes we thought of would
	// quietly refuse the rest. What the console adds is the server's own reading
	// of the document — whether it grants anonymous reads — which is the half
	// that actually decides what happens on the wire.
	const PUBLIC_READ_TEMPLATE = (name: string) =>
		JSON.stringify(
			{
				Version: '2012-10-17',
				Statement: [
					{
						Sid: 'PublicRead',
						Effect: 'Allow',
						Principal: '*',
						Action: 's3:GetObject',
						Resource: `arn:aws:s3:::${name}/*`
					}
				]
			},
			null,
			2
		);

	let policyDialog: HTMLDialogElement;
	let policyDraft = $state('');
	let policyLoading = $state(false);
	let policySaving = $state(false);
	let policyError = $state('');
	let policySaved = $state(false);
	let hasPolicy = $state(false);
	let policyPublicRead = $state(false);

	/// Client-side only, and only to keep the Save button from sending something
	/// the server will certainly refuse. The server validates again — it has to,
	/// since the S3 API accepts the same document without going through here.
	const policyMalformed = $derived.by(() => {
		const text = policyDraft.trim();
		if (!text) return '';
		try {
			JSON.parse(text);
			return '';
		} catch (cause) {
			return cause instanceof Error ? cause.message : 'not valid JSON';
		}
	});

	async function loadPolicy() {
		policyLoading = true;
		policyError = '';
		policySaved = false;
		try {
			const current = await api.bucketPolicy(bucket);
			hasPolicy = current.policy.length > 0;
			policyPublicRead = current.publicRead;
			// Pretty-printed on the way in so a document written by a client on
			// one line is readable here, and left exactly as typed on the way
			// back out.
			policyDraft = current.policy ? format(current.policy) : '';
		} catch (cause) {
			policyError = cause instanceof ApiError ? cause.message : 'could not read the policy';
		} finally {
			policyLoading = false;
		}
	}

	function format(document: string): string {
		try {
			return JSON.stringify(JSON.parse(document), null, 2);
		} catch {
			return document;
		}
	}

	function openPolicy() {
		policyDialog.showModal();
		loadPolicy();
	}

	async function savePolicy() {
		policySaving = true;
		policyError = '';
		policySaved = false;
		try {
			const saved = await api.setBucketPolicy(bucket, policyDraft.trim());
			hasPolicy = saved.policy.length > 0;
			policyPublicRead = saved.publicRead;
			publicRead = saved.publicRead;
			policySaved = true;
			setTimeout(() => (policySaved = false), 2000);
		} catch (cause) {
			policyError = cause instanceof ApiError ? cause.message : 'could not save the policy';
		} finally {
			policySaving = false;
		}
	}

	async function clearPolicy() {
		policySaving = true;
		policyError = '';
		try {
			const saved = await api.clearBucketPolicy(bucket);
			hasPolicy = false;
			policyPublicRead = saved.publicRead;
			publicRead = saved.publicRead;
			policyDraft = '';
			policySaved = true;
			setTimeout(() => (policySaved = false), 2000);
		} catch (cause) {
			policyError = cause instanceof ApiError ? cause.message : 'could not remove the policy';
		} finally {
			policySaving = false;
		}
	}

	$effect(() => {
		api
			.bucketPolicy(bucket)
			.then((current) => {
				hasPolicy = current.policy.length > 0;
				policyPublicRead = current.publicRead;
			})
			.catch(() => (hasPolicy = false));
	});

	// --- Uploads -------------------------------------------------------------

	let uploadOpen = $state(false);

	// --- Presentation --------------------------------------------------------

	/// A file's icon by extension. Only the families worth telling apart at a
	/// glance in a listing; everything else is a file, which is honest.
	function fileIcon(key: string): 'file' | 'memory' | 'overview' | 'globe' {
		const extension = key.slice(key.lastIndexOf('.') + 1).toLowerCase();
		if (['png', 'jpg', 'jpeg', 'gif', 'webp', 'svg', 'ico', 'avif'].includes(extension)) {
			return 'globe';
		}
		if (['json', 'xml', 'yaml', 'yml', 'csv', 'toml'].includes(extension)) return 'overview';
		if (['zip', 'gz', 'tar', 'br', 'zst', 'bz2', '7z'].includes(extension)) return 'memory';
		return 'file';
	}
</script>

<svelte:head><title>{bucket} · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-5">
	<section class="panel surface-raised grid overflow-hidden lg:grid-cols-[minmax(0,1fr)_18rem]">
		<div class="flex flex-col gap-4 p-5 sm:p-6">
			<div class="breadcrumbs py-0 text-sm">
				<ul>
					<li><a class="link-hover" href={resolve('/buckets')}>Buckets</a></li>
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

			<div class="flex flex-wrap items-center justify-between gap-3">
				<div class="flex min-w-0 items-center gap-3">
					<span
						class="bg-primary/10 text-primary grid size-10 shrink-0 place-items-center rounded-xl"
					>
						<Icon name={prefix ? 'folder' : 'bucket'} class="size-5" />
					</span>
					<div class="flex min-w-0 flex-col gap-0.5">
						<h1 class="truncate text-2xl font-semibold tracking-tight">
							{crumbs.at(-1)?.label ?? bucket}
						</h1>
						<p class="text-base-content/55 text-xs">
							{plural(prefixes.length, 'folder')} · {plural(objects.length, 'object')} · {formatBytes(
								totalBytes
							)}{truncated ? ' so far' : ''}
						</p>
					</div>
				</div>

				<div class="flex flex-wrap items-center gap-2">
					{#if publicRead}
						<span class="badge badge-sm badge-success badge-soft gap-1">
							<Icon name="globe" class="size-3" />
							anonymous read
						</span>
					{/if}

					<button
						class="btn btn-sm gap-1.5 {hasPolicy ? 'btn-ghost' : 'btn-ghost'}"
						onclick={openPolicy}
					>
						<Icon name="shield" class="size-3.5" />
						Policy
						{#if hasPolicy}<span class="badge badge-xs badge-primary"></span>{/if}
					</button>

					<button class="btn btn-ghost btn-sm gap-1.5" onclick={openCors}>
						<Icon name="globe" class="size-3.5" />
						CORS
						{#if corsCount > 0}<span class="badge badge-xs badge-ghost">{corsCount}</span>{/if}
					</button>

					<button
						class="btn btn-sm gap-1.5 {uploadOpen ? 'btn-neutral' : 'btn-primary'}"
						onclick={() => (uploadOpen = !uploadOpen)}
					>
						<Icon name={uploadOpen ? 'close' : 'upload'} class="size-3.5" />
						{uploadOpen ? 'Close' : 'Upload'}
					</button>
				</div>
			</div>
		</div>

		<div class="relative min-h-32 overflow-hidden sm:min-h-40 lg:min-h-full">
			<img
				src="/images/bucket-workspace.webp"
				alt="Colourful folders flowing into an object storage gateway"
				width="1200"
				height="545"
				decoding="async"
				class="absolute inset-0 size-full object-cover object-[70%_center]"
			/>
			<div
				class="from-base-100/70 pointer-events-none absolute inset-0 bg-gradient-to-b from-transparent via-transparent to-base-100/70 lg:bg-gradient-to-r lg:from-base-100/85 lg:via-transparent lg:to-transparent"
			></div>
		</div>
	</section>

	{#if error}
		<div
			role="alert"
			class="alert alert-error alert-soft"
			in:fly={{ y: motionDistance(-6), duration: motionDuration(180) }}
		>
			<Icon name="warning" />
			<span>{error}</span>
		</div>
	{/if}

	{#if uploadOpen}
		<div transition:fly={{ y: motionDistance(-8), duration: motionDuration(200) }}>
			<Uploader {bucket} {prefix} onfinished={() => loadFirstPage(bucket, prefix)} />
		</div>
	{/if}

	{#if loading}
		<div class="skeleton rounded-box h-64"></div>
	{:else if prefixes.length === 0 && objects.length === 0}
		<div
			class="panel surface-raised grid items-center gap-6 overflow-hidden p-6 sm:p-8 lg:grid-cols-[13rem_1fr]"
		>
			<img
				src="/images/empty-bucket.webp"
				alt="Colourful storage bucket ready for file uploads"
				width="768"
				height="768"
				loading="lazy"
				class="mx-auto aspect-square w-44 rounded-2xl object-cover shadow-xl shadow-primary/10 lg:w-52"
			/>
			<div class="flex flex-col items-center gap-4 text-center lg:items-start lg:text-left">
				<div class="flex flex-col gap-1">
					<p class="text-xl font-bold tracking-tight">This bucket is ready for files</p>
					<p class="text-base-content/60 max-w-md text-sm">
						Drop files in from the Upload button, or push them with
						<code class="bg-base-200 rounded px-1 py-0.5 font-mono text-xs">aws s3 cp</code> against the
						S3 port.
					</p>
				</div>
				<button class="btn btn-primary gap-2" onclick={() => (uploadOpen = true)}>
					<Icon name="upload" class="size-4" />
					Upload files
				</button>
			</div>
		</div>
	{:else}
		<div class="panel overflow-x-auto">
			<table class="table table-sm table-pin-rows">
				<thead>
					<tr class="border-base-300">
						<th>Name</th>
						<th class="w-32">Size</th>
						<th class="w-48">Modified</th>
						<th class="w-0"></th>
					</tr>
				</thead>
				<tbody>
					{#each prefixes as folder (folder)}
						<tr
							class="hover:bg-base-200/70 transition-colors"
							in:fade={{ duration: motionDuration(120) }}
						>
							<td colspan="3">
								<button
									class="group flex items-center gap-2.5 font-medium"
									onclick={() => navigate(folder)}
								>
									<span
										class="bg-warning/12 text-warning grid size-7 shrink-0 place-items-center rounded-lg"
									>
										<Icon name="folder" class="size-3.5" />
									</span>
									<span class="group-hover:underline">{leafName(folder, prefix)}</span>
									<Icon
										name="chevronRight"
										class="text-base-content/30 size-3.5 transition-transform group-hover:translate-x-0.5"
									/>
								</button>
							</td>
							<td></td>
						</tr>
					{/each}

					{#each objects as object (object.key)}
						<tr
							class="hover:bg-base-200/70 transition-colors"
							in:fade={{ duration: motionDuration(120) }}
						>
							<td class="max-w-md">
								<button
									class="group flex w-full items-center gap-2.5 text-left"
									title={object.key}
									onclick={() => openDetail(object)}
								>
									<span
										class="bg-base-200 text-base-content/50 grid size-7 shrink-0 place-items-center rounded-lg"
									>
										<Icon name={fileIcon(object.key)} class="size-3.5" />
									</span>
									<span class="truncate font-mono text-xs group-hover:underline">
										{leafName(object.key, prefix)}
									</span>
								</button>
							</td>
							<td class="tabular-nums">{formatBytes(object.size)}</td>
							<td class="text-base-content/60">{formatTimestamp(object.lastModifiedMs)}</td>
							<td>
								<button
									class="btn btn-ghost btn-xs text-error gap-1"
									aria-label="Delete {object.key}"
									onclick={() => {
										pendingDelete = object;
										deleteDialog.showModal();
									}}
								>
									<Icon name="trash" class="size-3.5" />
									Delete
								</button>
							</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</div>

		{#if truncated}
			<button class="btn btn-sm gap-1.5 self-start" onclick={loadMore} disabled={loadingMore}>
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
			<h2 class="flex items-start gap-2.5">
				<span
					class="bg-base-200 text-base-content/60 grid size-8 shrink-0 place-items-center rounded-lg"
				>
					<Icon name={fileIcon(detail.key)} class="size-4" />
				</span>
				<span class="pt-1.5 font-mono text-sm break-all">{detail.key}</span>
			</h2>

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
					<div
						class="join w-full"
						transition:fly={{ y: motionDistance(-4), duration: motionDuration(150) }}
					>
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

<dialog bind:this={corsDialog} class="modal">
	<div class="modal-box max-w-3xl">
		<h2 class="flex items-center gap-2 text-lg font-medium">
			<span class="bg-primary/10 text-primary grid size-8 place-items-center rounded-lg">
				<Icon name="globe" class="size-4" />
			</span>
			Cross-origin access
		</h2>
		<p class="text-base-content/70 mt-2 text-sm">
			Rules are checked top to bottom and the first one that permits the origin, the method
			<em>and</em> every header the browser asked about is the one that answers. A request no rule permits
			is still served to a signed client — CORS only decides whether a page's script may read the response.
		</p>

		{#if corsError}
			<div
				role="alert"
				class="alert alert-error alert-soft mt-4"
				in:fly={{ y: motionDistance(-6), duration: motionDuration(180) }}
			>
				<span>{corsError}</span>
			</div>
		{/if}

		{#if corsLoading}
			<div class="skeleton rounded-box mt-4 h-48"></div>
		{:else}
			<div class="mt-4 flex flex-col gap-4">
				{#each corsDrafts as draft, index (index)}
					<div
						class="border-base-300 rounded-box border p-4"
						in:fade={{ duration: motionDuration(120) }}
					>
						<div class="flex items-center justify-between gap-3">
							<span class="text-base-content/60 text-xs">Rule {index + 1}</span>
							<button
								class="btn btn-ghost btn-xs text-error"
								type="button"
								disabled={corsDrafts.length === 1}
								onclick={() => (corsDrafts = corsDrafts.filter((_, i) => i !== index))}
							>
								Remove
							</button>
						</div>

						<div class="mt-2 grid gap-4 sm:grid-cols-2">
							<fieldset class="fieldset gap-1 p-0 sm:col-span-2">
								<legend class="fieldset-legend">Allowed origins</legend>
								<input
									class="input w-full font-mono text-xs"
									placeholder="https://app.example.com, https://*.example.com"
									bind:value={draft.origins}
								/>
								<p class="fieldset-label">
									Scheme and port are part of the match. One <code class="font-mono">*</code> per entry.
								</p>
							</fieldset>

							<fieldset class="fieldset gap-1 p-0 sm:col-span-2">
								<legend class="fieldset-legend">Allowed methods</legend>
								<div class="flex flex-wrap gap-3">
									{#each CORS_METHODS as method (method)}
										<label class="label cursor-pointer gap-2">
											<input
												type="checkbox"
												class="checkbox checkbox-sm"
												checked={draft.methods.includes(method)}
												onchange={(event) =>
													toggleMethod(draft, method, event.currentTarget.checked)}
											/>
											<span class="font-mono text-xs">{method}</span>
										</label>
									{/each}
								</div>
							</fieldset>

							<fieldset class="fieldset gap-1 p-0">
								<legend class="fieldset-legend">Allowed request headers</legend>
								<input
									class="input w-full font-mono text-xs"
									placeholder="content-type, x-amz-*"
									bind:value={draft.headers}
								/>
								<p class="fieldset-label">What the browser may send. Blank permits none.</p>
							</fieldset>

							<fieldset class="fieldset gap-1 p-0">
								<legend class="fieldset-legend">Exposed response headers</legend>
								<input
									class="input w-full font-mono text-xs"
									placeholder="ETag"
									bind:value={draft.expose}
								/>
								<p class="fieldset-label">
									What a script may read back. ETag is needed to upload in parts.
								</p>
							</fieldset>

							<fieldset class="fieldset gap-1 p-0">
								<legend class="fieldset-legend">Preflight cache (seconds)</legend>
								<input
									class="input w-full"
									type="number"
									min="0"
									max="86400"
									placeholder="browser decides"
									bind:value={draft.maxAge}
								/>
							</fieldset>

							<fieldset class="fieldset gap-1 p-0">
								<legend class="fieldset-legend">Label</legend>
								<input class="input w-full" placeholder="optional" bind:value={draft.id} />
							</fieldset>
						</div>
					</div>
				{/each}

				<button
					class="btn btn-ghost btn-sm border-base-300 hover:border-primary gap-1.5 self-start border border-dashed"
					type="button"
					onclick={() => (corsDrafts = [...corsDrafts, blankDraft()])}
				>
					<Icon name="plus" class="size-3.5" />
					Add a rule
				</button>
			</div>
		{/if}

		<div class="modal-action">
			{#if corsSaved}
				<span class="text-success self-center text-xs" in:fade={{ duration: motionDuration(150) }}
					>Saved</span
				>
			{/if}
			<button
				class="btn btn-ghost btn-sm"
				type="button"
				disabled={corsSaving || corsCount === 0}
				onclick={clearCors}
			>
				Turn CORS off
			</button>
			<button class="btn btn-sm" type="button" onclick={() => corsDialog.close()}>Close</button>
			<button class="btn btn-primary btn-sm" type="button" disabled={corsSaving} onclick={saveCors}>
				{corsSaving ? 'Saving…' : 'Save rules'}
			</button>
		</div>
	</div>
	<form method="dialog" class="modal-backdrop"><button>close</button></form>
</dialog>

<dialog bind:this={policyDialog} class="modal">
	<div class="modal-box max-w-3xl">
		<h2 class="flex items-center gap-2 text-lg font-medium">
			<span class="bg-primary/10 text-primary grid size-8 place-items-center rounded-lg">
				<Icon name="shield" class="size-4" />
			</span>
			Bucket policy
		</h2>
		<p class="text-base-content/70 mt-3 text-sm">
			An IAM-shaped JSON document, stored and returned verbatim. MonoBucket does not evaluate it in
			general — it reads one thing out of it: whether an unauthenticated <code class="font-mono"
				>GetObject</code
			>
			is allowed. A statement it does not recognise, or one carrying a
			<code class="font-mono">Condition</code>, grants nothing rather than being guessed at.
		</p>

		{#if policyError}
			<div
				role="alert"
				class="alert alert-error alert-soft mt-4"
				in:fly={{ y: motionDistance(-6), duration: motionDuration(180) }}
			>
				<Icon name="warning" />
				<span>{policyError}</span>
			</div>
		{/if}

		{#if policyLoading}
			<div class="skeleton rounded-box mt-4 h-64"></div>
		{:else}
			<div class="mt-4 flex flex-col gap-2">
				<div class="flex flex-wrap items-center justify-between gap-2">
					<span class="text-base-content/70 text-xs font-medium tracking-wide uppercase">
						Document
					</span>
					<div class="flex items-center gap-2">
						{#if hasPolicy}
							{#if policyPublicRead}
								<span class="badge badge-sm badge-success badge-soft gap-1">
									<Icon name="globe" class="size-3" />
									grants anonymous read
								</span>
							{:else}
								<span class="badge badge-sm badge-ghost">grants no anonymous access</span>
							{/if}
						{/if}
						<button
							class="btn btn-ghost btn-xs"
							type="button"
							onclick={() => (policyDraft = PUBLIC_READ_TEMPLATE(bucket))}
						>
							Insert public-read template
						</button>
						<button
							class="btn btn-ghost btn-xs"
							type="button"
							disabled={!policyDraft.trim() || policyMalformed !== ''}
							onclick={() => (policyDraft = format(policyDraft))}
						>
							Reformat
						</button>
					</div>
				</div>

				<textarea
					class="textarea h-72 w-full font-mono text-xs leading-relaxed {policyMalformed
						? 'textarea-error'
						: ''}"
					spellcheck="false"
					placeholder={'{\n  "Version": "2012-10-17",\n  "Statement": []\n}'}
					aria-label="Bucket policy document"
					bind:value={policyDraft}></textarea>

				{#if policyMalformed}
					<p class="text-error text-xs">{policyMalformed}</p>
				{:else}
					<p class="text-base-content/50 text-xs">
						Up to 20 KB. Saving replaces whatever is there; the derived anonymous-read flag is
						recomputed on every write, so removing the statement removes the access it granted.
					</p>
				{/if}
			</div>
		{/if}

		<div class="modal-action">
			{#if policySaved}
				<span class="text-success self-center text-xs" in:fade={{ duration: motionDuration(150) }}
					>Saved</span
				>
			{/if}
			<button
				class="btn btn-ghost btn-sm"
				type="button"
				disabled={policySaving || !hasPolicy}
				onclick={clearPolicy}
			>
				Remove policy
			</button>
			<button class="btn btn-sm" type="button" onclick={() => policyDialog.close()}>Close</button>
			<button
				class="btn btn-primary btn-sm"
				type="button"
				disabled={policySaving || policyMalformed !== '' || !policyDraft.trim()}
				onclick={savePolicy}
			>
				{policySaving ? 'Saving…' : 'Save policy'}
			</button>
		</div>
	</div>
	<form method="dialog" class="modal-backdrop"><button>close</button></form>
</dialog>
