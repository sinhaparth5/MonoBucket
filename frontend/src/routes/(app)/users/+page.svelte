<script lang="ts">
	// People, as opposed to the programs the access keys are for.
	//
	// Everything on this page is also enforced on the server — the role check
	// happens per route, not per button. What the hiding buys is that nobody has
	// to discover a rule by being refused by it, and the one place that would be
	// genuinely dangerous to only hide is the last-administrator guard, which is
	// a 409 from the server that this page renders rather than a rule it applies.
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { flip } from 'svelte/animate';
	import { fade, fly } from 'svelte/transition';
	import {
		api,
		ApiError,
		unrestrictedBuckets,
		type BucketAccessInfo,
		type BucketAccessName,
		type BucketGrants,
		type RoleInfo,
		type RoleName,
		type User
	} from '$lib/api';
	import { formatTimestamp, plural } from '$lib/format';
	import { motionDistance, motionDuration } from '$lib/motion';
	import BucketAccessFields from '$lib/components/BucketAccessFields.svelte';
	import Icon from '$lib/components/Icon.svelte';

	let { data } = $props();

	let users = $state<User[] | null>(null);
	let roles = $state<RoleInfo[]>([]);
	let accessLevels = $state<BucketAccessInfo[]>([]);
	// Bucket names, for naming an exception without typing it. Fetched
	// separately and tolerated as empty: a user list that failed because the
	// bucket list did would be a worse page than one with a shorter picker.
	let bucketNames = $state<string[]>([]);
	let error = $state('');
	let busy = $state('');

	let newUsername = $state('');
	let newPassword = $state('');
	let newRole = $state<RoleName>('readonly');
	let newBuckets = $state<BucketGrants>(unrestrictedBuckets());
	let createError = $state('');
	let creating = $state(false);
	let createDialog: HTMLDialogElement;
	let usernameField = $state<HTMLInputElement | undefined>();

	let accessTarget = $state<User | null>(null);
	let accessDraft = $state<BucketGrants>(unrestrictedBuckets());
	let accessError = $state('');
	let savingAccess = $state(false);
	let accessDialog: HTMLDialogElement;

	let resetTarget = $state<User | null>(null);
	let resetPassword = $state('');
	let resetError = $state('');
	let resetting = $state(false);
	let resetDialog: HTMLDialogElement;

	let pendingDelete = $state<User | null>(null);
	let deleteDialog: HTMLDialogElement;

	// The account you are signed in as. Named separately because three different
	// controls need to leave it alone, and comparing against `data.session`
	// inline three times invites one of them to be forgotten.
	const me = $derived(data.session.username);

	const ROLE_STYLE: Record<RoleName, string> = {
		administrator: 'badge-primary',
		operator: 'badge-secondary',
		readonly: 'badge-ghost'
	};

	const ACCESS_LABEL: Record<BucketAccessName, string> = {
		write: 'read and write',
		read: 'read only',
		none: 'no access'
	};

	/// How an account's bucket access reads in the table. An administrator is
	/// never narrowed, so saying "all buckets" for one would hide the reason.
	function summariseAccess(user: User): string {
		if (user.role === 'administrator') return 'all buckets (administrator)';
		if (user.buckets.unrestricted) return 'all buckets';
		const named = Object.keys(user.buckets.exceptions).length;
		const base = `${ACCESS_LABEL[user.buckets.fallback]} by default`;
		return named === 0 ? base : `${base}, ${plural(named, 'exception')}`;
	}

	async function load() {
		try {
			const answer = await api.users();
			users = answer.users;
			roles = answer.roles;
			accessLevels = answer.bucketAccess;
			error = '';
			try {
				bucketNames = (await api.buckets()).map((bucket) => bucket.name);
			} catch {
				bucketNames = [];
			}
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			error = cause instanceof ApiError ? cause.message : 'could not list users';
		}
	}

	$effect(() => {
		load();
	});

	function openCreate() {
		createError = '';
		newUsername = '';
		newPassword = '';
		newRole = 'readonly';
		// Unrestricted, and visibly so. The server would default to this anyway
		// if the field were omitted; showing it means an account with the run of
		// every bucket is something somebody looked at and left alone.
		newBuckets = unrestrictedBuckets();
		createDialog.showModal();
		usernameField?.focus();
	}

	async function create(event: SubmitEvent) {
		event.preventDefault();
		createError = '';
		creating = true;
		try {
			// An administrator is never narrowed, and the server refuses to store
			// a narrowing for one rather than keeping a restriction it ignores.
			await api.createUser(
				newUsername.trim(),
				newPassword,
				newRole,
				newRole === 'administrator' ? undefined : newBuckets
			);
			createDialog.close();
			await load();
		} catch (cause) {
			createError = cause instanceof ApiError ? cause.message : 'could not create the user';
		} finally {
			creating = false;
		}
	}

	// Not optimistic, unlike the bucket access toggle. A role change also ends
	// that person's sessions, so the response carries a consequence worth
	// reporting rather than only confirming what the control already showed.
	async function changeRole(user: User, role: RoleName) {
		if (role === user.role) return;
		busy = user.username;
		try {
			await api.updateUser(user.username, { role });
			error = '';
			await load();
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not change the role';
			await load();
		} finally {
			busy = '';
		}
	}

	async function toggleDisabled(user: User) {
		busy = user.username;
		try {
			await api.updateUser(user.username, { disabled: !user.disabled });
			error = '';
			await load();
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not change the account status';
			await load();
		} finally {
			busy = '';
		}
	}

	function openAccess(user: User) {
		accessTarget = user;
		accessDraft = { ...user.buckets, exceptions: { ...user.buckets.exceptions } };
		accessError = '';
		accessDialog.showModal();
	}

	async function confirmAccess(event: SubmitEvent) {
		event.preventDefault();
		if (!accessTarget) return;
		accessError = '';
		savingAccess = true;
		try {
			await api.updateUser(accessTarget.username, { buckets: accessDraft });
			accessDialog.close();
			accessTarget = null;
			await load();
		} catch (cause) {
			accessError = cause instanceof ApiError ? cause.message : 'could not change bucket access';
		} finally {
			savingAccess = false;
		}
	}

	function openReset(user: User) {
		resetTarget = user;
		resetPassword = '';
		resetError = '';
		resetDialog.showModal();
	}

	async function confirmReset(event: SubmitEvent) {
		event.preventDefault();
		if (!resetTarget) return;
		resetError = '';
		resetting = true;
		try {
			await api.setPassword(resetPassword, { username: resetTarget.username });
			resetDialog.close();
			resetTarget = null;
			await load();
		} catch (cause) {
			resetError = cause instanceof ApiError ? cause.message : 'could not reset the password';
		} finally {
			resetting = false;
		}
	}

	async function confirmDelete() {
		if (!pendingDelete) return;
		const username = pendingDelete.username;
		busy = username;
		try {
			await api.deleteUser(username);
			deleteDialog.close();
			pendingDelete = null;
			await load();
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'could not delete the user';
			deleteDialog.close();
		} finally {
			busy = '';
		}
	}
</script>

<svelte:head><title>Users · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-6">
	<header class="flex flex-wrap items-end justify-between gap-4">
		<div class="flex flex-col gap-1">
			<span class="eyebrow">Console access</span>
			<h1 class="text-3xl font-bold tracking-tight sm:text-4xl">Users</h1>
			{#if users}
				<p class="text-base-content/55 text-sm">{plural(users.length, 'account')}</p>
			{/if}
		</div>

		<button class="btn btn-primary shadow-primary/20 gap-2 shadow-lg" onclick={openCreate}>
			<Icon name="plus" class="size-4" />
			Add user
		</button>
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

	{#if !users}
		<div class="skeleton rounded-box h-48" out:fade={{ duration: motionDuration(100) }}></div>
	{:else}
		<div class="panel overflow-x-auto shadow-sm">
			<table class="table table-sm">
				<thead>
					<tr class="border-base-300">
						<th>User</th>
						<th class="w-56">Role</th>
						<th class="w-64">Buckets</th>
						<th class="w-32">Status</th>
						<th class="w-52">Password changed</th>
						<th class="w-0"></th>
					</tr>
				</thead>
				<tbody>
					{#each users as user (user.username)}
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
										<Icon name="users" class="size-4" />
									</span>
									<span class="flex flex-col leading-tight">
										<span class="text-sm font-medium">{user.username}</span>
										{#if user.username === me}
											<span class="text-base-content/50 text-xs">that is you</span>
										{/if}
									</span>
								</span>
							</td>
							<td>
								<select
									class="select select-sm w-full max-w-44"
									value={user.role}
									disabled={busy === user.username}
									aria-label="Role for {user.username}"
									onchange={(event) => changeRole(user, event.currentTarget.value as RoleName)}
								>
									{#each roles as role (role.name)}
										<option value={role.name}>{role.name}</option>
									{/each}
								</select>
							</td>
							<td>
								<button
									class="btn btn-ghost btn-sm w-full justify-start gap-2 font-normal"
									disabled={busy === user.username || user.role === 'administrator'}
									aria-label="Bucket access for {user.username}"
									onclick={() => openAccess(user)}
								>
									<Icon
										name={user.buckets.unrestricted ? 'bucket' : 'shield'}
										class="size-3.5 shrink-0 {user.buckets.unrestricted
											? 'text-base-content/50'
											: 'text-primary'}"
									/>
									<span class="truncate text-xs">{summariseAccess(user)}</span>
								</button>
							</td>
							<td>
								<span
									class="badge badge-sm {user.disabled ? 'badge-ghost' : ROLE_STYLE[user.role]}"
								>
									{user.disabled ? 'disabled' : 'active'}
								</span>
							</td>
							<td class="text-base-content/60">
								{user.passwordChangedAtMs > 0 ? formatTimestamp(user.passwordChangedAtMs) : 'never'}
							</td>
							<td>
								<div class="flex items-center justify-end gap-1">
									<button
										class="btn btn-ghost btn-sm gap-1.5"
										disabled={busy === user.username}
										onclick={() => openReset(user)}
									>
										<Icon name="shield" class="size-3.5" />
										Reset password
									</button>
									<button
										class="btn btn-ghost btn-sm gap-1.5"
										disabled={busy === user.username || user.username === me}
										onclick={() => toggleDisabled(user)}
									>
										<Icon name={user.disabled ? 'check' : 'close'} class="size-3.5" />
										{user.disabled ? 'Enable' : 'Disable'}
									</button>
									<button
										class="btn btn-ghost btn-sm text-error gap-1.5"
										disabled={busy === user.username || user.username === me}
										onclick={() => {
											pendingDelete = user;
											deleteDialog.showModal();
										}}
									>
										<Icon name="trash" class="size-3.5" />
										Delete
									</button>
								</div>
							</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</div>

		<section class="panel surface-raised flex flex-col gap-4 p-6">
			<div class="flex flex-col gap-1">
				<span class="eyebrow">Reference</span>
				<h2 class="text-xl font-bold tracking-tight">What each role can do</h2>
				<p class="text-base-content/60 text-sm">
					Sent by the server, so this list is what is actually enforced. An S3 access key acts as
					the person who issued it and can never do more than they can.
				</p>
			</div>

			<div class="grid gap-3 md:grid-cols-3">
				{#each roles as role (role.name)}
					<div class="border-base-300 flex flex-col gap-3 rounded-xl border p-4">
						<span class="badge {ROLE_STYLE[role.name]} badge-soft self-start">{role.name}</span>
						<p class="text-base-content/70 text-sm leading-relaxed">{role.description}</p>
						<div class="flex flex-wrap gap-1">
							{#each role.permissions as permission (permission)}
								<span class="badge badge-ghost badge-sm font-mono text-xs">{permission}</span>
							{/each}
						</div>
					</div>
				{/each}
			</div>
		</section>
	{/if}
</div>

<dialog class="modal" bind:this={createDialog}>
	<form class="modal-box flex flex-col gap-4" onsubmit={create}>
		<h3 class="text-lg font-bold tracking-tight">Add a user</h3>
		<p class="text-base-content/60 text-sm">
			They sign in to this console with the password you set here. It is not an S3 credential and
			will not sign a request.
		</p>

		<fieldset class="fieldset gap-1.5 p-0">
			<legend class="fieldset-legend text-sm">Username</legend>
			<label class="input w-full">
				<Icon name="users" class="text-primary size-4" />
				<input
					type="text"
					maxlength="64"
					spellcheck="false"
					autocomplete="off"
					bind:this={usernameField}
					bind:value={newUsername}
					placeholder="sam"
					required
				/>
			</label>
			<span class="label text-xs">Letters, digits, dot, underscore or hyphen.</span>
		</fieldset>

		<fieldset class="fieldset gap-1.5 p-0">
			<legend class="fieldset-legend text-sm">Password</legend>
			<label class="input w-full">
				<Icon name="shield" class="text-primary size-4" />
				<input
					type="password"
					autocomplete="new-password"
					minlength="12"
					bind:value={newPassword}
					placeholder="At least 12 characters"
					required
				/>
			</label>
			<span class="label text-xs">They can change it themselves once they are signed in.</span>
		</fieldset>

		<fieldset class="fieldset gap-1.5 p-0">
			<legend class="fieldset-legend text-sm">Role</legend>
			<select class="select w-full" bind:value={newRole}>
				{#each roles as role (role.name)}
					<option value={role.name}>{role.name} — {role.description}</option>
				{/each}
			</select>
		</fieldset>

		{#if newRole === 'administrator'}
			<div role="note" class="alert alert-info alert-soft text-sm">
				<Icon name="shield" class="size-4" />
				<span>
					An administrator reaches every bucket. Bucket access is not narrowed for one, because an
					administrator can change it back.
				</span>
			</div>
		{:else}
			<BucketAccessFields bind:grants={newBuckets} buckets={bucketNames} levels={accessLevels} />
		{/if}

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
				Add user
			</button>
		</div>
	</form>
	<form method="dialog" class="modal-backdrop"><button aria-label="Close">close</button></form>
</dialog>

<dialog class="modal" bind:this={accessDialog}>
	<form class="modal-box flex flex-col gap-4" onsubmit={confirmAccess}>
		<h3 class="text-lg font-bold tracking-tight">
			Bucket access for {accessTarget?.username}
		</h3>
		<p class="text-base-content/60 text-sm">
			This narrows their role, it never widens it — a readonly account still only reads. Their S3
			access keys are held to the same list, and every session they have open ends when you save.
		</p>

		<BucketAccessFields bind:grants={accessDraft} buckets={bucketNames} levels={accessLevels} />

		{#if accessError}
			<div role="alert" class="alert alert-error alert-soft text-sm">
				<Icon name="warning" class="size-4" />
				<span>{accessError}</span>
			</div>
		{/if}

		<div class="modal-action">
			<button type="button" class="btn btn-ghost" onclick={() => accessDialog.close()}>
				Cancel
			</button>
			<button class="btn btn-primary gap-2" type="submit" disabled={savingAccess}>
				{#if savingAccess}<span class="loading loading-spinner loading-xs"></span>{/if}
				Save access
			</button>
		</div>
	</form>
	<form method="dialog" class="modal-backdrop"><button aria-label="Close">close</button></form>
</dialog>

<dialog class="modal" bind:this={resetDialog}>
	<form class="modal-box flex flex-col gap-4" onsubmit={confirmReset}>
		<h3 class="text-lg font-bold tracking-tight">
			Reset the password for {resetTarget?.username}
		</h3>
		<p class="text-base-content/60 text-sm">
			Every session that account has open ends immediately, and their S3 access keys are untouched.
		</p>

		<fieldset class="fieldset gap-1.5 p-0">
			<legend class="fieldset-legend text-sm">New password</legend>
			<label class="input w-full">
				<Icon name="shield" class="text-primary size-4" />
				<input
					type="password"
					autocomplete="new-password"
					minlength="12"
					bind:value={resetPassword}
					placeholder="At least 12 characters"
					required
				/>
			</label>
		</fieldset>

		{#if resetError}
			<div role="alert" class="alert alert-error alert-soft text-sm">
				<Icon name="warning" class="size-4" />
				<span>{resetError}</span>
			</div>
		{/if}

		<div class="modal-action">
			<button type="button" class="btn btn-ghost" onclick={() => resetDialog.close()}>
				Cancel
			</button>
			<button class="btn btn-primary gap-2" type="submit" disabled={resetting}>
				{#if resetting}<span class="loading loading-spinner loading-xs"></span>{/if}
				Reset password
			</button>
		</div>
	</form>
	<form method="dialog" class="modal-backdrop"><button aria-label="Close">close</button></form>
</dialog>

<dialog class="modal" bind:this={deleteDialog}>
	<div class="modal-box flex flex-col gap-4">
		<h3 class="text-lg font-bold tracking-tight">Delete {pendingDelete?.username}?</h3>
		<div role="alert" class="alert alert-warning alert-soft text-sm">
			<Icon name="warning" class="size-4" />
			<span>
				Their S3 access keys are revoked along with the account, so anything still signing requests
				with one stops working. Disabling instead keeps the keys and can be undone.
			</span>
		</div>

		<div class="modal-action">
			<button type="button" class="btn btn-ghost" onclick={() => deleteDialog.close()}>
				Cancel
			</button>
			<button
				class="btn btn-error gap-2"
				disabled={busy === pendingDelete?.username}
				onclick={confirmDelete}
			>
				<Icon name="trash" class="size-4" />
				Delete user
			</button>
		</div>
	</div>
	<form method="dialog" class="modal-backdrop"><button aria-label="Close">close</button></form>
</dialog>
