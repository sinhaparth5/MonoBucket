<script lang="ts">
	import '../app.css';
	import logo from '$lib/assets/monobucket-logo.svg';

	let { children } = $props();
</script>

<svelte:head>
	<link rel="icon" type="image/svg+xml" href={logo} />
</svelte:head>

{@render children()}
