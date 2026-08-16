// The dashboard ships inside the MonoBucket binary; there is no Node runtime
// alongside it, so everything renders in the browser and talks to the C++ API
// over fetch. Prerendering is off because every view depends on live server
// state.
export const ssr = false;
export const prerender = false;

// Assets are fingerprinted by Vite and served with immutable cache headers, so
// the client router can safely prefetch across the whole app.
export const trailingSlash = 'never';
