<script lang="ts" module>
	type IconDefinition = {
		main: string;
		detail?: string;
		accent?: string;
		fill?: string;
		brand?: boolean;
	};

	// MonoBucket's product symbols use a luminous, two-plane construction inspired
	// by the storage core artwork. Utility controls stay single-colour: a close,
	// warning, or sign-out glyph should be instantly legible before it is stylish.
	// Every shape shares the same 24×24 grid, 1.8px stroke and round joins.
	const ICONS_RAW = {
		overview: {
			main: 'M19.4 7.2A9 9 0 1 1 7 4.6',
			detail: 'M3.4 12h4l2.1-4.2 4 8.4 2.2-4.2h4.9',
			accent: 'M17.7 4.2h.01',
			fill: 'M12 3a9 9 0 1 1-9 9 9 9 0 0 1 9-9Z',
			brand: true
		},
		bucket: {
			main: 'M4.3 7.4h15.4l-1.3 11a2.2 2.2 0 0 1-2.2 2H7.8a2.2 2.2 0 0 1-2.2-2Z',
			detail:
				'M4.3 7.4c0-2.1 3.4-3.8 7.7-3.8s7.7 1.7 7.7 3.8c0 2-3.4 3.6-7.7 3.6S4.3 9.4 4.3 7.4Z M6.1 15.2c3.7 1.7 8.1 1.7 11.8 0',
			accent: 'M16.8 5.1a9.8 9.8 0 0 1 2.9 2.3 M6.2 18.5h.01',
			fill: 'M4.3 7.4h15.4l-1.3 11a2.2 2.2 0 0 1-2.2 2H7.8a2.2 2.2 0 0 1-2.2-2Z',
			brand: true
		},
		folder: {
			main: 'M3 7.8a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8.4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z',
			detail: 'M3.2 10.2h17.6 M7 16.4h6.2',
			accent: 'M15.8 16.4H17',
			fill: 'M3 10.2h18v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z',
			brand: true
		},
		file: {
			main: 'M6 3h8l5 5v11a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2Z',
			detail: 'M14 3v5h5 M8 12h8 M8 15.5h5',
			accent: 'M8 18.5h2',
			fill: 'M6 3h8l5 5v11a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2Z',
			brand: true
		},
		settings: {
			main: 'M4 6h16 M4 12h16 M4 18h16',
			detail:
				'M14 6a2 2 0 1 0 4 0 2 2 0 0 0-4 0Z M6 12a2 2 0 1 0 4 0 2 2 0 0 0-4 0Z M13 18a2 2 0 1 0 4 0 2 2 0 0 0-4 0Z',
			accent: 'M16 6h.01 M8 12h.01 M15 18h.01',
			brand: true
		},
		upload: {
			main: 'M7.2 18.8H6a3.2 3.2 0 0 1-.5-6.4A6.6 6.6 0 0 1 18 10.1a4.4 4.4 0 0 1-.8 8.7H16',
			detail: 'M12 20V9 M8.5 12.5 12 9l3.5 3.5',
			accent: 'M8 20h8',
			fill: 'M7.2 18.8H6a3.2 3.2 0 0 1-.5-6.4A6.6 6.6 0 0 1 18 10.1a4.4 4.4 0 0 1-.8 8.7H16Z',
			brand: true
		},
		trash: {
			main: 'M4 7h16 M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2 M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12',
			detail: 'M10 11v6 M14 11v6'
		},
		link: {
			main: 'M10 13a4 4 0 0 0 5.7.4l3-3A4 4 0 0 0 13 4.7l-1.7 1.7 M14 11a4 4 0 0 0-5.7-.4l-3 3A4 4 0 0 0 11 19.3l1.7-1.7'
		},
		shield: {
			main: 'M12 2.8 20 6v5.7c0 4.4-3 8.3-8 9.8-5-1.5-8-5.4-8-9.8V6Z',
			detail: 'M12 7.5a2.6 2.6 0 0 1 1 5v3h-2v-3a2.6 2.6 0 0 1 1-5Z',
			accent: 'M12 10.1h.01',
			fill: 'M12 2.8 20 6v5.7c0 4.4-3 8.3-8 9.8-5-1.5-8-5.4-8-9.8V6Z',
			brand: true
		},
		globe: {
			main: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Z',
			detail:
				'M3.6 9h16.8 M3.6 15h16.8 M12 3c2.3 2.5 3.4 5.5 3.4 9S14.3 18.5 12 21 M12 3C9.7 5.5 8.6 8.5 8.6 12s1.1 6.5 3.4 9',
			accent: 'M18.8 7.1h.01 M5.2 16.9h.01',
			fill: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Z',
			brand: true
		},
		sun: {
			main: 'M12 7a5 5 0 1 0 0 10 5 5 0 0 0 0-10Z',
			detail:
				'M12 2v2 M12 20v2 M4.9 4.9l1.4 1.4 M17.7 17.7l1.4 1.4 M2 12h2 M20 12h2 M4.9 19.1l1.4-1.4 M17.7 6.3l1.4-1.4'
		},
		moon: {
			main: 'M20 14.5A8.5 8.5 0 0 1 9.5 4a8.5 8.5 0 1 0 10.5 10.5Z',
			accent: 'M17.5 6.2h.01'
		},
		menu: { main: 'M4 6h16 M4 12h16 M4 18h16' },
		refresh: {
			main: 'M20 6v5h-5 M4 18v-5h5 M18.5 10A7 7 0 0 0 6.2 7.5L4 11 M5.5 14A7 7 0 0 0 17.8 16.5L20 13'
		},
		plus: { main: 'M12 5v14 M5 12h14' },
		close: { main: 'M6 6l12 12 M18 6L6 18' },
		check: { main: 'M5 12.5l4.5 4.5L19 7' },
		copy: {
			main: 'M9 9h9a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1Z M5 15H4a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1h9a1 1 0 0 1 1 1v1'
		},
		signOut: { main: 'M15 17l5-5-5-5 M20 12H9 M12 4H6a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h6' },
		warning: {
			main: 'M12 4.5 2.8 20h18.4Z',
			detail: 'M12 10v4 M12 17.5v.01'
		},
		disk: {
			main: 'M4 7.2C4 5 7.6 3.3 12 3.3S20 5 20 7.2v9.6c0 2.2-3.6 3.9-8 3.9s-8-1.7-8-3.9Z',
			detail: 'M4 7.2c0 2.2 3.6 3.9 8 3.9s8-1.7 8-3.9 M4 12c0 2.2 3.6 3.9 8 3.9s8-1.7 8-3.9',
			accent: 'M16.8 15.2h.01 M16.8 18.1h.01',
			fill: 'M4 7.2C4 5 7.6 3.3 12 3.3S20 5 20 7.2v9.6c0 2.2-3.6 3.9-8 3.9s-8-1.7-8-3.9Z',
			brand: true
		},
		memory: {
			main: 'M7 7h10v10H7Z M4 10h3 M4 14h3 M17 10h3 M17 14h3 M10 4v3 M14 4v3 M10 17v3 M14 17v3',
			detail: 'M10 10h4v4h-4Z',
			accent: 'M12 12h.01',
			fill: 'M7 7h10v10H7Z',
			brand: true
		},
		cache: {
			main: 'M12 3.5c4.4 0 8 1.5 8 3.3v10.4c0 1.8-3.6 3.3-8 3.3s-8-1.5-8-3.3V6.8c0-1.8 3.6-3.3 8-3.3Z',
			detail: 'M4 6.8c0 1.8 3.6 3.3 8 3.3s8-1.5 8-3.3 M4 12c0 1.8 3.6 3.3 8 3.3s8-1.5 8-3.3',
			accent: 'M16.8 16.7h.01 M16.8 19h.01',
			fill: 'M12 3.5c4.4 0 8 1.5 8 3.3v10.4c0 1.8-3.6 3.3-8 3.3s-8-1.5-8-3.3V6.8c0-1.8 3.6-3.3 8-3.3Z',
			brand: true
		},
		chevronRight: { main: 'M9 6l6 6-6 6' },
		key: {
			main: 'M15.5 3.5a5 5 0 1 0 3.6 8.6L21 14l1-3-2-1.4a5 5 0 0 0-4.5-6.1Z',
			detail: 'M13 9 3.5 18.5 3 21l2.5-.5L15 11',
			accent: 'M16 8.5h.01',
			fill: 'M15.5 3.5a5 5 0 1 0 3.6 8.6L21 14l1-3-2-1.4a5 5 0 0 0-4.5-6.1Z',
			brand: true
		},
		plug: {
			main: 'M8 8.5h8v3.2a4 4 0 0 1-8 0Z M10 3v5.5 M14 3v5.5 M12 15.7V21',
			detail: 'M6 8.5h12',
			accent: 'M12 21h.01',
			fill: 'M8 8.5h8v3.2a4 4 0 0 1-8 0Z',
			brand: true
		},
		eye: {
			main: 'M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7Z',
			detail: 'M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6Z'
		},
		eyeOff: {
			main: 'M4 4l16 16 M9.9 5.2A9.6 9.6 0 0 1 12 5c6.5 0 10 7 10 7a17 17 0 0 1-3.3 4.1 M6.6 7C4 8.8 2 12 2 12s3.5 7 10 7c1.6 0 3-.4 4.2-1 M9.9 10a3 3 0 0 0 4.2 4.2'
		}
	} as const;

	export type IconName = keyof typeof ICONS_RAW;
	const ICONS: Record<IconName, IconDefinition> = ICONS_RAW;
</script>

<script lang="ts">
	interface Props {
		name: IconName;
		class?: string;
		/// Set only for a standalone meaningful icon. When visible text already
		/// names the action, the icon is decorative and hidden from assistive tech.
		label?: string;
	}

	let { name, class: className = 'size-4', label }: Props = $props();
	const instanceId = $props.id();
	const gradientId = `mb-icon-${instanceId}`;
	const accentId = `mb-icon-accent-${instanceId}`;
	const icon = $derived(ICONS[name]);
</script>

<svg
	class="shrink-0 {className}"
	viewBox="0 0 24 24"
	fill="none"
	stroke-linecap="round"
	stroke-linejoin="round"
	role={label ? 'img' : 'presentation'}
	aria-label={label}
	aria-hidden={label ? undefined : 'true'}
>
	{#if icon.brand}
		<defs>
			<linearGradient id={gradientId} x1="3" y1="3" x2="21" y2="21" gradientUnits="userSpaceOnUse">
				<stop stop-color="currentColor" />
				<stop offset="0.62" stop-color="var(--color-secondary)" />
				<stop offset="1" stop-color="var(--color-primary)" />
			</linearGradient>
			<linearGradient id={accentId} x1="5" y1="4" x2="19" y2="20" gradientUnits="userSpaceOnUse">
				<stop stop-color="var(--color-accent)" />
				<stop offset="1" stop-color="var(--color-warning)" />
			</linearGradient>
		</defs>
	{/if}

	{#if icon.fill}
		<path
			d={icon.fill}
			fill={icon.brand ? `url(#${gradientId})` : 'currentColor'}
			fill-opacity="0.14"
			stroke="none"
		/>
	{/if}
	<path
		d={icon.main}
		stroke={icon.brand ? `url(#${gradientId})` : 'currentColor'}
		stroke-width="1.8"
	/>
	{#if icon.detail}
		<path
			d={icon.detail}
			stroke={icon.brand ? `url(#${gradientId})` : 'currentColor'}
			stroke-width="1.8"
		/>
	{/if}
	{#if icon.accent}
		<path
			d={icon.accent}
			stroke={icon.brand ? `url(#${accentId})` : 'currentColor'}
			stroke-width={icon.brand ? '2.8' : '1.8'}
		/>
	{/if}
</svg>
