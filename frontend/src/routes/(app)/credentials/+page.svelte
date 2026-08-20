<script lang="ts">
	// S3 credentials, managed by someone who is signed in with a password. The
	// screen exists because the two used to be the same thing: rotating the key
	// a script used meant changing how a person logged in, so in practice nobody
	// rotated anything.
	//
	// One rule shapes the whole page — a secret is returned exactly once, by the
	// call that created or rotated it. Nothing here re-fetches one, because
	// nothing can. That is why the reveal is a dialog that warns before it
	// closes rather than a column in the table.
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { flip } from 'svelte/animate';
	import { fade, fly } from 'svelte/transition';
	import { api, ApiError, can, type Credential, type IssuedCredential } from '$lib/api';
	import { formatTimestamp, plural } from '$lib/format';
	import { motionDistance, motionDuration } from '$lib/motion';
	import Icon from '$lib/components/Icon.svelte';

	let { data } = $props();

	// An administrator sees everybody's keys, so the owner column only earns its
	// width for them — for anyone else every row would say the same name.
	const mayWrite = $derived(can(data.session, 'credential:write'));
	const seesEveryone = $derived(can(data.session, 'user:read'));

	let credentials = $state<Credential[] | null>(null);
	let error = $state('');
	let busy = $state('');

	let name = $state('');
	let createError = $state('');
	let creating = $state(false);
	let createDialog: HTMLDialogElement;
	let nameField = $state<HTMLInputElement | undefined>();

	// The only place a secret exists in this browser, and only until the dialog
	// closes. Never written to storage: a copy that outlives the dialog is a
	// copy the "shown once" promise does not cover.
	let issued = $state<IssuedCredential | null>(null);
	let issuedKind = $state<'created' | 'rotated'>('created');
	let secretDialog: HTMLDialogElement;
	let copied = $state('');

	// What a key is called in this console. The id identifies it to S3 and to
	// nothing else — twenty characters that tell a reader nothing about which
	// job holds them — so the name leads and the id sits under it, still
	// selectable for whoever has to match it against a client's configuration.
	// Keys minted before names existed have none, and fall back to the id
	// rather than to a blank row.
	function keyLabel(credential: Credential) {
		return credential.name || credential.accessKeyId;
	}

	let pendingRevoke = $state<Credential | null>(null);
	let revokeDialog: HTMLDialogElement;
	let pendingRotate = $state<Credential | null>(null);
	let rotateDialog: HTMLDialogElement;

	async function load() {
		try {
			credentials = await api.credentials();
			error = '';
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			error = cause instanceof ApiError ? cause.message : 'could not list access keys';
		}
	}

	$effect(() => {
		load();
	});

	function openCreate() {
		createError = '';
		createDialog.showModal();
		nameField?.focus();
	}

	function reveal(credential: IssuedCredential, kind: 'created' | 'rotated') {
		issued = credential;
		issuedKind = kind;
		copied = '';
		secretDialog.showModal();
	}

	async function create(event: SubmitEvent) {
		event.preventDefault();
		createError = '';
		creating = true;
		try {
			const credential = await api.createCredential(name.trim());
			name = '';
			createDialog.close();
			reveal(credential, 'created');
			await load();
		} catch (cause) {
			createError = cause instanceof ApiError ? cause.message : 'could not create the access key';
		} finally {
			creating = false;
		}
	}

	async function confirmRotate() {
		if (!pendingRotate) return;
		const id = pendingRotate.accessKeyId;
		busy = id;
		try {
			const credential = await api.rotateCredential(id);
			rotateDialog.close();
			pendingRotate = null;
			reveal(credential, 'rotated');
			await load();
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not rotate the access key';
			rotateDialog.close();
		} finally {
			busy = '';
		}
	}

	async function confirmRevoke() {
		if (!pendingRevoke) return;
		const id = pendingRevoke.accessKeyId;
		busy = id;
		try {
			await api.revokeCredential(id);
			revokeDialog.close();
			pendingRevoke = null;
			await load();
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not revoke the access key';
			revokeDialog.close();
		} finally {
			busy = '';
		}
	}

	// Best effort. The clipboard API is unavailable over plain HTTP on some
	// browsers, which is exactly the deployment this console expects, so the
	// secret stays selectable and the button simply says nothing happened.
	async function copy(label: string, value: string) {
		try {
			await navigator.clipboard.writeText(value);
			copied = label;
		} catch {
			copied = '';
		}
	}

	function closeSecret() {
		secretDialog.close();
		issued = null;
		copied = '';
	}
</script>

<svelte:head><title>Access keys · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-6">
	<header class="flex flex-wrap items-end justify-between gap-4">
		<div class="flex flex-col gap-1">
			<span class="eyebrow">S3 credentials</span>
			<h1 class="text-3xl font-bold tracking-tight sm:text-4xl">Access keys</h1>
			{#if credentials}
				<p class="text-base-content/55 text-sm">{plural(credentials.length, 'active key')}</p>
			{/if}
		</div>

		{#if mayWrite}
			<button class="btn btn-primary gap-2 shadow-primary/20 shadow-lg" onclick={openCreate}>
				<Icon name="plus" class="size-4" />
				Create access key
			</button>
		{/if}
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

	{#if !credentials}
		<div class="skeleton rounded-box h-48" out:fade={{ duration: motionDuration(100) }}></div>
	{:else if credentials.length === 0}
		<div
			class="panel surface-raised flex flex-col items-center gap-4 p-8 text-center"
			in:fly={{ y: motionDistance(10), duration: motionDuration(240), opacity: 0.5 }}
		>
			<span class="bg-primary/12 text-primary grid size-14 place-items-center rounded-2xl">
				<Icon name="key" class="size-7" />
			</span>
			<div class="flex flex-col gap-1">
				<p class="text-xl font-bold tracking-tight">No access keys yet</p>
				<p class="text-base-content/60 mx-auto max-w-md text-sm">
					Create one to give a program access to the S3 API. A key acts as the person who issued it
					and can never do more than they can, so revoking one never locks anybody out of this
					console.
				</p>
			</div>
			{#if mayWrite}
				<button class="btn btn-primary gap-2" onclick={openCreate}>
					<Icon name="plus" class="size-4" />
					Create the first key
				</button>
			{/if}
		</div>
	{:else}
		<div class="panel overflow-x-auto shadow-sm">
			<table class="table table-sm">
				<thead>
					<tr class="border-base-300">
						<th>Name</th>
						{#if seesEveryone}<th class="w-40">Owner</th>{/if}
						<th class="w-52">Created</th>
						<th class="w-52">Secret rotated</th>
						<th class="w-0"></th>
					</tr>
				</thead>
				<tbody>
					{#each credentials as credential (credential.accessKeyId)}
						<tr
							class="hover:bg-base-200/70 transition-colors"
							animate:flip={{ duration: motionDuration(200) }}
							in:fade={{ duration: motionDuration(150) }}
						>
							<td>
								<span class="flex items-center gap-3">
									<span
										class="bg-primary/12 text-primary grid size-8 shrink-0 place-items-center rounded-lg"
									>
										<Icon name="key" class="size-4" />
									</span>
									<span class="flex min-w-0 flex-col">
										<span class="truncate text-sm font-medium">{keyLabel(credential)}</span>
										{#if credential.name}
											<span class="text-base-content/50 font-mono text-xs">
												{credential.accessKeyId}
											</span>
										{/if}
									</span>
								</span>
							</td>
							{#if seesEveryone}
								<td class="text-base-content/70 text-sm">{credential.owner || '—'}</td>
							{/if}
							<td class="text-base-content/60">{formatTimestamp(credential.createdAtMs)}</td>
							<td class="text-base-content/60">
								{credential.rotatedAtMs > 0 ? formatTimestamp(credential.rotatedAtMs) : 'never'}
							</td>
							<td>
								<div class="flex items-center justify-end gap-1">
									{#if mayWrite}
										<button
											class="btn btn-ghost btn-sm gap-1.5"
											disabled={busy === credential.accessKeyId}
											onclick={() => {
												pendingRotate = credential;
												rotateDialog.showModal();
											}}
										>
											<Icon name="refresh" class="size-3.5" />
											Rotate
										</button>
										<button
											class="btn btn-ghost btn-sm text-error gap-1.5"
											disabled={busy === credential.accessKeyId}
											onclick={() => {
												pendingRevoke = credential;
												revokeDialog.showModal();
											}}
										>
											<Icon name="trash" class="size-3.5" />
											Revoke
										</button>
									{:else}
										<span class="text-base-content/45 text-xs">read only</span>
									{/if}
								</div>
							</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</div>
	{/if}
</div>

<dialog class="modal" bind:this={createDialog}>
	<form class="modal-box flex flex-col gap-4" onsubmit={create}>
		<h3 class="text-lg font-bold tracking-tight">Create an access key</h3>
		<p class="text-base-content/60 text-sm">
			The secret is shown once, on the next screen. MonoBucket cannot show it again.
		</p>

		<fieldset class="fieldset gap-1.5 p-0">
			<legend class="fieldset-legend text-sm">Name</legend>
			<label class="input w-full">
				<Icon name="file" class="text-primary size-4" />
				<input
					type="text"
					maxlength="200"
					bind:this={nameField}
					bind:value={name}
					placeholder="Nightly backup job"
				/>
			</label>
			<span class="label text-xs">
				What this key is called in the list. Leave it out and it shows its id instead.
			</span>
		</fieldset>

		{#if createError}
			<div role="alert" class="alert alert-error alert-soft text-sm">
				<Icon name="warning" class="size-4" />
				<span>{createError}</span>
			</div>
		{/if}

		<div class="modal-action">
			<button type="button" class="btn btn-ghost" onclick={() => createDialog.close()}>
				Cancel
			</button>
			<button class="btn btn-primary gap-2" type="submit" disabled={creating}>
				{#if creating}<span class="loading loading-spinner loading-xs"></span>{/if}
				Create key
			</button>
		</div>
	</form>
	<form method="dialog" class="modal-backdrop"><button aria-label="Close">close</button></form>
</dialog>

<dialog class="modal" bind:this={secretDialog}>
	<div class="modal-box flex flex-col gap-4">
		<h3 class="text-lg font-bold tracking-tight">
			{issuedKind === 'created' ? 'Access key created' : 'Secret rotated'}
		</h3>

		<div role="alert" class="alert alert-warning alert-soft text-sm">
			<Icon name="warning" class="size-4" />
			<span>
				Copy the secret now. It is not stored anywhere this console can read, so closing this dialog
				is the last time you will see it.
			</span>
		</div>

		{#if issuedKind === 'rotated'}
			<p class="text-base-content/60 text-sm">
				The previous secret stopped working immediately. Anything still using it will get
				<code class="bg-base-200 rounded px-1 py-0.5 font-mono text-xs">SignatureDoesNotMatch</code> until
				it is updated.
			</p>
		{/if}

		{#if issued}
			<fieldset class="fieldset gap-1.5 p-0">
				<legend class="fieldset-legend text-sm">Access key ID</legend>
				<div class="join w-full">
					<input
						class="input join-item w-full font-mono text-sm"
						readonly
						value={issued.accessKeyId}
						aria-label="Access key ID"
					/>
					<button
						type="button"
						class="btn join-item gap-1.5"
						onclick={() => copy('id', issued?.accessKeyId ?? '')}
					>
						<Icon name={copied === 'id' ? 'check' : 'copy'} class="size-4" />
					</button>
				</div>
			</fieldset>

			<fieldset class="fieldset gap-1.5 p-0">
				<legend class="fieldset-legend text-sm">Secret access key</legend>
				<div class="join w-full">
					<input
						class="input join-item w-full font-mono text-sm"
						readonly
						value={issued.secretKey}
						aria-label="Secret access key"
					/>
					<button
						type="button"
						class="btn join-item gap-1.5"
						onclick={() => copy('secret', issued?.secretKey ?? '')}
					>
						<Icon name={copied === 'secret' ? 'check' : 'copy'} class="size-4" />
					</button>
				</div>
			</fieldset>
		{/if}

		<div class="modal-action">
			<button type="button" class="btn btn-primary" onclick={closeSecret}>
				I have saved the secret
			</button>
		</div>
	</div>
</dialog>

<dialog class="modal" bind:this={rotateDialog}>
	<div class="modal-box flex flex-col gap-4">
		<h3 class="text-lg font-bold tracking-tight">Rotate this secret?</h3>
		<p class="text-base-content/70 text-sm">
			<span class="font-medium">{pendingRotate ? keyLabel(pendingRotate) : ''}</span> keeps its ID (<span
				class="font-mono text-xs">{pendingRotate?.accessKeyId}</span
			>) and gets a new secret. Every client still using the old one starts failing straight away —
			that is what rotation is for, so make sure you can update them.
		</p>
		<div class="modal-action">
			<button
				type="button"
				class="btn btn-ghost"
				onclick={() => {
					rotateDialog.close();
					pendingRotate = null;
				}}
			>
				Cancel
			</button>
			<button type="button" class="btn btn-warning gap-2" onclick={confirmRotate}>
				<Icon name="refresh" class="size-4" />
				Rotate secret
			</button>
		</div>
	</div>
</dialog>

<dialog class="modal" bind:this={revokeDialog}>
	<div class="modal-box flex flex-col gap-4">
		<h3 class="text-lg font-bold tracking-tight">Revoke this access key?</h3>
		<p class="text-base-content/70 text-sm">
			<span class="font-medium">{pendingRevoke ? keyLabel(pendingRevoke) : ''}</span>
			(<span class="font-mono text-xs">{pendingRevoke?.accessKeyId}</span>) stops signing requests
			on the next one it makes. The key is deleted rather than disabled, so this cannot be undone.
		</p>
		<div class="modal-action">
			<button
				type="button"
				class="btn btn-ghost"
				onclick={() => {
					revokeDialog.close();
					pendingRevoke = null;
				}}
			>
				Cancel
			</button>
			<button type="button" class="btn btn-error gap-2" onclick={confirmRevoke}>
				<Icon name="trash" class="size-4" />
				Revoke key
			</button>
		</div>
	</div>
</dialog>
