import { defineConfig } from 'vite';
import adapter from '@sveltejs/adapter-static';
import { sveltekit } from '@sveltejs/kit/vite';
import tailwindcss from '@tailwindcss/vite';

export default defineConfig({
	plugins: [
		tailwindcss(),
		sveltekit({
			compilerOptions: {
				// Force runes mode for the project, except for libraries. Can be removed in svelte 6.
				runes: ({ filename }) =>
					filename.split(/[/\\]/).includes('node_modules') ? undefined : true
			},
			// The dashboard is embedded in the MonoBucket binary and served as a
			// single-page app: the C++ asset store hands `index.html` to any
			// unmatched console route and lets SvelteKit resolve it client-side.
			adapter: adapter({
				pages: 'build',
				assets: 'build',
				fallback: 'index.html',
				precompress: false
			})
		})
	],
	// `pnpm dev` serves the console from Vite while the C++ server runs beside
	// it. The API is proxied rather than reached by absolute URL so the browser
	// sees one origin: the session cookie is SameSite=Strict, and a cross-origin
	// XHR would simply not carry it.
	server: {
		proxy: {
			'/_mb': { target: 'http://127.0.0.1:9001', changeOrigin: false }
		}
	}
});
