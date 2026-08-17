<script lang="ts">
	import { goto } from '$app/navigation';
	import { page } from '$app/state';
	import { resolve } from '$app/paths';
	import { fade } from 'svelte/transition';
	import { api } from '$lib/api';
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
		{ href: '/settings', label: 'Settings', icon: 'settings', hint: 'The resolved configuration' }
	] as const satisfies readonly { href: string; label: string; icon: IconName; hint: string }[];

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

<div class="drawer lg:drawer-open bg-base-200 min-h-screen">
	<input bind:this={drawerToggle} id="console-drawer" type="checkbox" class="drawer-toggle" />

	<div class="drawer-content flex min-h-screen min-w-0 flex-col">
		<header
			class="navbar border-base-300 bg-base-100/85 sticky top-0 z-20 min-h-0 gap-2 border-b px-3 py-2 backdrop-blur sm:px-4"
		>
			<label
				for="console-drawer"
				class="btn btn-ghost btn-sm drawer-button px-2 lg:hidden"
				aria-label="Open navigation"
			>
				<Icon name="menu" />
			</label>

			<span class="flex items-center gap-2 font-semibold lg:hidden">
				<img src={logo} alt="" class="size-6" />
				MonoBucket
			</span>

			<div class="ml-auto flex items-center gap-1 sm:gap-2">
				<!-- Three states, not two: someone who has never touched this should
				     keep following the operating system, and a two-way switch has
				     nowhere to put that. -->
				<div class="dropdown dropdown-end">
					<div
						tabindex="0"
						role="button"
						class="btn btn-ghost btn-sm px-2"
						aria-label="Appearance"
						title="Appearance"
					>
						<Icon name={theme.choice === 'dark' ? 'moon' : 'sun'} />
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

				<span class="text-base-content/60 hidden font-mono text-xs sm:inline">
					{data.session.accessKey}
				</span>

				<button
					class="btn btn-ghost btn-sm px-2"
					onclick={signOut}
					disabled={signingOut}
					title="Sign out"
				>
					<Icon name="signOut" />
					<span class="sr-only">Sign out</span>
				</button>
			</div>
		</header>

		<main class="min-w-0 flex-1 p-4 sm:p-6">
			<div class="mx-auto w-full max-w-7xl">
				{#key page.url.pathname}
					<div in:fade={{ duration: 150 }}>
						{@render children()}
					</div>
				{/key}
			</div>
		</main>
	</div>

	<div class="drawer-side z-30">
		<label for="console-drawer" aria-label="Close navigation" class="drawer-overlay"></label>

		<div class="bg-base-100 border-base-300 flex min-h-full w-64 flex-col border-r">
			<div class="border-base-300 surface-raised flex items-center gap-2.5 border-b px-4 py-3.5">
				<img src={logo} alt="" class="size-9" />
				<span class="flex flex-col leading-tight">
					<span class="font-semibold tracking-tight">MonoBucket</span>
					<span class="text-base-content/50 text-[0.6875rem]">Object storage console</span>
				</span>
			</div>

			<ul class="menu menu-sm w-full grow gap-0.5 p-2">
				{#each NAV as item (item.href)}
					<li>
						<a
							href={resolve(item.href)}
							title={item.hint}
							class="gap-3 {isActive(item.href)
								? 'bg-primary/10 text-primary font-medium'
								: 'text-base-content/75'}"
							onclick={() => {
								if (drawerToggle) drawerToggle.checked = false;
							}}
						>
							<Icon name={item.icon} />
							{item.label}
						</a>
					</li>
				{/each}
			</ul>

			{#if data.session.usingDefaultCredentials}
				<div role="alert" class="alert alert-warning alert-soft m-2 gap-2 text-xs">
					<Icon name="warning" class="size-4" />
					<span>
						Built-in demo credentials are in use. Set the root key pair before exposing this.
					</span>
				</div>
			{/if}

			<div
				class="border-base-300 text-base-content/45 flex items-center justify-between border-t px-4 py-3 text-xs"
			>
				<span class="font-mono">v{data.session.version}</span>
				<span class="flex items-center gap-1.5">
					<span class="bg-success size-1.5 rounded-full"></span>
					connected
				</span>
			</div>
		</div>
	</div>
</div>
