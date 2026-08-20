<script lang="ts">
	// Read-only, and that is the feature rather than a limitation.
	//
	// Configuration is environment only: every knob is parsed once at startup,
	// cross-validated, and frozen before the first listener opens. A panel with
	// save buttons would either lie about taking effect or force a restart path
	// that does not exist. So this shows what the server actually resolved, and
	// names the variable that produced each value — which is what someone came
	// here to find out.
	import { goto } from '$app/navigation';
	import { resolve } from '$app/paths';
	import { fly } from 'svelte/transition';
	import {
		api,
		ApiError,
		can,
		type Backup,
		type ServerConfig,
		type Setting,
		type UploadLimit
	} from '$lib/api';
	import {
		ALLOCATION_UNITS,
		allocationBytes,
		formatBytes,
		formatDuration,
		plural,
		splitAllocation,
		type AllocationUnit
	} from '$lib/format';
	import { motionDistance, motionDuration } from '$lib/motion';
	import Icon, { type IconName } from '$lib/components/Icon.svelte';

	let { data } = $props();

	let config = $state<ServerConfig | null>(null);
	let error = $state('');
	let copied = $state('');

	// The one thing on this page that is not read-only, and it is here rather
	// than under Users because it is the only account-level control every role
	// holds. Changing somebody else's password lives on the Users page and needs
	// `user:write`; changing your own needs only the current one.
	let currentPassword = $state('');
	let nextPassword = $state('');
	let confirmPassword = $state('');
	let passwordError = $state('');
	let passwordNotice = $state('');
	let changingPassword = $state(false);

	async function changePassword(event: SubmitEvent) {
		event.preventDefault();
		passwordError = '';
		passwordNotice = '';

		if (nextPassword !== confirmPassword) {
			passwordError = 'the two new passwords do not match';
			return;
		}

		changingPassword = true;
		try {
			await api.setPassword(nextPassword, { currentPassword });
			currentPassword = '';
			nextPassword = '';
			confirmPassword = '';
			// The server ended every session for this account and handed back a
			// fresh cookie, so this tab keeps working and every other one does
			// not. Saying so is the point — a password change whose reason is a
			// leaked copy should visibly have done something.
			passwordNotice = 'Password changed. Any other session you had open has been signed out.';
		} catch (cause) {
			passwordError = cause instanceof ApiError ? cause.message : 'could not change the password';
		} finally {
			changingPassword = false;
		}
	}

	// The one figure on this page that is stored rather than configured, so it
	// is the one control here that has a save button. Everything else is
	// environment-only and could not honour one.
	const mayWriteSettings = $derived(can(data.session, 'settings:write'));
	const mayBackUp = $derived(can(data.session, 'backup:write'));

	// Nothing here schedules anything. The server takes a copy when somebody
	// asks and never on a timer — retention and off-site copies are a backup
	// tool's job, and half of one built into the server is the half nobody
	// tests.
	let backupName = $state('');
	let backingUp = $state(false);
	let backupError = $state('');
	let lastBackup = $state<Backup | null>(null);

	async function takeBackup(event: SubmitEvent) {
		event.preventDefault();
		backupError = '';
		backingUp = true;
		try {
			lastBackup = await api.createBackup(backupName.trim());
			backupName = '';
		} catch (cause) {
			backupError = cause instanceof ApiError ? cause.message : 'could not write the backup';
		} finally {
			backingUp = false;
		}
	}

	function suggestBackupName() {
		// Sortable and unambiguous, which is what somebody scanning a directory
		// of them a year later needs. Colons are left out: they are legal on
		// this filesystem and a nuisance on others.
		backupName = new Date().toISOString().replace(/[:.]/g, '-').replace('Z', '');
	}

	let limit = $state<UploadLimit | null>(null);
	let limitAmount = $state(1);
	let limitUnit = $state<AllocationUnit>('GiB');
	let limitError = $state('');
	let limitNotice = $state('');
	let savingLimit = $state(false);

	const limitBytes = $derived(allocationBytes(limitAmount, limitUnit));
	const overCeiling = $derived(limit !== null && limitBytes > limit.ceilingBytes);

	async function saveLimit(event: SubmitEvent) {
		event.preventDefault();
		limitError = '';
		limitNotice = '';
		savingLimit = true;
		try {
			limit = await api.setUploadLimit(limitBytes);
			// Read back from the answer rather than kept from the form: the
			// server is what decides, and showing the figure it stored is the
			// only way this panel cannot drift from what is enforced.
			({ amount: limitAmount, unit: limitUnit } = splitAllocation(limit.maxUploadBytes));
			limitNotice = `Uploads over ${formatBytes(limit.maxUploadBytes)} are now refused. Transfers already under way finish.`;
		} catch (cause) {
			limitError = cause instanceof ApiError ? cause.message : 'could not change the limit';
		} finally {
			savingLimit = false;
		}
	}

	async function load() {
		try {
			config = await api.config();
			limit = await api.uploadLimit();
			({ amount: limitAmount, unit: limitUnit } = splitAllocation(limit.maxUploadBytes));
			error = '';
		} catch (cause) {
			if (cause instanceof ApiError && cause.unauthorized) {
				await goto(resolve('/login'));
				return;
			}
			error = cause instanceof ApiError ? cause.message : 'could not read the configuration';
		}
	}

	$effect(() => {
		load();
	});

	// The grouping the server does not have and a reader does: config.cpp is one
	// flat struct, but nobody comes here asking about "settings", they come
	// asking about storage, or memory, or who can sign in.
	const GROUPS: { title: string; icon: IconName; blurb: string; keys: string[] }[] = [
		{
			title: 'Listeners',
			icon: 'plug',
			blurb: 'Two ports, one process. The console never answers on the S3 port.',
			keys: ['host', 's3Port', 'consolePort', 'consoleEnabled', 's3Domain', 'region']
		},
		{
			title: 'Credentials',
			icon: 'key',
			blurb:
				'Who signs in here, and the bootstrap S3 pair. Neither is the other: manage day-to-day S3 keys under Access keys.',
			keys: ['adminUsername', 'rootAccessKey', 'rootSecretKey']
		},
		{
			title: 'Storage',
			icon: 'disk',
			blurb: 'Where payloads live and how hard a write is pushed to the platter.',
			keys: [
				'dataDir',
				'durability',
				'metadataMemoryBytes',
				'metadataMaxOpenFiles',
				'reclaimGraceSeconds',
				'reclaimIntervalSeconds'
			]
		},
		{
			title: 'Concurrency and limits',
			icon: 'memory',
			blurb:
				'The bounds that keep resident memory flat. A full I/O queue sheds load as 503 SlowDown rather than growing.',
			keys: [
				'workerThreads',
				'ioThreads',
				'ioQueueLimit',
				'maxBodyBytes',
				'maxMemoryBodyBytes',
				'streamChunkBytes',
				'idleTimeoutSeconds'
			]
		},
		{
			title: 'Cache',
			icon: 'cache',
			blurb: 'Sits in front of metadata reads. A cache outage never becomes a storage outage.',
			keys: [
				'cacheBackend',
				'cacheMaxBytes',
				'cacheTtlSeconds',
				'cacheLocalTtlSeconds',
				'redisConfigured',
				'redisPoolSize'
			]
		},
		{
			title: 'Observability',
			icon: 'overview',
			blurb: 'Prometheus text on /metrics, structured lines on stderr.',
			keys: ['logLevel', 'metricsEnabled']
		}
	];

	const byKey = $derived(
		new Map((config?.settings ?? []).map((setting) => [setting.key, setting]))
	);

	// Anything the server reports that no group claims. Listed rather than
	// dropped: a setting added to the backend should show up here on its own
	// instead of silently disappearing from the panel.
	const ungrouped = $derived.by(() => {
		const claimed = new Set(GROUPS.flatMap((group) => group.keys));
		return (config?.settings ?? []).filter((setting) => !claimed.has(setting.key));
	});

	const BYTE_KEYS = new Set([
		'metadataMemoryBytes',
		'maxBodyBytes',
		'maxMemoryBodyBytes',
		'streamChunkBytes',
		'cacheMaxBytes'
	]);
	const SECOND_KEYS = new Set([
		'reclaimGraceSeconds',
		'reclaimIntervalSeconds',
		'idleTimeoutSeconds',
		'cacheTtlSeconds',
		'cacheLocalTtlSeconds'
	]);

	/// The raw value is always shown; this is the second line that says what it
	/// means. 1073741824 is a number, "1.0 GiB" is an answer.
	function annotate(setting: Setting): string {
		if (typeof setting.value !== 'number') return '';
		if (BYTE_KEYS.has(setting.key)) return formatBytes(setting.value);
		if (SECOND_KEYS.has(setting.key)) {
			return setting.value === 0 ? 'no limit' : formatDuration(setting.value);
		}
		return '';
	}

	function render(setting: Setting): string {
		if (typeof setting.value === 'boolean') return setting.value ? 'on' : 'off';
		if (setting.value === '') return '—';
		return String(setting.value);
	}

	async function copyEnv(setting: Setting) {
		const line = `${setting.env}=${setting.value}`;
		try {
			await navigator.clipboard.writeText(line);
			copied = setting.key;
			setTimeout(() => (copied = ''), 1500);
		} catch {
			// Clipboard access can be refused; the text is on screen either way.
		}
	}
</script>

<svelte:head><title>Settings · MonoBucket</title></svelte:head>

<div class="flex flex-col gap-6">
	<header
		class="panel surface-raised grid min-h-56 overflow-hidden lg:grid-cols-[minmax(0,1fr)_20rem]"
	>
		<div class="flex flex-col justify-center gap-3 p-6 sm:p-8">
			<span class="eyebrow">Runtime configuration</span>
			<div class="flex flex-col gap-1">
				<h1 class="text-3xl font-bold tracking-tight sm:text-4xl">Settings</h1>
				<p class="text-base-content/60 max-w-xl text-sm">
					The configuration this process resolved at startup, and the variable behind each value.
				</p>
			</div>
			<span class="badge badge-info badge-soft w-fit gap-1.5">
				<Icon name="settings" class="size-3.5" />
				Read-only runtime snapshot
			</span>
		</div>
		<div class="relative min-h-44 overflow-hidden lg:min-h-full">
			<img
				src="/images/settings-runtime.webp"
				alt="Luminous green configuration controls orbiting a storage core"
				width="900"
				height="600"
				decoding="async"
				class="absolute inset-0 size-full object-cover object-center"
			/>
			<div
				class="from-base-100/80 pointer-events-none absolute inset-0 bg-gradient-to-b from-transparent via-transparent to-base-100/80 lg:bg-gradient-to-r lg:from-base-100/85 lg:via-transparent lg:to-transparent"
			></div>
		</div>
	</header>

	<section class="panel surface-raised flex flex-col gap-4 p-6">
		<div class="flex flex-col gap-1">
			<span class="eyebrow">Your account</span>
			<h2 class="text-xl font-bold tracking-tight">
				Signed in as {data.session.username}
				<span class="badge badge-primary badge-soft ml-2 align-middle">{data.session.role}</span>
			</h2>
			<p class="text-base-content/60 max-w-xl text-sm">
				This password signs you in to the console. It does not sign S3 requests, so changing it
				leaves every access key working.
			</p>
		</div>

		<form class="grid gap-3 md:grid-cols-3" onsubmit={changePassword}>
			<fieldset class="fieldset gap-1.5 p-0">
				<legend class="fieldset-legend text-sm">Current password</legend>
				<label class="input w-full">
					<Icon name="shield" class="text-primary size-4" />
					<input
						type="password"
						autocomplete="current-password"
						bind:value={currentPassword}
						required
					/>
				</label>
			</fieldset>

			<fieldset class="fieldset gap-1.5 p-0">
				<legend class="fieldset-legend text-sm">New password</legend>
				<label class="input w-full">
					<Icon name="shield" class="text-primary size-4" />
					<input
						type="password"
						autocomplete="new-password"
						minlength="12"
						bind:value={nextPassword}
						placeholder="At least 12 characters"
						required
					/>
				</label>
			</fieldset>

			<fieldset class="fieldset gap-1.5 p-0">
				<legend class="fieldset-legend text-sm">Confirm new password</legend>
				<label class="input w-full">
					<Icon name="shield" class="text-primary size-4" />
					<input
						type="password"
						autocomplete="new-password"
						minlength="12"
						bind:value={confirmPassword}
						required
					/>
				</label>
			</fieldset>

			{#if passwordError}
				<div role="alert" class="alert alert-error alert-soft text-sm md:col-span-3">
					<Icon name="warning" class="size-4" />
					<span>{passwordError}</span>
				</div>
			{/if}

			{#if passwordNotice}
				<div role="status" class="alert alert-success alert-soft text-sm md:col-span-3">
					<Icon name="check" class="size-4" />
					<span>{passwordNotice}</span>
				</div>
			{/if}

			<div class="md:col-span-3">
				<button class="btn btn-primary gap-2" type="submit" disabled={changingPassword}>
					{#if changingPassword}<span class="loading loading-spinner loading-xs"></span>{/if}
					Change password
				</button>
			</div>
		</form>
	</section>

	{#if mayBackUp}
		<section class="panel surface-raised flex flex-col gap-4 p-6">
			<div class="flex flex-col gap-1">
				<span class="eyebrow">Backup</span>
				<h2 class="text-xl font-bold tracking-tight">Take a checkpoint</h2>
				<p class="text-base-content/60 max-w-xl text-sm">
					Writes a consistent, startable copy of this instance into
					<code class="text-xs">MONOBUCKET_BACKUP_DIR</code>, without stopping anything. Payloads
					are hard-linked, so on the same filesystem it duplicates no bytes and finishes almost
					immediately. Writes arriving while it runs are simply not in the copy.
				</p>
				<p class="text-base-content/50 max-w-xl text-xs">
					Nothing is scheduled. There is no retention and no rotation — a copy is written when you
					ask, and what happens to it afterwards is a backup tool's job. Verify one before you rely
					on it: <code>MONOBUCKET_DATA_DIR=&lt;dir&gt; monobucket --fsck --deep</code>. Restore by
					stopping the server and pointing <code>MONOBUCKET_DATA_DIR</code> at it.
				</p>
			</div>

			<form class="flex flex-wrap items-end gap-3" onsubmit={takeBackup}>
				<fieldset class="fieldset gap-1 p-0">
					<legend class="fieldset-legend text-sm">Name</legend>
					<div class="join">
						<input
							class="input join-item w-72"
							type="text"
							maxlength="128"
							spellcheck="false"
							autocomplete="off"
							bind:value={backupName}
							placeholder="2026-08-20T15-30-00"
							aria-label="Backup name"
							required
						/>
						<button
							type="button"
							class="btn join-item"
							onclick={suggestBackupName}
							title="Use the current time"
						>
							<Icon name="refresh" class="size-4" />
						</button>
					</div>
					<span class="label text-xs"
						>A name, not a path — it is created inside the backup directory.</span
					>
				</fieldset>

				<button class="btn btn-primary gap-2" type="submit" disabled={backingUp}>
					{#if backingUp}<span class="loading loading-spinner loading-xs"></span>{/if}
					Take backup
				</button>
			</form>

			{#if lastBackup}
				<div role="status" class="alert alert-success alert-soft text-sm">
					<Icon name="check" class="size-4" />
					<span>
						Wrote <span class="font-mono text-xs">{lastBackup.destination}</span> in
						{lastBackup.elapsedMs} ms — {plural(lastBackup.payloadsLinked, 'payload')} linked{#if !lastBackup.instant},
							{lastBackup.payloadsCopied}
							copied ({formatBytes(lastBackup.bytesCopied)}) because the destination is on another
							filesystem{/if}.
					</span>
				</div>
			{/if}

			{#if backupError}
				<div role="alert" class="alert alert-error alert-soft text-sm">
					<Icon name="warning" class="size-4" />
					<span>{backupError}</span>
				</div>
			{/if}
		</section>
	{/if}

	{#if limit}
		<section class="panel surface-raised flex flex-col gap-4 p-6">
			<div class="flex flex-col gap-1">
				<span class="eyebrow">Uploads</span>
				<h2 class="text-xl font-bold tracking-tight">Maximum object size</h2>
				<p class="text-base-content/60 max-w-xl text-sm">
					One limit for the whole instance, applied to every object however it arrives — the console
					uploader, a signed PUT, and a multipart upload measured across its parts. It is stored
					rather than configured, so it survives a restart and takes effect on the next request.
				</p>
			</div>

			<div class="flex flex-wrap items-baseline gap-x-6 gap-y-1">
				<span class="text-2xl font-bold tabular-nums">{formatBytes(limit.maxUploadBytes)}</span>
				<span class="text-base-content/50 text-xs">
					ceiling {formatBytes(limit.ceilingBytes)} · raise it with MONOBUCKET_MAX_UPLOAD_CEILING_BYTES
				</span>
			</div>

			{#if mayWriteSettings}
				<form class="flex flex-wrap items-end gap-3" onsubmit={saveLimit}>
					<fieldset class="fieldset gap-1 p-0">
						<legend class="fieldset-legend text-sm">New limit</legend>
						<div class="join">
							<input
								class="input join-item w-32"
								type="number"
								min="1"
								step="1"
								bind:value={limitAmount}
								aria-label="Maximum upload amount"
								required
							/>
							<select
								class="select join-item w-24"
								bind:value={limitUnit}
								aria-label="Maximum upload unit"
							>
								{#each ALLOCATION_UNITS as option (option.label)}
									<option value={option.label}>{option.label}</option>
								{/each}
							</select>
						</div>
					</fieldset>

					<button
						class="btn btn-primary gap-2"
						type="submit"
						disabled={savingLimit || overCeiling || limitBytes === limit.maxUploadBytes}
					>
						{#if savingLimit}<span class="loading loading-spinner loading-xs"></span>{/if}
						Save limit
					</button>

					{#if overCeiling}
						<p class="text-warning w-full text-xs">
							That is above this instance's ceiling of {formatBytes(limit.ceilingBytes)}.
						</p>
					{/if}
				</form>
			{:else}
				<p class="text-base-content/50 text-xs">Only an administrator can change this.</p>
			{/if}

			{#if limitError}
				<div role="alert" class="alert alert-error alert-soft text-sm">
					<Icon name="warning" class="size-4" />
					<span>{limitError}</span>
				</div>
			{/if}
			{#if limitNotice}
				<div role="status" class="alert alert-success alert-soft text-sm">
					<Icon name="check" class="size-4" />
					<span>{limitNotice}</span>
				</div>
			{/if}
		</section>
	{/if}

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

	<div role="note" class="alert alert-info alert-soft items-start">
		<Icon name="settings" />
		<div class="flex flex-col gap-1">
			<span class="font-medium">Nothing here is editable, on purpose.</span>
			<span class="text-sm">
				Every setting is a <code class="font-mono">MONOBUCKET_*</code> environment variable, parsed once
				and validated before the first listener opens. A malformed value aborts startup with a message
				rather than falling back to a default nobody asked for — which is only true because there is no
				second way to set one.
			</span>
		</div>
	</div>

	{#if config?.usingDefaultCredentials}
		<div role="alert" class="alert alert-warning alert-soft items-start">
			<Icon name="warning" />
			<div class="flex flex-col gap-1">
				<span class="font-medium">The built-in demo credentials are in use.</span>
				<span class="text-sm">
					Set <code class="font-mono">MONOBUCKET_ROOT_ACCESS_KEY</code> and
					<code class="font-mono">MONOBUCKET_ROOT_SECRET_KEY</code> before this is reachable by anyone
					else.
				</span>
			</div>
		</div>
	{/if}

	{#if !config}
		<div class="grid gap-4 lg:grid-cols-2">
			{#each [0, 1, 2, 3] as index (index)}
				<div class="skeleton rounded-box h-56"></div>
			{/each}
		</div>
	{:else}
		<div
			class="flex flex-col gap-6"
			in:fly={{ y: motionDistance(10), duration: motionDuration(240), opacity: 0.5 }}
		>
			{#if config.cacheBackendActive !== byKey.get('cacheBackend')?.value}
				<div role="alert" class="alert alert-warning alert-soft">
					<Icon name="cache" />
					<span>
						The configured cache backend is
						<code class="font-mono">{byKey.get('cacheBackend')?.value}</code>, but
						<code class="font-mono">{config.cacheBackendActive}</code>
						is serving. A backend that cannot be reached falls back to the local tier rather than failing
						requests.
					</span>
				</div>
			{/if}

			<div class="grid gap-4 lg:grid-cols-2">
				{#each GROUPS as group (group.title)}
					{@const rows = group.keys.map((key) => byKey.get(key)).filter((row) => row !== undefined)}
					{#if rows.length > 0}
						<section class="panel flex flex-col overflow-hidden">
							<header class="surface-raised border-base-300 flex items-start gap-3 border-b p-5">
								<span
									class="bg-primary/10 text-primary grid size-9 shrink-0 place-items-center rounded-lg"
								>
									<Icon name={group.icon} class="size-4.5" />
								</span>
								<div class="flex flex-col gap-0.5">
									<h2 class="font-medium">{group.title}</h2>
									<p class="text-base-content/55 text-xs">{group.blurb}</p>
								</div>
							</header>

							<dl class="divide-base-300 divide-y">
								{#each rows as setting (setting.key)}
									<div
										class="hover:bg-base-200/60 flex items-start gap-3 px-4 py-2.5 transition-colors"
									>
										<dt class="flex min-w-0 flex-1 flex-col gap-0.5">
											<span class="text-sm">{setting.key}</span>
											{#if setting.env}
												<button
													class="text-base-content/45 hover:text-primary flex items-center gap-1 self-start font-mono text-[0.6875rem] transition-colors"
													title="Copy {setting.env}=… "
													onclick={() => copyEnv(setting)}
												>
													<Icon name={copied === setting.key ? 'check' : 'copy'} class="size-3" />
													{setting.env}
												</button>
											{/if}
										</dt>
										<dd class="flex shrink-0 flex-col items-end gap-0.5 text-right">
											<span class="font-mono text-sm break-all">{render(setting)}</span>
											{#if annotate(setting)}
												<span class="text-base-content/45 text-[0.6875rem]">
													{annotate(setting)}
												</span>
											{/if}
										</dd>
									</div>
								{/each}
							</dl>
						</section>
					{/if}
				{/each}
			</div>

			{#if ungrouped.length > 0}
				<section class="panel flex flex-col overflow-hidden">
					<header class="surface-raised border-base-300 border-b p-4">
						<h2 class="font-medium">Other</h2>
						<p class="text-base-content/55 text-xs">
							{plural(ungrouped.length, 'setting')} the server reports that this panel has no group for.
						</p>
					</header>
					<dl class="divide-base-300 divide-y">
						{#each ungrouped as setting (setting.key)}
							<div class="flex items-center justify-between gap-3 px-4 py-2.5">
								<dt class="text-sm">{setting.key}</dt>
								<dd class="font-mono text-sm break-all">{render(setting)}</dd>
							</div>
						{/each}
					</dl>
				</section>
			{/if}

			<p class="text-base-content/45 text-xs">
				Secrets are redacted by the server before they reach this page, and the administrator
				password is not sent at all — not even redacted. Everything here is read once at startup
				from the environment, so changing a value means restarting with a different one.
			</p>
		</div>
	{/if}
</div>
