<script lang="ts">
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { flip } from 'svelte/animate';
	import { fade, fly } from 'svelte/transition';
	import { api, ApiError, type Bucket } from '$lib/api';
	import { formatTimestamp, plural } from '$lib/format';
	import { motionDistance, motionDuration } from '$lib/motion';
	import Icon from '$lib/components/Icon.svelte';

	let buckets = $state<Bucket[] | null>(null);
	let error = $state('');
	let busy = $state('');
	let filter = $state('');

	let newName = $state('');
	let createError = $state('');
	let creating = $state(false);
	let createDialog: HTMLDialogElement;
	let nameField = $state<HTMLInputElement | undefined>();

	let pendingDelete = $state<Bucket | null>(null);
	let deleteDialog: HTMLDialogElement;

	async function load() {
		try {
			buckets = await api.buckets();
			error = '';
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			error = cause instanceof ApiError ? cause.message : 'could not list buckets';
		}
	}

	$effect(() => {
		load();
	});

	const visible = $derived(
		(buckets ?? []).filter((bucket) => bucket.name.includes(filter.trim().toLowerCase()))
	);

	function openCreate() {
		createError = '';
		createDialog.showModal();
		// The field is the only thing in the dialog worth touching, and a modal
		// that opens with nothing focused makes a keyboard user hunt for it.
		nameField?.focus();
	}

	async function create(event: SubmitEvent) {
		event.preventDefault();
		createError = '';
		creating = true;
		try {
			await api.createBucket(newName.trim());
			newName = '';
			createDialog.close();
			await load();
		} catch (cause) {
			createError = cause instanceof ApiError ? cause.message : 'could not create the bucket';
		} finally {
			creating = false;
		}
	}

	async function confirmDelete() {
		if (!pendingDelete) return;
		const name = pendingDelete.name;
		busy = name;
		try {
			await api.deleteBucket(name);
			deleteDialog.close();
			pendingDelete = null;
			await load();
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not delete the bucket';
			deleteDialog.close();
		} finally {
			busy = '';
		}
	}

	// Optimistic on purpose: the toggle is the control and the response only
	// confirms it. A failure puts the switch back and says why.
	async function toggleAccess(bucket: Bucket, publicRead: boolean) {
		busy = bucket.name;
		const previous = bucket.publicRead;
		bucket.publicRead = publicRead;
		try {
			await api.setBucketAccess(bucket.name, publicRead);
			error = '';
		} catch (cause) {
			bucket.publicRead = previous;
			error = cause instanceof ApiError ? cause.message : 'could not change bucket access';
		} finally {
			busy = '';
		}
	}

	// A stable colour per bucket, derived from the name rather than assigned.
	// Six accents from the theme, so a list of buckets has landmarks in it and
	// the same bucket looks the same on every machine and after every restart.
	const SWATCHES = [
		'bg-primary/12 text-primary',
		'bg-secondary/12 text-secondary',
		'bg-accent/12 text-accent',
		'bg-info/12 text-info',
		'bg-success/12 text-success',
		'bg-warning/15 text-warning'
	];

	function swatch(name: string): string {
		let hash = 0;
		for (let index = 0; index < name.length; index += 1) {
			hash = (hash * 31 + name.charCodeAt(index)) >>> 0;
		}
		return SWATCHES[hash % SWATCHES.length];
	}
</script>

<svelte:head><title>Buckets · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-6">
	<header class="flex flex-wrap items-end justify-between gap-4">
		<div class="flex flex-col gap-1">
			<span class="eyebrow">Object storage</span>
			<h1 class="text-3xl font-bold tracking-tight sm:text-4xl">Buckets</h1>
			{#if buckets}
				<p class="text-base-content/55 text-sm">
					{plural(buckets.length, 'bucket')}{filter.trim() ? ` · ${visible.length} shown` : ''}
				</p>
			{/if}
		</div>

		<div class="flex items-center gap-2">
			{#if buckets && buckets.length > 4}
				<label class="input input-sm w-44">
					<Icon name="overview" class="size-3.5 opacity-50" />
					<input
						type="search"
						placeholder="Filter"
						bind:value={filter}
						aria-label="Filter buckets"
					/>
				</label>
			{/if}
			<button class="btn btn-primary gap-2 shadow-lg shadow-primary/20" onclick={openCreate}>
				<Icon name="plus" class="size-4" />
				Create bucket
			</button>
		</div>
	</header>

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

	{#if !buckets}
		<div class="skeleton rounded-box h-48" out:fade={{ duration: motionDuration(100) }}></div>
	{:else if buckets.length === 0}
		<div
			class="panel surface-raised grid items-center gap-6 overflow-hidden p-6 sm:p-8 lg:grid-cols-[14rem_1fr]"
			in:fly={{ y: motionDistance(10), duration: motionDuration(240), opacity: 0.5 }}
		>
			<img
				src="/images/empty-bucket.webp"
				alt="Colourful empty storage bucket with file tiles above it"
				width="768"
				height="768"
				loading="lazy"
				class="mx-auto aspect-square w-48 rounded-2xl object-cover shadow-xl shadow-primary/10 lg:w-56"
			/>
			<div class="flex flex-col items-center gap-4 text-center lg:items-start lg:text-left">
				<div class="flex flex-col gap-1">
					<p class="text-xl font-bold tracking-tight">Your first bucket starts here</p>
					<p class="text-base-content/60 max-w-md text-sm">
						Create one here, or with <code class="bg-base-200 rounded px-1 py-0.5 font-mono text-xs"
							>aws s3 mb</code
						> against the S3 port. Both write the same record.
					</p>
				</div>
				<button class="btn btn-primary gap-2" onclick={openCreate}>
					<Icon name="plus" class="size-4" />
					Create the first bucket
				</button>
			</div>
		</div>
	{:else}
		<div class="panel overflow-x-auto shadow-sm">
			<table class="table table-sm">
				<thead>
					<tr class="border-base-300">
						<th>Name</th>
						<th class="w-52">Created</th>
						<th class="w-40">Anonymous read</th>
						<th class="w-0"></th>
					</tr>
				</thead>
				<tbody>
					{#each visible as bucket (bucket.name)}
						<tr
							class="hover:bg-base-200/70 transition-colors"
							animate:flip={{ duration: motionDuration(200) }}
							in:fade={{ duration: motionDuration(150) }}
						>
							<td>
								<a class="group flex items-center gap-3" href={resolve(`/buckets/${bucket.name}`)}>
									<span
										class="grid size-8 shrink-0 place-items-center rounded-lg {swatch(bucket.name)}"
									>
										<Icon name="bucket" class="size-4" />
									</span>
									<span class="flex min-w-0 flex-col">
										<span class="truncate font-medium group-hover:underline">{bucket.name}</span>
										<span class="flex flex-wrap items-center gap-1">
											{#if bucket.hasPolicy}
												<span class="badge badge-xs badge-ghost gap-1">
													<Icon name="shield" class="size-2.5" />
													policy
												</span>
											{/if}
											{#if bucket.corsRules > 0}
												<span class="badge badge-xs badge-ghost gap-1">
													<Icon name="globe" class="size-2.5" />
													{plural(bucket.corsRules, 'CORS rule')}
												</span>
											{/if}
										</span>
									</span>
								</a>
							</td>
							<td class="text-base-content/60">{formatTimestamp(bucket.createdAtMs)}</td>
							<td>
								<label class="flex cursor-pointer items-center gap-2">
									<input
										type="checkbox"
										class="toggle toggle-sm toggle-success"
										checked={bucket.publicRead}
										disabled={busy === bucket.name}
										aria-label="Allow anonymous reads from {bucket.name}"
										onchange={(event) => toggleAccess(bucket, event.currentTarget.checked)}
									/>
									<span class="text-base-content/50 text-xs">
										{bucket.publicRead ? 'public' : 'private'}
									</span>
								</label>
							</td>
							<td>
								<button
									class="btn btn-ghost btn-xs text-error gap-1"
									disabled={busy === bucket.name}
									aria-label="Delete {bucket.name}"
									onclick={() => {
										pendingDelete = bucket;
										deleteDialog.showModal();
									}}
								>
									<Icon name="trash" class="size-3.5" />
									Delete
								</button>
							</td>
						</tr>
					{/each}

					{#if visible.length === 0}
						<tr>
							<td colspan="4" class="text-base-content/50 py-8 text-center text-sm">
								No bucket matches “{filter.trim()}”.
							</td>
						</tr>
					{/if}
				</tbody>
			</table>
		</div>
	{/if}
</div>

<dialog bind:this={createDialog} class="modal">
	<div class="modal-box max-w-md overflow-hidden p-0">
		<div class="surface-raised grid grid-cols-[1fr_7.5rem] items-center overflow-hidden">
			<div class="flex flex-col gap-1 p-6 pr-2">
				<span class="eyebrow">New storage space</span>
				<h2 class="text-xl font-semibold tracking-tight">Create bucket</h2>
				<p class="text-base-content/55 text-xs">Give your objects a durable home.</p>
			</div>
			<div class="relative h-32 overflow-hidden">
				<img
					src="/images/bucket-create.webp"
					alt="Colourful storage rings assembling into a new bucket"
					width="480"
					height="480"
					loading="lazy"
					decoding="async"
					class="absolute inset-0 size-full object-cover"
				/>
				<div
					class="from-base-100/70 pointer-events-none absolute inset-0 bg-gradient-to-r from-base-100/70 to-transparent"
				></div>
			</div>
		</div>

		<form class="flex flex-col gap-4 p-6" onsubmit={create}>
			<fieldset class="fieldset gap-1 p-0">
				<legend class="fieldset-legend">Name</legend>
				<input
					bind:this={nameField}
					class="input w-full font-mono"
					type="text"
					bind:value={newName}
					placeholder="my-bucket"
					autocomplete="off"
					spellcheck="false"
					required
				/>
				<!-- `fieldset-label`, not `label`: daisyUI's `.label` is
				     `white-space: nowrap`, and a sentence of guidance inside a
				     fieldset then sets a minimum width the dialog cannot meet, so
				     the field grows straight through the side of the modal. -->
				<p class="fieldset-label">
					DNS rules apply: lowercase letters, digits, dots and hyphens, 3 to 63 characters.
				</p>
			</fieldset>

			{#if createError}
				<div
					role="alert"
					class="alert alert-error alert-soft text-sm"
					in:fly={{ y: motionDistance(-4), duration: motionDuration(180) }}
				>
					<Icon name="warning" class="size-4" />
					<span>{createError}</span>
				</div>
			{/if}

			<div class="modal-action">
				<button class="btn btn-sm" type="button" onclick={() => createDialog.close()}>
					Cancel
				</button>
				<button class="btn btn-primary btn-sm" type="submit" disabled={creating}>
					{#if creating}<span class="loading loading-spinner loading-xs"></span>{/if}
					Create
				</button>
			</div>
		</form>
	</div>
	<form method="dialog" class="modal-backdrop"><button>close</button></form>
</dialog>

<dialog bind:this={deleteDialog} class="modal">
	<div class="modal-box max-w-md">
		<h2 class="flex items-center gap-2 text-lg font-medium">
			<span class="bg-error/10 text-error grid size-8 place-items-center rounded-lg">
				<Icon name="trash" class="size-4" />
			</span>
			Delete {pendingDelete?.name}
		</h2>
		<p class="text-base-content/70 mt-3 text-sm">
			The server refuses this while the bucket still holds objects, so nothing is deleted by
			accident.
		</p>
		<div class="modal-action">
			<button class="btn btn-sm" type="button" onclick={() => deleteDialog.close()}>Cancel</button>
			<button class="btn btn-error btn-sm" type="button" onclick={confirmDelete}>Delete</button>
		</div>
	</div>
	<form method="dialog" class="modal-backdrop"><button>close</button></form>
</dialog>
