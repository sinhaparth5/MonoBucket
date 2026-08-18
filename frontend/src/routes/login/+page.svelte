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
	import { fade, fly } from 'svelte/transition';
	import { api, ApiError } from '$lib/api';
	import { motionDelay, motionDistance, motionDuration } from '$lib/motion';
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
	class="text-base-content min-h-dvh lg:grid lg:grid-cols-[minmax(26rem,0.82fr)_minmax(0,1.18fr)]"
>
	<section
		class="bg-base-100/80 relative flex min-h-dvh items-center px-6 py-12 backdrop-blur-xl sm:px-10 lg:px-14"
	>
		<div
			aria-hidden="true"
			class="bg-primary/10 pointer-events-none absolute -top-32 -left-32 size-80 rounded-full blur-3xl"
		></div>

		<div
			class="relative mx-auto w-full max-w-md"
			in:fly={{ y: motionDistance(12), duration: motionDuration(280), opacity: 0.5 }}
		>
			<div class="mb-10 flex items-center gap-3">
				<img src={logo} alt="" class="size-12" width="48" height="48" />
				<div class="flex flex-col leading-tight">
					<span class="brand-text text-xl font-bold tracking-tight">MonoBucket</span>
					<span class="text-base-content/50 text-sm">Object storage console</span>
				</div>
			</div>

			<div class="mb-7">
				<span class="badge badge-primary badge-soft mb-4 gap-2">
					<span class="bg-primary size-1.5 rounded-full"></span>
					Self-hosted and S3 compatible
				</span>
				<h1 class="max-w-sm text-4xl leading-tight font-bold tracking-tight sm:text-5xl">
					Your storage, <span class="brand-text">in clear view.</span>
				</h1>
				<p class="text-base-content/60 mt-3 max-w-sm text-base leading-relaxed">
					Sign in with the root credentials configured for this MonoBucket instance.
				</p>
			</div>

			<form class="panel surface-raised flex flex-col gap-5 p-6 sm:p-7" onsubmit={submit}>
				<fieldset class="fieldset gap-1.5 p-0">
					<legend class="fieldset-legend text-sm">Access key</legend>
					<label class="input h-12 w-full">
						<Icon name="key" class="text-primary size-4.5" />
						<input
							type="text"
							autocomplete="username"
							spellcheck="false"
							bind:value={accessKey}
							placeholder="monobucket"
							required
						/>
					</label>
				</fieldset>

				<fieldset class="fieldset gap-1.5 p-0">
					<legend class="fieldset-legend text-sm">Secret key</legend>
					<label class="input h-12 w-full pr-1">
						<Icon name="shield" class="text-primary size-4.5" />
						<input
							type={showSecret ? 'text' : 'password'}
							autocomplete="current-password"
							bind:value={secretKey}
							placeholder="Enter your secret key"
							required
						/>
						<button
							type="button"
							class="btn btn-ghost size-10 p-0"
							aria-label={showSecret ? 'Hide the secret key' : 'Show the secret key'}
							onclick={() => (showSecret = !showSecret)}
						>
							<Icon name={showSecret ? 'eyeOff' : 'eye'} class="size-4.5" />
						</button>
					</label>
				</fieldset>

				{#if error}
					<div
						role="alert"
						class="alert alert-error alert-soft text-sm"
						in:fly={{ y: motionDistance(-6), duration: motionDuration(180) }}
					>
						<Icon name="warning" class="size-4" />
						<span>{error}</span>
					</div>
				{/if}

				<button
					class="btn btn-primary h-12 text-base shadow-lg shadow-primary/20"
					type="submit"
					disabled={busy}
				>
					{#if busy}<span class="loading loading-spinner loading-sm"></span>{/if}
					{busy ? 'Signing in…' : 'Sign in to console'}
				</button>

				<p class="text-base-content/50 text-xs leading-relaxed">
					The browser receives an HttpOnly session cookie. Your S3 secret stays on the server.
				</p>
			</form>
		</div>
	</section>

	<aside
		class="relative hidden min-h-dvh overflow-hidden p-4 lg:block"
		in:fade={{ delay: motionDelay(70), duration: motionDuration(320) }}
	>
		<img
			src="/images/console-login.webp"
			alt="Abstract file objects flowing into a colourful storage vessel"
			width="1536"
			height="1024"
			fetchpriority="high"
			class="absolute inset-4 size-[calc(100%_-_2rem)] rounded-[2rem] object-cover"
		/>
		<div
			class="absolute inset-4 rounded-[2rem] bg-gradient-to-t from-[#07091f]/90 via-transparent to-transparent"
		></div>
		<div class="absolute right-12 bottom-12 left-12 text-white">
			<p class="max-w-xl text-2xl leading-snug font-semibold">
				One binary for your S3 API, metadata, cache, and dashboard.
			</p>
			<div class="mt-5 flex flex-wrap gap-2">
				<span class="badge border-white/20 bg-white/10 text-white backdrop-blur"
					>Bounded memory</span
				>
				<span class="badge border-white/20 bg-white/10 text-white backdrop-blur">SigV4</span>
				<span class="badge border-white/20 bg-white/10 text-white backdrop-blur">Live metrics</span>
			</div>
		</div>
	</aside>
</div>
