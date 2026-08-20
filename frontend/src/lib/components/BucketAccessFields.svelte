<script lang="ts">
	// The bucket-access half of a user form, in one place because the create
	// dialog and the edit dialog have to offer exactly the same thing. Two
	// copies of this would drift the moment a level was added.
	//
	// Presentation only. The server re-decides every request, and a level this
	// picker never offers is still refused if a hand-written request sends it.
	import type { BucketAccessInfo, BucketAccessName, BucketGrants } from '$lib/api';

	let {
		grants = $bindable(),
		buckets,
		levels,
		disabled = false
	}: {
		grants: BucketGrants;
		/// Every bucket on the instance, so an exception can be named without
		/// typing it. Buckets created later fall to the default.
		buckets: string[];
		levels: BucketAccessInfo[];
		disabled?: boolean;
	} = $props();

	const LABEL: Record<BucketAccessName, string> = {
		write: 'Read and write',
		read: 'Read only',
		none: 'No access'
	};

	/// Rebuilds the whole value rather than mutating it, so that a `$bindable`
	/// parent sees one assignment per change.
	function commit(fallback: BucketAccessName, exceptions: Record<string, BucketAccessName>) {
		// An exception that now says what the default says is dropped. It grants
		// nothing the default does not, and a record naming buckets for no
		// reason is one nobody can later explain. The server drops it too, so
		// leaving it would also make the form disagree with what it saved.
		const kept: Record<string, BucketAccessName> = {};
		for (const [bucket, access] of Object.entries(exceptions)) {
			if (access !== fallback) kept[bucket] = access;
		}
		grants = {
			fallback,
			exceptions: kept,
			unrestricted: fallback === 'write' && Object.keys(kept).length === 0
		};
	}

	function setBucket(bucket: string, next: string) {
		const exceptions = { ...grants.exceptions };
		if (next === 'default') delete exceptions[bucket];
		else exceptions[bucket] = next as BucketAccessName;
		commit(grants.fallback, exceptions);
	}
</script>

<fieldset class="fieldset gap-1.5 p-0">
	<legend class="fieldset-legend text-sm">Bucket access</legend>
	<select
		class="select w-full"
		{disabled}
		value={grants.fallback}
		aria-label="Default bucket access"
		onchange={(event) => commit(event.currentTarget.value as BucketAccessName, grants.exceptions)}
	>
		{#each levels as level (level.name)}
			<option value={level.name}>{LABEL[level.name]} — {level.description}</option>
		{/each}
	</select>
	<span class="label text-xs">
		The default for every bucket left alone below, and for buckets created later.
	</span>

	{#if buckets.length}
		<div
			class="border-base-300 divide-base-300 mt-1 flex max-h-56 flex-col divide-y overflow-y-auto rounded-xl border"
		>
			{#each buckets as bucket (bucket)}
				<label class="hover:bg-base-200/60 flex items-center justify-between gap-3 px-3 py-2">
					<span class="truncate font-mono text-xs">{bucket}</span>
					<select
						class="select select-sm w-44 shrink-0"
						{disabled}
						value={grants.exceptions[bucket] ?? 'default'}
						onchange={(event) => setBucket(bucket, event.currentTarget.value)}
					>
						<option value="default">Default — {LABEL[grants.fallback].toLowerCase()}</option>
						{#each levels as level (level.name)}
							<option value={level.name}>{LABEL[level.name]}</option>
						{/each}
					</select>
				</label>
			{/each}
		</div>
	{:else}
		<span class="label text-xs">No buckets yet, so there is nothing to make an exception for.</span>
	{/if}
</fieldset>
