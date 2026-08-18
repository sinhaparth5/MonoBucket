<script lang="ts">
	// The security event log. Bounded on the server — a fixed ring of the most
	// recent entries — so this page is a window on what just happened rather
	// than an archive. Anything that needs to be kept belongs wherever this
	// server's stdout is collected.
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { fade, fly } from 'svelte/transition';
	import { api, ApiError, type AuditEntry } from '$lib/api';
	import { formatTimestamp, plural } from '$lib/format';
	import { motionDistance, motionDuration } from '$lib/motion';
	import Icon, { type IconName } from '$lib/components/Icon.svelte';

	let entries = $state<AuditEntry[] | null>(null);
	let capacity = $state(0);
	let error = $state('');
	let refreshing = $state(false);
	let filter = $state('');
	let refusalsOnly = $state(false);

	// The first component of the dotted action decides the icon, so a verb this
	// build has never seen still renders as something rather than as a gap.
	const SUBJECT_ICON: Record<string, IconName> = {
		user: 'users',
		credential: 'key',
		session: 'shield',
		authz: 'warning'
	};

	function iconFor(action: string): IconName {
		return SUBJECT_ICON[action.split('.')[0]] ?? 'activity';
	}

	async function load() {
		refreshing = true;
		try {
			const answer = await api.audit();
			entries = answer.entries;
			capacity = answer.capacity;
			error = '';
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			error = cause instanceof ApiError ? cause.message : 'could not read the activity log';
		} finally {
			refreshing = false;
		}
	}

	$effect(() => {
		load();
	});

	const visible = $derived(
		(entries ?? []).filter((entry) => {
			if (refusalsOnly && entry.allowed) return false;
			const needle = filter.trim().toLowerCase();
			if (!needle) return true;
			return (
				entry.action.toLowerCase().includes(needle) ||
				entry.actor.toLowerCase().includes(needle) ||
				entry.target.toLowerCase().includes(needle)
			);
		})
	);

	const refusals = $derived((entries ?? []).filter((entry) => !entry.allowed).length);
</script>

<svelte:head><title>Activity · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-6">
	<header class="flex flex-wrap items-end justify-between gap-4">
		<div class="flex flex-col gap-1">
			<span class="eyebrow">Audit</span>
			<h1 class="text-3xl font-bold tracking-tight sm:text-4xl">Activity</h1>
			{#if entries}
				<p class="text-base-content/55 text-sm">
					{plural(entries.length, 'event')} held, {refusals} refused · the log keeps the most recent
					{capacity}
				</p>
			{/if}
		</div>

		<button class="btn btn-ghost gap-2" onclick={load} disabled={refreshing}>
			<Icon name="refresh" class="size-4" />
			Refresh
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

	<div class="flex flex-wrap items-center gap-3">
		<label class="input w-full max-w-sm">
			<Icon name="overview" class="text-primary size-4" />
			<input type="search" bind:value={filter} placeholder="Filter by action, user or target" />
		</label>
		<label class="label cursor-pointer gap-2">
			<input type="checkbox" class="toggle toggle-sm" bind:checked={refusalsOnly} />
			<span class="text-sm">Refusals only</span>
		</label>
	</div>

	{#if !entries}
		<div class="skeleton rounded-box h-64" out:fade={{ duration: motionDuration(100) }}></div>
	{:else if visible.length === 0}
		<div
			class="panel surface-raised flex flex-col items-center gap-4 p-8 text-center"
			in:fly={{ y: motionDistance(10), duration: motionDuration(240), opacity: 0.5 }}
		>
			<span class="bg-primary/12 text-primary grid size-14 place-items-center rounded-2xl">
				<Icon name="activity" class="size-7" />
			</span>
			<div class="flex flex-col gap-1">
				<p class="text-xl font-bold tracking-tight">Nothing to show</p>
				<p class="text-base-content/60 mx-auto max-w-md text-sm">
					Sign-ins, user changes, credential changes and refused requests all land here.
				</p>
			</div>
		</div>
	{:else}
		<div class="panel overflow-x-auto shadow-sm">
			<table class="table table-sm">
				<thead>
					<tr class="border-base-300">
						<th class="w-52">When</th>
						<th class="w-56">Action</th>
						<th class="w-40">Actor</th>
						<th>Target</th>
						<th>Detail</th>
					</tr>
				</thead>
				<tbody>
					{#each visible as entry (entry.sequence)}
						<tr
							class="hover:bg-base-200/70 transition-colors"
							in:fade={{ duration: motionDuration(150) }}
						>
							<td class="text-base-content/60 whitespace-nowrap">{formatTimestamp(entry.atMs)}</td>
							<td>
								<span class="flex items-center gap-2">
									<span
										class="grid size-7 shrink-0 place-items-center rounded-lg {entry.allowed
											? 'bg-primary/12 text-primary'
											: 'bg-error/12 text-error'}"
									>
										<Icon name={iconFor(entry.action)} class="size-3.5" />
									</span>
									<span class="font-mono text-xs">{entry.action}</span>
								</span>
							</td>
							<td class="text-sm">{entry.actor || '—'}</td>
							<td class="text-base-content/70 max-w-xs truncate font-mono text-xs">
								{entry.target || '—'}
							</td>
							<td class="text-base-content/60 max-w-sm truncate text-xs">{entry.detail || '—'}</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</div>
	{/if}
</div>
