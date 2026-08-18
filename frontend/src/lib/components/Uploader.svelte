<script lang="ts">
	// Drag-and-drop upload into the prefix currently being browsed.
	//
	// Files go straight to `/_mb/api/upload`, one request each, streamed into the
	// payload tree on the far side. The console does not chunk them itself: a
	// resumable console protocol would be a second upload session to design,
	// implement and get wrong, and the S3 listener already has the one S3 clients
	// use — multipart — for the case that needs it. What the browser does give us
	// for free is byte-accurate progress on the request body, which is the part
	// anyone watching actually wants.
	import { fly } from 'svelte/transition';
	import { ApiError, uploadObject, type UploadHandle } from '$lib/api';
	import { formatBytes, plural } from '$lib/format';
	import { motionDistance, motionDuration } from '$lib/motion';
	import Icon from './Icon.svelte';

	interface Props {
		bucket: string;
		/// Where the files land. Keys are this plus the file name — a dropped
		/// folder is not walked, because `webkitdirectory` is the only way to get
		/// one and it is a different control.
		prefix: string;
		/// Called after every file settles, successfully or not, so the listing
		/// behind the drop zone catches up.
		onfinished: () => void;
	}

	let { bucket, prefix, onfinished }: Props = $props();

	type Transfer = {
		id: number;
		name: string;
		key: string;
		size: number;
		sent: number;
		state: 'waiting' | 'sending' | 'done' | 'failed' | 'cancelled';
		error: string;
		handle?: UploadHandle;
	};

	// Three at a time. One is slower than it needs to be for a folder of small
	// files; unbounded would put the console in competition with the S3 listener
	// for the same bounded I/O pool, and shedding load we generated ourselves is
	// a strange way to run a dashboard.
	const CONCURRENCY = 3;

	let transfers = $state<Transfer[]>([]);
	let dragging = $state(false);
	let picker = $state<HTMLInputElement | undefined>();
	let nextId = 0;

	// Counted rather than derived from the array so a drag that leaves a child
	// element does not read as a drag that left the zone. dragenter/dragleave
	// fire in pairs as the pointer crosses every descendant.
	let dragDepth = 0;

	const active = $derived(transfers.filter((t) => t.state === 'waiting' || t.state === 'sending'));
	const failed = $derived(transfers.filter((t) => t.state === 'failed'));
	const totalBytes = $derived(transfers.reduce((sum, t) => sum + t.size, 0));
	const sentBytes = $derived(transfers.reduce((sum, t) => sum + t.sent, 0));
	const overall = $derived(totalBytes > 0 ? sentBytes / totalBytes : 0);

	function enqueue(files: FileList | null) {
		if (!files || files.length === 0) return;

		for (const file of files) {
			transfers.push({
				id: (nextId += 1),
				name: file.name,
				key: prefix + file.name,
				size: file.size,
				sent: 0,
				state: 'waiting',
				error: '',
				handle: undefined
			});
			pending[nextId] = file;
		}
		pump();
	}

	// A plain record rather than a Map, and deliberately outside `$state`: a
	// `$state` proxy wraps what it stores, and a proxied File is no longer
	// something XMLHttpRequest will accept as a request body.
	const pending: Record<number, File> = {};

	function pump() {
		let sending = transfers.filter((t) => t.state === 'sending').length;

		for (const transfer of transfers) {
			if (sending >= CONCURRENCY) break;
			if (transfer.state !== 'waiting') continue;

			const file = pending[transfer.id];
			if (!file) continue;

			transfer.state = 'sending';
			sending += 1;

			const handle = uploadObject(bucket, transfer.key, file, (sent) => {
				transfer.sent = sent;
			});
			transfer.handle = handle;

			handle.done
				.then(() => {
					transfer.state = 'done';
					transfer.sent = transfer.size;
				})
				.catch((cause: unknown) => {
					const cancelled = cause instanceof ApiError && cause.message === 'cancelled';
					transfer.state = cancelled ? 'cancelled' : 'failed';
					transfer.error = cause instanceof ApiError ? cause.message : 'upload failed';
				})
				.finally(() => {
					delete pending[transfer.id];
					transfer.handle = undefined;
					onfinished();
					pump();
				});
		}
	}

	function cancel(transfer: Transfer) {
		if (transfer.handle) {
			transfer.handle.cancel();
			return;
		}
		// Still queued: it never started, so there is nothing to abort.
		transfer.state = 'cancelled';
		delete pending[transfer.id];
		pump();
	}

	function clearSettled() {
		transfers = transfers.filter((t) => t.state === 'waiting' || t.state === 'sending');
	}

	function onDrop(event: DragEvent) {
		event.preventDefault();
		dragDepth = 0;
		dragging = false;
		enqueue(event.dataTransfer?.files ?? null);
	}
</script>

<svelte:window
	ondragenter={(event) => {
		// Only a drag carrying files. Dragging selected text across the page
		// should not light up an upload target.
		if (!event.dataTransfer?.types.includes('Files')) return;
		dragDepth += 1;
		dragging = true;
	}}
	ondragleave={() => {
		dragDepth = Math.max(0, dragDepth - 1);
		if (dragDepth === 0) dragging = false;
	}}
	ondragover={(event) => {
		if (event.dataTransfer?.types.includes('Files')) event.preventDefault();
	}}
	ondrop={onDrop}
/>

<div class="flex flex-col gap-3">
	<!-- svelte-ignore a11y_no_static_element_interactions -->
	<div
		class="rounded-box flex flex-col items-center gap-2 border-2 border-dashed px-6 py-6 text-center transition-colors duration-200 {dragging
			? 'border-primary bg-primary/5'
			: 'border-base-300 hover:border-base-content/25'}"
		ondragover={(event) => event.preventDefault()}
		ondrop={onDrop}
	>
		<span
			class="grid size-10 place-items-center rounded-xl transition-colors duration-200 {dragging
				? 'bg-primary text-primary-content'
				: 'bg-base-200 text-base-content/60'}"
		>
			<Icon name="upload" class="size-5" />
		</span>
		<p class="text-sm">
			{#if dragging}
				Drop to upload into <span class="font-mono">{prefix || '/'}</span>
			{:else}
				Drop files here, or
				<button class="link link-primary" onclick={() => picker?.click()}>browse</button>
			{/if}
		</p>
		<p class="text-base-content/50 text-xs">
			Uploaded into <span class="font-mono">{bucket}/{prefix}</span> · a name that already exists is replaced
		</p>
		<input
			bind:this={picker}
			type="file"
			multiple
			class="hidden"
			onchange={(event) => {
				enqueue(event.currentTarget.files);
				// Cleared so re-picking the same file fires change again.
				event.currentTarget.value = '';
			}}
		/>
	</div>

	{#if transfers.length > 0}
		<div
			class="panel flex flex-col overflow-hidden"
			transition:fly={{ y: motionDistance(-6), duration: motionDuration(200) }}
		>
			<div class="border-base-300 flex flex-wrap items-center gap-3 border-b px-4 py-2.5">
				<span class="text-sm font-medium">
					{#if active.length > 0}
						Uploading {active.length} of {plural(transfers.length, 'file')}
					{:else if failed.length > 0}
						{plural(failed.length, 'upload')} failed
					{:else}
						{plural(transfers.length, 'file')} uploaded
					{/if}
				</span>
				<span class="text-base-content/50 text-xs tabular-nums">
					{formatBytes(sentBytes)} of {formatBytes(totalBytes)}
				</span>
				{#if active.length === 0}
					<button class="btn btn-ghost btn-xs ml-auto" onclick={clearSettled}>Clear</button>
				{/if}
			</div>

			{#if active.length > 0}
				<progress class="progress progress-primary h-1 w-full rounded-none" value={overall} max="1"
				></progress>
			{/if}

			<ul class="divide-base-300 max-h-64 divide-y overflow-y-auto">
				{#each transfers as transfer (transfer.id)}
					<li class="flex items-center gap-3 px-4 py-2">
						<span
							class="grid size-7 shrink-0 place-items-center rounded-lg {transfer.state === 'done'
								? 'bg-success/12 text-success'
								: transfer.state === 'failed'
									? 'bg-error/12 text-error'
									: transfer.state === 'cancelled'
										? 'bg-base-200 text-base-content/40'
										: 'bg-primary/10 text-primary'}"
						>
							<Icon
								name={transfer.state === 'done'
									? 'check'
									: transfer.state === 'failed'
										? 'warning'
										: transfer.state === 'cancelled'
											? 'close'
											: 'file'}
								class="size-3.5"
							/>
						</span>

						<div class="flex min-w-0 flex-1 flex-col gap-1">
							<div class="flex items-baseline justify-between gap-2">
								<span class="truncate font-mono text-xs" title={transfer.key}>
									{transfer.name}
								</span>
								<span class="text-base-content/50 shrink-0 text-xs tabular-nums">
									{#if transfer.state === 'failed'}
										{transfer.error}
									{:else if transfer.state === 'cancelled'}
										cancelled
									{:else if transfer.state === 'done'}
										{formatBytes(transfer.size)}
									{:else}
										{formatBytes(transfer.sent)} / {formatBytes(transfer.size)}
									{/if}
								</span>
							</div>
							{#if transfer.state === 'sending' || transfer.state === 'waiting'}
								<progress
									class="progress progress-primary h-1 w-full"
									value={transfer.size > 0 ? transfer.sent / transfer.size : 0}
									max="1"
								></progress>
							{/if}
						</div>

						{#if transfer.state === 'sending' || transfer.state === 'waiting'}
							<button
								class="btn btn-ghost btn-xs px-1"
								aria-label="Cancel uploading {transfer.name}"
								onclick={() => cancel(transfer)}
							>
								<Icon name="close" class="size-3.5" />
							</button>
						{/if}
					</li>
				{/each}
			</ul>
		</div>
	{/if}
</div>
