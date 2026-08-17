<script lang="ts">
	// Deliberately plain in what it asks for. The console authenticates against
	// the root credentials from the environment and hands back a session cookie;
	// accounts, roles and key management are a later problem, and pretending
	// otherwise here would mean building a UI for a backend that does not exist.
	//
	// It is the first screen anyone sees, though, so it wears the same mark and
	// the same surfaces as the console behind it rather than looking like a
	// framework default that nobody got to.
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { fly } from 'svelte/transition';
	import { api, ApiError } from '$lib/api';
	import Icon from '$lib/components/Icon.svelte';
	import logo from '$lib/assets/monobucket-logo.svg';

	let accessKey = $state('');
	let secretKey = $state('');
	let showSecret = $state(false);
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

<div
	class="bg-base-200 text-base-content relative flex min-h-dvh items-center justify-center overflow-hidden p-6"
>
	<!-- Two soft washes off the brand hues. Decoration, so it is hidden from
	     assistive technology and sits behind everything. -->
	<div
		aria-hidden="true"
		class="bg-primary/12 pointer-events-none absolute -top-40 -left-40 size-96 rounded-full blur-3xl"
	></div>
	<div
		aria-hidden="true"
		class="bg-secondary/12 pointer-events-none absolute -right-40 -bottom-40 size-96 rounded-full blur-3xl"
	></div>

	<div class="relative w-full max-w-sm" in:fly={{ y: 12, duration: 300 }}>
		<div class="mb-6 flex items-center gap-3">
			<img src={logo} alt="" class="size-12" />
			<div class="flex flex-col leading-tight">
				<span class="text-xl font-semibold tracking-tight">MonoBucket</span>
				<span class="text-base-content/55 text-sm">Sign in to the console.</span>
			</div>
		</div>

		<form class="panel surface-raised flex flex-col gap-4 p-6 shadow-sm" onsubmit={submit}>
			<fieldset class="fieldset gap-1 p-0">
				<legend class="fieldset-legend">Access key</legend>
				<label class="input w-full">
					<Icon name="key" class="size-4 opacity-50" />
					<input
						type="text"
						autocomplete="username"
						spellcheck="false"
						bind:value={accessKey}
						required
					/>
				</label>
			</fieldset>

			<fieldset class="fieldset gap-1 p-0">
				<legend class="fieldset-legend">Secret key</legend>
				<label class="input w-full">
					<Icon name="shield" class="size-4 opacity-50" />
					<!-- Reveal, because a secret pasted from a password manager that
					     silently picked up a trailing space is otherwise diagnosed as
					     "invalid credentials". -->
					<input
						type={showSecret ? 'text' : 'password'}
						autocomplete="current-password"
						bind:value={secretKey}
						required
					/>
					<button
						type="button"
						class="opacity-50 transition-opacity hover:opacity-100"
						aria-label={showSecret ? 'Hide the secret key' : 'Show the secret key'}
						onclick={() => (showSecret = !showSecret)}
					>
						<Icon name={showSecret ? 'eyeOff' : 'eye'} class="size-4" />
					</button>
				</label>
			</fieldset>

			{#if error}
				<div role="alert" class="alert alert-error alert-soft text-sm" in:fly={{ y: -6 }}>
					<Icon name="warning" class="size-4" />
					<span>{error}</span>
				</div>
			{/if}

			<button class="btn btn-primary" type="submit" disabled={busy}>
				{#if busy}<span class="loading loading-spinner loading-sm"></span>{/if}
				Sign in
			</button>

			<p class="text-base-content/50 text-xs">
				These are the credentials in <code class="font-mono">MONOBUCKET_ROOT_ACCESS_KEY</code> and
				<code class="font-mono">MONOBUCKET_ROOT_SECRET_KEY</code>. The browser is given a session
				cookie, never an S3 secret.
			</p>
		</form>
	</div>
</div>
