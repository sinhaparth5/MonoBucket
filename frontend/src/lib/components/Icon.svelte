<script lang="ts" module>
	// One icon set, drawn on the same 24×24 grid with the same 2px stroke and the
	// same round caps, so nothing in the console looks borrowed from somewhere
	// else. The geometry is inlined rather than pulled from an icon package for
	// the reason everything else here is inlined: the console is compiled into
	// the binary and must not fetch anything, and the twenty shapes actually used
	// are cheaper as markup than a tree-shaken dependency is as a build step.
	//
	// Emoji are not an option — they render as somebody else's artwork at a size
	// we do not control and carry no `currentColor`.
	const PATHS = {
		overview: 'M3 13h4l3 7 4-16 3 9h4',
		// A pail rather than a cylinder: the cylinder is already the cache, and two
		// stacked ellipses are indistinguishable from each other at 14px anyway.
		bucket: 'M3.5 7h17l-1.7 12.3a2 2 0 0 1-2 1.7H7.2a2 2 0 0 1-2-1.7Z M8 3h8l1 4H7Z',
		folder: 'M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z',
		file: 'M14 3v5h5 M6 3h8l5 5v11a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2Z',
		settings:
			'M4 6h10 M18 6h2 M4 12h2 M10 12h10 M4 18h10 M18 18h2 M16 6a2 2 0 1 0 0-.01 M8 12a2 2 0 1 0 0-.01 M16 18a2 2 0 1 0 0-.01',
		upload: 'M12 16V4 M8 8l4-4 4 4 M4 16v2a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2',
		trash:
			'M4 7h16 M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2 M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12',
		link: 'M10 13a4 4 0 0 0 5.7.4l3-3A4 4 0 0 0 13 4.7l-1.7 1.7 M14 11a4 4 0 0 0-5.7-.4l-3 3A4 4 0 0 0 11 19.3l1.7-1.7',
		shield: 'M12 3l7 3v5.5c0 4.3-2.9 8.2-7 9.5-4.1-1.3-7-5.2-7-9.5V6Z M9.5 12l1.8 1.8L15 10',
		globe:
			'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Z M3.5 9h17 M3.5 15h17 M12 3c2.4 2.4 3.6 5.4 3.6 9S14.4 18.6 12 21 M12 3C9.6 5.4 8.4 8.4 8.4 12s1.2 6.6 3.6 9',
		sun: 'M12 7a5 5 0 1 0 0 10 5 5 0 0 0 0-10Z M12 2v2 M12 20v2 M4.9 4.9l1.4 1.4 M17.7 17.7l1.4 1.4 M2 12h2 M20 12h2 M4.9 19.1l1.4-1.4 M17.7 6.3l1.4-1.4',
		moon: 'M20 14.5A8.5 8.5 0 0 1 9.5 4a8.5 8.5 0 1 0 10.5 10.5Z',
		menu: 'M4 6h16 M4 12h16 M4 18h16',
		refresh:
			'M20 6v5h-5 M4 18v-5h5 M18.5 10A7 7 0 0 0 6.2 7.5L4 11 M5.5 14A7 7 0 0 0 17.8 16.5L20 13',
		plus: 'M12 5v14 M5 12h14',
		close: 'M6 6l12 12 M18 6L6 18',
		check: 'M5 12.5l4.5 4.5L19 7',
		copy: 'M9 9h9a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1Z M5 15H4a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1h9a1 1 0 0 1 1 1v1',
		signOut: 'M15 17l5-5-5-5 M20 12H9 M12 4H6a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h6',
		warning: 'M12 4.5 2.8 20h18.4Z M12 10v4 M12 17.5v.01',
		disk: 'M3 12a9 9 0 1 1 18 0 9 9 0 0 1-18 0Z M12 9.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5Z M14 14l4.5 4.5',
		memory: 'M7 7h10v10H7Z M4 10h3 M4 14h3 M17 10h3 M17 14h3 M10 4v3 M14 4v3 M10 17v3 M14 17v3',
		cache:
			'M12 4c4.4 0 8 1.3 8 3s-3.6 3-8 3-8-1.3-8-3 3.6-3 8-3Z M4 7v10c0 1.7 3.6 3 8 3s8-1.3 8-3V7 M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3',
		chevronRight: 'M9 6l6 6-6 6',
		key: 'M15.5 3.5a5 5 0 1 0 3.6 8.6L21 14l1-3-2-1.4a5 5 0 0 0-4.5-6.1Z M13 9 3.5 18.5 3 21l2.5-.5L15 11',
		plug: 'M9 3v6 M15 3v6 M6 9h12v2a6 6 0 0 1-6 6 6 6 0 0 1-6-6Z M12 17v4',
		eye: 'M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7Z M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6Z',
		eyeOff:
			'M4 4l16 16 M9.9 5.2A9.6 9.6 0 0 1 12 5c6.5 0 10 7 10 7a17 17 0 0 1-3.3 4.1 M6.6 7C4 8.8 2 12 2 12s3.5 7 10 7c1.6 0 3-.4 4.2-1 M9.9 10a3 3 0 0 0 4.2 4.2'
	} as const;

	export type IconName = keyof typeof PATHS;
</script>

<script lang="ts">
	interface Props {
		name: IconName;
		/// Tailwind size utility. Icons are sized by a token rather than a pixel
		/// value so a row of them never ends up mixing 18px and 20px.
		class?: string;
		/// Set for a standalone meaningful icon; omitted the icon is decorative
		/// and hidden from assistive technology, which is right when a visible
		/// label already says the same thing.
		label?: string;
	}

	let { name, class: className = 'size-4', label }: Props = $props();
</script>

<svg
	class="shrink-0 {className}"
	viewBox="0 0 24 24"
	fill="none"
	stroke="currentColor"
	stroke-width="2"
	stroke-linecap="round"
	stroke-linejoin="round"
	role={label ? 'img' : 'presentation'}
	aria-label={label}
	aria-hidden={label ? undefined : 'true'}
>
	{#each PATHS[name].split(' M') as segment, index (index)}
		<path d={index === 0 ? segment : `M${segment}`} />
	{/each}
</svg>
