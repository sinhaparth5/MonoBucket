<script lang="ts">
	// An allocation, entered as an amount and a unit.
	//
	// One component rather than two forms, because the create dialog and the
	// resize dialog have to agree on what "4" means and on which figures are
	// refusable. A second copy would eventually round differently from the first
	// and the server would be the one to say so.
	import { ALLOCATION_UNITS, allocationBytes, formatBytes, type AllocationUnit } from '$lib/format';

	interface Props {
		/// The chosen allocation in bytes. Bound, so the parent sends exactly
		/// what was shown.
		bytes: number;
		amount: number;
		unit: AllocationUnit;
		/// What is left to allocate, or null when the instance sets no ceiling.
		availableBytes: number | null;
		/// What the bucket already holds. An allocation below this is refused by
		/// the server, so the form says so before the round trip.
		usedBytes?: number;
		label?: string;
		disabled?: boolean;
	}

	let {
		bytes = $bindable(),
		amount = $bindable(),
		unit = $bindable(),
		availableBytes,
		usedBytes = 0,
		label = 'Allocation',
		disabled = false
	}: Props = $props();

	$effect(() => {
		bytes = allocationBytes(amount, unit);
	});

	// Advisory only. The server checks both of these again with numbers that
	// cannot have moved underneath it, and its answer is the one that counts.
	const overCapacity = $derived(availableBytes !== null && bytes > availableBytes);
	const belowUsage = $derived(usedBytes > 0 && bytes < usedBytes);
</script>

<fieldset class="fieldset gap-1 p-0">
	<legend class="fieldset-legend">{label}</legend>
	<div class="join w-full">
		<input
			class="input join-item w-full"
			type="number"
			min="1"
			step="1"
			bind:value={amount}
			{disabled}
			aria-label="{label} amount"
			required
		/>
		<select class="select join-item w-28" bind:value={unit} {disabled} aria-label="{label} unit">
			{#each ALLOCATION_UNITS as option (option.label)}
				<option value={option.label}>{option.label}</option>
			{/each}
		</select>
	</div>

	<p class="fieldset-label">
		{#if availableBytes === null}
			This instance sets no overall capacity, so any allocation is accepted.
		{:else}
			{formatBytes(availableBytes)} available to allocate.
		{/if}
	</p>

	{#if belowUsage}
		<p class="text-warning text-xs">
			This bucket already holds {formatBytes(usedBytes)}. An allocation below what is stored is
			refused.
		</p>
	{:else if overCapacity}
		<p class="text-warning text-xs">That is more than this instance has left to allocate.</p>
	{/if}
</fieldset>
