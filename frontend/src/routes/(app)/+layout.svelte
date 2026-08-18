<script lang="ts">
	import { goto } from '$app/navigation';
	import { page } from '$app/state';
	import { resolve } from '$app/paths';
	import { fly, scale } from 'svelte/transition';
	import { api } from '$lib/api';
	import { motionDistance, motionDuration } from '$lib/motion';
	import { theme, type ThemeChoice } from '$lib/theme.svelte';
	import Icon, { type IconName } from '$lib/components/Icon.svelte';
	import logo from '$lib/assets/monobucket-logo.svg';

	let { data, children } = $props();

	// `lg:drawer-open` pins the sidebar on a desktop and leaves it collapsible on
	// a phone, which is the only responsive behaviour this shell needs.
	// `as const` so each href stays a literal: `resolve()` is typed against the
	// route table, and a widened `string` would take the whole nav out of that
	// check — which is the check that catches a route renamed out from under it.
	const NAV = [
		{ href: '/', label: 'Overview', icon: 'overview', hint: 'Live traffic, memory and capacity' },
		{
			href: '/buckets',
			label: 'Buckets',
			icon: 'bucket',
			hint: 'Browse, upload and share objects'
		},
		{
			href: '/credentials',
			label: 'Access keys',
			icon: 'key',
			hint: 'Issue, rotate and revoke S3 credentials'
		},
		{ href: '/settings', label: 'Settings', icon: 'settings', hint: 'The resolved configuration' }
	] as const satisfies readonly { href: string; label: string; icon: IconName; hint: string }[];

	const currentNav = $derived(
		NAV.find((item) =>
			item.href === '/' ? page.url.pathname === '/' : page.url.pathname.startsWith(item.href)
		) ?? NAV[0]
	);

	// `/` would otherwise light up on every route.
	function isActive(href: string): boolean {
		return href === '/' ? page.url.pathname === '/' : page.url.pathname.startsWith(href);
	}

	const THEME_CHOICES: { value: ThemeChoice; label: string; icon: IconName }[] = [
		{ value: 'system', label: 'Match the system', icon: 'settings' },
		{ value: 'light', label: 'Light', icon: 'sun' },
		{ value: 'dark', label: 'Dark', icon: 'moon' }
	];

	let signingOut = $state(false);

	async function signOut() {
		signingOut = true;
		try {
			await api.logout();
		} finally {
			await goto(resolve('/login'));
		}
	}

	// Closes the drawer after a tap on a phone, where the sidebar is an overlay
	// and would otherwise stay across the page it just navigated to.
	let drawerToggle = $state<HTMLInputElement | undefined>();
</script>

<a
	class="btn btn-primary btn-sm fixed top-2 left-2 z-50 -translate-y-20 focus:translate-y-0"
	href="#main-content"
>
	Skip to content
</a>

<div class="drawer lg:drawer-open min-h-screen">
	<input bind:this={drawerToggle} id="console-drawer" type="checkbox" class="drawer-toggle" />

	<div class="drawer-content flex min-h-screen min-w-0 flex-col">
		<header
			class="navbar border-base-300 bg-base-100/80 sticky top-0 z-20 min-h-16 gap-3 border-b px-3 backdrop-blur-xl sm:px-6"
		>
			<label
				for="console-drawer"
				class="btn btn-ghost drawer-button size-11 p-0 lg:hidden"
				aria-label="Open navigation"
			>
				<Icon name="menu" />
			</label>

			<span class="flex items-center gap-2.5 font-semibold lg:hidden">
				<img src={logo} alt="" class="size-8" width="32" height="32" />
				MonoBucket
			</span>

			<div class="hidden min-w-0 flex-col lg:flex">
				<span class="font-semibold tracking-tight">{currentNav.label}</span>
				<span class="text-base-content/50 truncate text-xs">{currentNav.hint}</span>
			</div>

			<div class="ml-auto flex items-center gap-1.5 sm:gap-2">
				<span class="badge badge-success badge-soft hidden gap-1.5 sm:flex">
					<span class="bg-success size-1.5 rounded-full"></span>
					Connected
				</span>
				<!-- Three states, not two: someone who has never touched this should
				     keep following the operating system, and a two-way switch has
				     nowhere to put that. -->
				<div class="dropdown dropdown-end">
					<div
						tabindex="0"
						role="button"
						class="btn btn-ghost size-10 p-0"
						aria-label="Appearance"
						title="Appearance"
					>
						{#key theme.choice}
							<span
								class="grid place-items-center"
								in:scale={{ start: 0.86, duration: motionDuration(180) }}
							>
								<Icon name={theme.choice === 'dark' ? 'moon' : 'sun'} />
							</span>
						{/key}
					</div>
					<ul
						class="dropdown-content menu menu-sm bg-base-100 rounded-box border-base-300 z-30 mt-2 w-52 border p-1 shadow-lg"
					>
						{#each THEME_CHOICES as option (option.value)}
							<li>
								<button
									class={theme.choice === option.value ? 'menu-active' : ''}
									onclick={() => theme.set(option.value)}
								>
									<Icon name={option.icon} />
									{option.label}
									{#if theme.choice === option.value}
										<Icon name="check" class="ml-auto size-3.5" />
									{/if}
								</button>
							</li>
						{/each}
					</ul>
				</div>

				<div class="divider divider-horizontal mx-0 hidden h-5 self-center sm:flex"></div>

				<span class="text-base-content/55 hidden max-w-40 truncate text-xs md:inline">
					{data.session.username}
				</span>

				<button
					class="btn btn-ghost size-10 p-0"
					onclick={signOut}
					disabled={signingOut}
					title="Sign out"
				>
					<Icon name="signOut" />
					<span class="sr-only">Sign out</span>
				</button>
			</div>
		</header>

		<main id="main-content" class="min-w-0 flex-1 p-4 sm:p-6 lg:p-8">
			<div class="mx-auto w-full max-w-7xl">
				{#key page.url.pathname}
					<div
						in:fly={{
							y: motionDistance(8),
							opacity: 0.55,
							duration: motionDuration(220)
						}}
					>
						{@render children()}
					</div>
				{/key}
			</div>
		</main>
	</div>

	<div class="drawer-side z-30">
		<label for="console-drawer" aria-label="Close navigation" class="drawer-overlay"></label>

		<div
			class="bg-base-100/90 border-base-300 flex min-h-full w-72 flex-col border-r backdrop-blur-xl"
		>
			<div class="border-base-300 surface-raised flex min-h-16 items-center gap-3 border-b px-5">
				<img src={logo} alt="" class="size-10" width="40" height="40" />
				<span class="flex flex-col leading-tight">
					<span class="brand-text text-base font-bold tracking-tight">MonoBucket</span>
					<span class="text-base-content/50 text-xs">Storage console</span>
				</span>
			</div>

			<div class="eyebrow px-5 pt-6 pb-2">Workspace</div>
			<ul class="menu w-full grow gap-1 px-3 py-1">
				{#each NAV as item (item.href)}
					<li>
						<a
							href={resolve(item.href)}
							title={item.hint}
							class="min-h-12 gap-3 rounded-xl px-3 {isActive(item.href)
								? 'bg-primary text-primary-content shadow-primary/20 font-semibold shadow-lg'
								: 'text-base-content/65 hover:bg-base-200 hover:text-base-content'}"
							onclick={() => {
								if (drawerToggle) drawerToggle.checked = false;
							}}
						>
							<span
								class="grid size-8 place-items-center rounded-lg {isActive(item.href)
									? 'bg-primary-content/12'
									: 'bg-base-200'}"
							>
								<Icon name={item.icon} class="size-4.5" />
							</span>
							<span class="flex flex-col">
								<span>{item.label}</span>
								<span class="text-[0.6875rem] font-normal opacity-65">{item.hint}</span>
							</span>
						</a>
					</li>
				{/each}
			</ul>

			{#if data.session.usingDefaultCredentials}
				<div role="alert" class="alert alert-warning alert-soft m-2 gap-2 text-xs">
					<Icon name="warning" class="size-4" />
					<span>
						The built-in demo S3 key pair is in use. Change it before exposing this server.
					</span>
				</div>
			{/if}

			<div class="border-base-300 flex items-center justify-between border-t px-5 py-4 text-xs">
				<span class="text-base-content/45 font-mono">v{data.session.version}</span>
				<span class="text-success flex items-center gap-1.5 font-medium">
					<span class="bg-success size-2 rounded-full shadow-[0_0_8px_currentColor]"></span>
					Online
				</span>
			</div>
		</div>
	</div>
</div>
