import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { api, ApiError } from './api';

// The console's logic is which endpoint gets called, with what body, and what
// it does with the answer. All of that is reachable with a stubbed `fetch`,
// which is why this suite runs in node rather than a browser.

interface Call {
	url: string;
	init: RequestInit;
}

let calls: Call[] = [];

/// Queues one response per call, in order.
function respondWith(...responses: { status: number; body: unknown }[]) {
	let index = 0;
	vi.stubGlobal(
		'fetch',
		vi.fn(async (url: string, init: RequestInit = {}) => {
			calls.push({ url, init });
			const next = responses[Math.min(index, responses.length - 1)];
			index += 1;
			return {
				ok: next.status >= 200 && next.status < 300,
				status: next.status,
				text: async () => (next.body === undefined ? '' : JSON.stringify(next.body))
			} as Response;
		})
	);
}

function bodyOf(call: Call): Record<string, unknown> {
	return JSON.parse(String(call.init.body));
}

beforeEach(() => {
	calls = [];
});

afterEach(() => {
	vi.unstubAllGlobals();
});

describe('signing in', () => {
	it('posts a username and password, never a key pair', async () => {
		respondWith({ status: 200, body: { username: 'admin', expiresInSeconds: 43200 } });

		const result = await api.login('admin', 'a long enough password');

		expect(calls).toHaveLength(1);
		expect(calls[0].url).toBe('/_mb/api/login');
		expect(calls[0].init.method).toBe('POST');
		expect(bodyOf(calls[0])).toEqual({ username: 'admin', password: 'a long enough password' });

		// The old shape would have sent these. Asserted by absence because the
		// regression is silent: a server that still accepted them would pass
		// every other test in this file.
		expect(bodyOf(calls[0])).not.toHaveProperty('accessKey');
		expect(bodyOf(calls[0])).not.toHaveProperty('secretKey');

		expect(result.username).toBe('admin');
	});

	it('sends the session cookie with every request', async () => {
		respondWith({ status: 200, body: { username: 'admin', expiresInSeconds: 43200 } });
		await api.login('admin', 'a long enough password');

		// The cookie is HttpOnly, so it only travels when the request asks.
		expect(calls[0].init.credentials).toBe('same-origin');
	});

	it('surfaces a rejected sign-in as a 401 without inventing detail', async () => {
		respondWith({ status: 401, body: { error: 'invalid username or password' } });

		await expect(api.login('admin', 'wrong')).rejects.toThrow(ApiError);
		await expect(api.login('admin', 'wrong')).rejects.toMatchObject({
			status: 401,
			// One message for every failure mode: the client must not be able to
			// tell "no such user" from "wrong password" either.
			message: 'invalid username or password'
		});
	});

	it('reports a rate-limited sign-in as such', async () => {
		respondWith({ status: 429, body: { error: 'too many failed attempts, try again shortly' } });

		await expect(api.login('admin', 'wrong')).rejects.toMatchObject({ status: 429 });
	});

	it('reports an unreachable server rather than a parse failure', async () => {
		vi.stubGlobal(
			'fetch',
			vi.fn(async () => {
				throw new TypeError('failed to fetch');
			})
		);

		await expect(api.login('admin', 'password')).rejects.toMatchObject({
			status: 0,
			message: 'cannot reach the server'
		});
	});
});

describe('the session', () => {
	it('names the administrator, not a credential', async () => {
		respondWith({
			status: 200,
			body: {
				authenticated: true,
				username: 'admin',
				usingDefaultCredentials: false,
				s3Port: 9000,
				s3Domain: '',
				version: '2026.08.0'
			}
		});

		const session = await api.session();

		expect(calls[0].url).toBe('/_mb/api/session');
		expect(session.username).toBe('admin');
		expect(session).not.toHaveProperty('accessKey');
	});

	it('reports an expired session as unauthenticated rather than throwing', async () => {
		// The layout load asks this on boot to choose between the dashboard and
		// the login form. A throw there would be an error page instead.
		respondWith({
			status: 200,
			body: { authenticated: false, username: '', usingDefaultCredentials: false }
		});

		const session = await api.session();
		expect(session.authenticated).toBe(false);
		expect(session.username).toBe('');
	});

	it('signs out through a POST so a prefetch cannot end the session', async () => {
		respondWith({ status: 200, body: { signedOut: true } });

		const result = await api.logout();

		expect(calls[0].url).toBe('/_mb/api/logout');
		expect(calls[0].init.method).toBe('POST');
		expect(result.signedOut).toBe(true);
	});

	it('marks a 401 so callers can redirect to the login page', async () => {
		respondWith({ status: 401, body: { error: 'not signed in' } });

		await api.credentials().catch((cause) => {
			expect(cause).toBeInstanceOf(ApiError);
			expect((cause as ApiError).unauthorized).toBe(true);
		});
		expect.assertions(2);
	});
});

describe('S3 credentials', () => {
	it('lists keys without secrets', async () => {
		respondWith({
			status: 200,
			body: {
				credentials: [
					{
						accessKeyId: 'MBAAAAAAAAAAAAAAAAAA',
						description: 'backups',
						createdAt: '2026-08-18T00:00:00.000Z',
						createdAtMs: 1,
						rotatedAt: null,
						rotatedAtMs: 0
					}
				]
			}
		});

		const credentials = await api.credentials();

		expect(calls[0].url).toBe('/_mb/api/credentials');
		expect(credentials).toHaveLength(1);
		expect(credentials[0].accessKeyId).toBe('MBAAAAAAAAAAAAAAAAAA');
		expect(credentials[0]).not.toHaveProperty('secretKey');
	});

	it('returns the secret exactly once, when the key is created', async () => {
		respondWith({
			status: 201,
			body: {
				accessKeyId: 'MBAAAAAAAAAAAAAAAAAA',
				secretKey: 'a-secret-worth-forty-characters-exactly1',
				description: 'backups',
				createdAt: '2026-08-18T00:00:00.000Z',
				createdAtMs: 1,
				rotatedAt: null,
				rotatedAtMs: 0
			}
		});

		const issued = await api.createCredential('backups');

		expect(calls[0].url).toBe('/_mb/api/credentials');
		expect(calls[0].init.method).toBe('POST');
		expect(bodyOf(calls[0])).toEqual({ description: 'backups' });
		expect(issued.secretKey).toBe('a-secret-worth-forty-characters-exactly1');
	});

	it('rotates by id and returns the replacement secret', async () => {
		respondWith({
			status: 200,
			body: {
				accessKeyId: 'MBAAAAAAAAAAAAAAAAAA',
				secretKey: 'the-replacement-secret-forty-characters1',
				description: 'backups',
				createdAt: '2026-08-18T00:00:00.000Z',
				createdAtMs: 1,
				rotatedAt: '2026-08-19T00:00:00.000Z',
				rotatedAtMs: 2
			}
		});

		const issued = await api.rotateCredential('MBAAAAAAAAAAAAAAAAAA');

		expect(calls[0].url).toBe('/_mb/api/credentials/rotate');
		expect(bodyOf(calls[0])).toEqual({ accessKeyId: 'MBAAAAAAAAAAAAAAAAAA' });
		// The id survives rotation; only the secret changes.
		expect(issued.accessKeyId).toBe('MBAAAAAAAAAAAAAAAAAA');
		expect(issued.secretKey).toBe('the-replacement-secret-forty-characters1');
	});

	it('revokes through DELETE with the id in the query', async () => {
		respondWith({ status: 200, body: { revoked: 'MBAAAAAAAAAAAAAAAAAA' } });

		const result = await api.revokeCredential('MBAAAAAAAAAAAAAAAAAA');

		expect(calls[0].url).toBe('/_mb/api/credentials?accessKeyId=MBAAAAAAAAAAAAAAAAAA');
		expect(calls[0].init.method).toBe('DELETE');
		expect(result.revoked).toBe('MBAAAAAAAAAAAAAAAAAA');
	});

	it('escapes an id rather than pasting it into the query', async () => {
		respondWith({ status: 404, body: { error: 'no such access key' } });

		await api.revokeCredential('a/b c&d').catch(() => {});
		expect(calls[0].url).toBe('/_mb/api/credentials?accessKeyId=a%2Fb%20c%26d');
	});

	it('reports revoking an unknown key as a 404', async () => {
		respondWith({ status: 404, body: { error: 'no such access key' } });

		await expect(api.revokeCredential('MBAAAAAAAAAAAAAAAAAA')).rejects.toMatchObject({
			status: 404,
			message: 'no such access key'
		});
	});
});
