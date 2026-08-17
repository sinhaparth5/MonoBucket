<script lang="ts">
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { flip } from 'svelte/animate';
	import { fade, fly } from 'svelte/transition';
	import { api, ApiError, type Bucket } from '$lib/api';
	import { formatTimestamp } from '$lib/format';

	let buckets = $state<Bucket[] | null>(null);
	let error = $state('');
	let busy = $state('');

	let newName = $state('');
	let createError = $state('');
	let createDialog: HTMLDialogElement;

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

	async function create(event: SubmitEvent) {
		event.preventDefault();
		createError = '';
		try {
			await api.createBucket(newName.trim());
			newName = '';
			createDialog.close();
			await load();
		} catch (cause) {
			createError = cause instanceof ApiError ? cause.message : 'could not create the bucket';
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
</script>

<svelte:head><title>Buckets · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-5">
	<div class="flex flex-wrap items-center justify-between gap-3">
		<h1 class="text-2xl font-semibold">Buckets</h1>
		<button class="btn btn-primary btn-sm" onclick={() => createDialog.showModal()}>
			Create bucket
		</button>
	</div>

	{#if error}
		<div role="alert" class="alert alert-error alert-soft" in:fly={{ y: -6, duration: 200 }}>
			<span>{error}</span>
		</div>
	{/if}

	{#if !buckets}
		<div class="skeleton rounded-box h-48"></div>
	{:else if buckets.length === 0}
		<div
			class="border-base-300 rounded-box flex flex-col items-start gap-3 border border-dashed p-8"
		>
			<p class="text-base-content/70">
				No buckets yet. Create one here, or with <code class="font-mono">aws s3 mb</code> against the
				S3 port.
			</p>
			<button class="btn btn-primary btn-sm" onclick={() => createDialog.showModal()}>
				Create the first bucket
			</button>
		</div>
	{:else}
		<div class="border-base-300 bg-base-100 rounded-box overflow-x-auto border">
			<table class="table table-sm">
				<thead>
					<tr>
						<th>Name</th>
						<th>Created</th>
						<th>Anonymous read</th>
						<th class="w-0"></th>
					</tr>
				</thead>
				<tbody>
					{#each buckets as bucket (bucket.name)}
						<tr animate:flip={{ duration: 200 }} in:fade={{ duration: 150 }}>
							<td>
								<a class="link link-hover font-medium" href={resolve(`/buckets/${bucket.name}`)}>
									{bucket.name}
								</a>
								{#if bucket.hasPolicy}
									<span class="badge badge-xs badge-ghost ml-2">policy</span>
								{/if}
							</td>
							<td class="text-base-content/70">{formatTimestamp(bucket.createdAtMs)}</td>
							<td>
								<input
									type="checkbox"
									class="toggle toggle-sm"
									checked={bucket.publicRead}
									disabled={busy === bucket.name}
									aria-label="Allow anonymous reads from {bucket.name}"
									onchange={(event) => toggleAccess(bucket, event.currentTarget.checked)}
								/>
							</td>
							<td>
								<button
									class="btn btn-ghost btn-xs text-error"
									disabled={busy === bucket.name}
									onclick={() => {
										pendingDelete = bucket;
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
	{/if}
</div>

<dialog bind:this={createDialog} class="modal">
	<div class="modal-box max-w-sm">
		<h2 class="text-lg font-medium">Create bucket</h2>
		<form class="mt-4 flex flex-col gap-4" onsubmit={create}>
			<fieldset class="fieldset gap-1 p-0">
				<legend class="fieldset-legend">Name</legend>
				<input
					class="input w-full"
					type="text"
					bind:value={newName}
					placeholder="my-bucket"
					required
				/>
				<p class="label">
					DNS rules apply: lowercase letters, digits, dots and hyphens, 3 to 63 characters.
				</p>
			</fieldset>

			{#if createError}
				<div role="alert" class="alert alert-error alert-soft text-sm">
					<span>{createError}</span>
				</div>
			{/if}

			<div class="modal-action">
				<button class="btn btn-sm" type="button" onclick={() => createDialog.close()}>Cancel</button
				>
				<button class="btn btn-primary btn-sm" type="submit">Create</button>
			</div>
		</form>
	</div>
	<form method="dialog" class="modal-backdrop"><button>close</button></form>
</dialog>

<dialog bind:this={deleteDialog} class="modal">
	<div class="modal-box max-w-sm">
		<h2 class="text-lg font-medium">Delete {pendingDelete?.name}</h2>
		<p class="text-base-content/70 mt-2 text-sm">
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
