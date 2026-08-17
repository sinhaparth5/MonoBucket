import { redirect } from '@sveltejs/kit';
import { resolve } from '$app/paths';
import { api } from '$lib/api';

// The guard for every signed-in view. It runs in the browser because the app is
// a static SPA served out of the binary — there is no Node process to check a
// cookie before the HTML goes out, so the first thing the shell does is ask.
//
// A failed request and an anonymous session both land on the login page: from
// here they are the same answer, which is "you cannot see this yet".
export const load = async () => {
	const session = await api.session().catch(() => null);
	if (!session?.authenticated) redirect(307, resolve('/login'));
	return { session };
};
