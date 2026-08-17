<script lang="ts">
	import { goto } from '$app/navigation';
	import { page } from '$app/state';
	import { resolve } from '$app/paths';
	import { fade } from 'svelte/transition';
	import { api } from '$lib/api';

	let { data, children } = $props();

	// `lg:drawer-open` pins the sidebar on a desktop and leaves it collapsible on
	// a phone, which is the only responsive behaviour this shell needs.
	const NAV = [
		{ href: '/', label: 'Overview' },
		{ href: '/buckets', label: 'Buckets' },
		{ href: '/design', label: 'Design reference' }
	] as const;

	// `/` would otherwise light up on every route.
	function isActive(href: string): boolean {
		return href === '/' ? page.url.pathname === '/' : page.url.pathname.startsWith(href);
	}

	let signingOut = $state(false);

	async function signOut() {
		signingOut = true;
		try {
			await api.logout();
		} finally {
			await goto(resolve('/login'));
		}
	}
</script>

<div class="drawer lg:drawer-open bg-base-200 min-h-screen">
	<input id="console-drawer" type="checkbox" class="drawer-toggle" />

	<div class="drawer-content flex min-h-screen flex-col">
		<header
			class="navbar bg-base-100 border-base-300 sticky top-0 z-20 min-h-0 gap-2 border-b px-4 py-2"
		>
			<label for="console-drawer" class="btn btn-ghost btn-sm drawer-button lg:hidden">
				<svg class="size-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
					<path d="M4 6h16M4 12h16M4 18h16" stroke-linecap="round" />
				</svg>
				<span class="sr-only">Open navigation</span>
			</label>

			<div class="navbar-start lg:hidden">
				<span class="font-semibold">MonoBucket</span>
			</div>

			<div class="navbar-end ml-auto flex items-center gap-2">
				<div class="join">
					<input
						type="radio"
						name="console-theme"
						value="corporate"
						aria-label="Light"
						class="theme-controller join-item btn btn-xs"
					/>
					<input
						type="radio"
						name="console-theme"
						value="night"
						aria-label="Dark"
						class="theme-controller join-item btn btn-xs"
					/>
				</div>

				<span class="text-base-content/60 hidden font-mono text-xs sm:inline"
					>{data.session.accessKey}</span
				>

				<button class="btn btn-ghost btn-xs" onclick={signOut} disabled={signingOut}>
					Sign out
				</button>
			</div>
		</header>

		<main class="flex-1 p-4 sm:p-6">
			{#key page.url.pathname}
				<div in:fade={{ duration: 150 }}>
					{@render children()}
				</div>
			{/key}
		</main>
	</div>

	<div class="drawer-side z-30">
		<label for="console-drawer" aria-label="Close navigation" class="drawer-overlay"></label>

		<div class="bg-base-100 border-base-300 flex min-h-full w-60 flex-col border-r">
			<div class="border-base-300 flex items-center gap-2 border-b px-4 py-3">
				<span class="bg-primary size-2 rounded-full"></span>
				<span class="font-semibold">MonoBucket</span>
			</div>

			<ul class="menu menu-sm w-full grow gap-1 p-2">
				{#each NAV as item (item.href)}
					<li>
						<a href={resolve(item.href)} class={isActive(item.href) ? 'menu-active' : ''}>
							{item.label}
						</a>
					</li>
				{/each}
			</ul>

			{#if data.session.usingDefaultCredentials}
				<div role="alert" class="alert alert-warning alert-soft m-2 text-xs">
					<span
						>Built-in demo credentials are in use. Set the root key pair before exposing this.</span
					>
				</div>
			{/if}

			<div class="text-base-content/40 border-base-300 border-t px-4 py-3 text-xs">
				v{data.session.version}
			</div>
		</div>
	</div>
</div>
