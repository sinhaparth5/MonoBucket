<script lang="ts">
	// Deliberately plain. The console authenticates against the root credentials
	// from the environment and hands back a session cookie; accounts, roles and
	// key management are a later problem, and pretending otherwise here would
	// mean building a UI for a backend that does not exist.
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { fly } from 'svelte/transition';
	import { api, ApiError } from '$lib/api';

	let accessKey = $state('');
	let secretKey = $state('');
	let error = $state('');
	let busy = $state(false);

	async function submit(event: SubmitEvent) {
		event.preventDefault();
		if (busy) return;

		busy = true;
		error = '';
		try {
			await api.login(accessKey, secretKey);
			await goto(resolve('/'));
		} catch (cause) {
			error = cause instanceof ApiError ? cause.message : 'sign in failed';
		} finally {
			busy = false;
		}
	}
</script>

<svelte:head><title>Sign in · MonoBucket</title></svelte:head>

<div class="bg-base-200 text-base-content flex min-h-screen items-center justify-center p-6">
	<div class="w-full max-w-sm" in:fly={{ y: 12, duration: 300 }}>
		<div class="mb-6 flex flex-col gap-1">
			<span class="text-2xl font-semibold">MonoBucket</span>
			<span class="text-base-content/60">Sign in to the console.</span>
		</div>

		<form
			class="border-base-300 bg-base-100 rounded-box flex flex-col gap-4 border p-6"
			onsubmit={submit}
		>
			<fieldset class="fieldset gap-1 p-0">
				<legend class="fieldset-legend">Access key</legend>
				<input
					class="input w-full"
					type="text"
					autocomplete="username"
					bind:value={accessKey}
					required
				/>
			</fieldset>

			<fieldset class="fieldset gap-1 p-0">
				<legend class="fieldset-legend">Secret key</legend>
				<input
					class="input w-full"
					type="password"
					autocomplete="current-password"
					bind:value={secretKey}
					required
				/>
			</fieldset>

			{#if error}
				<div role="alert" class="alert alert-error alert-soft text-sm" in:fly={{ y: -6 }}>
					<span>{error}</span>
				</div>
			{/if}

			<button class="btn btn-primary" type="submit" disabled={busy}>
				{#if busy}<span class="loading loading-spinner loading-sm"></span>{/if}
				Sign in
			</button>

			<p class="text-base-content/50 text-xs">
				These are the credentials in <code class="font-mono">MONOBUCKET_ROOT_ACCESS_KEY</code> and
				<code class="font-mono">MONOBUCKET_ROOT_SECRET_KEY</code>.
			</p>
		</form>
	</div>
</div>
