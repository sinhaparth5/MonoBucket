import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { api, ApiError, can } from './api';

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

describe('permissions', () => {
	it('answers from the list the server sent', () => {
		const session = { permissions: ['bucket:read', 'object:read'] as const };

		expect(can(session, 'bucket:read')).toBe(true);
		expect(can(session, 'object:read')).toBe(true);
		expect(can(session, 'bucket:write')).toBe(false);
	});

	it('requires every permission asked for, not any of them', () => {
		const session = { permissions: ['bucket:read'] as const };

		expect(can(session, 'bucket:read', 'bucket:write')).toBe(false);
		expect(can(session, 'bucket:read', 'bucket:read')).toBe(true);
	});

	it('treats no session as holding nothing', () => {
		// The signed-out shell asks this before it knows anything, and the
		// answer has to be "no" rather than a crash.
		expect(can(null, 'bucket:read')).toBe(false);
		expect(can(undefined, 'user:write')).toBe(false);
		expect(can({ permissions: [] }, 'settings:read')).toBe(false);
	});
});

describe('users', () => {
	it('reads the accounts and the role catalogue together', async () => {
		respondWith({
			status: 200,
			body: {
				users: [{ username: 'admin', role: 'administrator', disabled: false }],
				roles: [{ name: 'administrator', description: 'everything', permissions: [] }]
			}
		});

		const answer = await api.users();

		expect(calls[0].url).toBe('/_mb/api/users');
		expect(answer.users).toHaveLength(1);
		// The picker is built from what the server shipped, so it can never
		// offer a role this build does not have.
		expect(answer.roles[0].name).toBe('administrator');
	});

	it('creates a user with a password and a role', async () => {
		respondWith({ status: 201, body: { username: 'sam', role: 'operator' } });

		await api.createUser('sam', 'a long enough password', 'operator');

		expect(calls[0].init.method).toBe('POST');
		expect(bodyOf(calls[0])).toEqual({
			username: 'sam',
			password: 'a long enough password',
			role: 'operator'
		});
	});

	it('patches only what changed', async () => {
		respondWith({ status: 200, body: { username: 'sam', role: 'readonly', endedSessions: 2 } });

		const result = await api.updateUser('sam', { role: 'readonly' });

		expect(calls[0].init.method).toBe('PATCH');
		// No `disabled` key: sending one would ask the server to reassert a
		// status this call was never about.
		expect(bodyOf(calls[0])).toEqual({ username: 'sam', role: 'readonly' });
		expect(result.endedSessions).toBe(2);
	});

	it('disables without touching the role', async () => {
		respondWith({ status: 200, body: { username: 'sam', disabled: true, endedSessions: 1 } });

		await api.updateUser('sam', { disabled: true });

		expect(bodyOf(calls[0])).toEqual({ username: 'sam', disabled: true });
	});

	it('surfaces the last-administrator refusal as a conflict', async () => {
		respondWith({ status: 409, body: { error: 'this is the last enabled administrator' } });

		await expect(api.updateUser('admin', { role: 'readonly' })).rejects.toMatchObject({
			status: 409,
			message: 'this is the last enabled administrator'
		});
	});

	it('deletes with the username escaped into the query', async () => {
		respondWith({
			status: 200,
			body: { deleted: 'sam', revokedCredentials: 2, endedSessions: 1 }
		});

		const result = await api.deleteUser('a b');

		expect(calls[0].url).toBe('/_mb/api/users?username=a%20b');
		expect(calls[0].init.method).toBe('DELETE');
		// The keys go with the account, and the caller is told how many.
		expect(result.revokedCredentials).toBe(2);
	});
});

describe('passwords', () => {
	it('changes your own with the current one and no username', async () => {
		respondWith({ status: 200, body: { username: 'sam', endedSessions: 3 } });

		await api.setPassword('a brand new password', { currentPassword: 'the old one' });

		expect(calls[0].url).toBe('/_mb/api/users/password');
		expect(bodyOf(calls[0])).toEqual({
			newPassword: 'a brand new password',
			currentPassword: 'the old one'
		});
		// Omitting the username is what makes this a self-change rather than a
		// reset, so its absence is the assertion.
		expect(bodyOf(calls[0])).not.toHaveProperty('username');
	});

	it('resets somebody else with a username and no current password', async () => {
		respondWith({ status: 200, body: { username: 'sam', endedSessions: 0 } });

		await api.setPassword('a brand new password', { username: 'sam' });

		expect(bodyOf(calls[0])).toEqual({ newPassword: 'a brand new password', username: 'sam' });
		expect(bodyOf(calls[0])).not.toHaveProperty('currentPassword');
	});

	it('reports a wrong current password as a 401', async () => {
		respondWith({ status: 401, body: { error: 'the current password is wrong' } });

		await expect(
			api.setPassword('a brand new password', { currentPassword: 'wrong' })
		).rejects.toMatchObject({ status: 401 });
	});
});

describe('the activity log', () => {
	it('asks for a bounded number of entries', async () => {
		respondWith({ status: 200, body: { entries: [], capacity: 5000 } });

		await api.audit();
		expect(calls[0].url).toBe('/_mb/api/audit?limit=200');

		await api.audit(25);
		expect(calls[1].url).toBe('/_mb/api/audit?limit=25');
	});

	it('reports a refusal to read it as forbidden rather than unauthorized', async () => {
		respondWith({ status: 403, body: { error: 'this account is not permitted to audit:read' } });

		const failure = await api.audit().catch((cause) => cause);

		// The two land in different places in the console: a 401 belongs on the
		// login page and a 403 belongs where the reader already is.
		expect(failure).toBeInstanceOf(ApiError);
		expect(failure.forbidden).toBe(true);
		expect(failure.unauthorized).toBe(false);
	});
});

describe('storage allocations', () => {
	it('sends an allocation with every bucket it creates', async () => {
		respondWith({ status: 201, body: { name: 'photos', quotaBytes: 1073741824 } });

		await api.createBucket('photos', 1073741824);

		expect(calls[0].url).toBe('/_mb/api/buckets');
		expect(calls[0].init.method).toBe('POST');
		// The server refuses a create with no allocation, so a console that
		// omitted the field would fail every creation — asserted here rather
		// than discovered there.
		expect(bodyOf(calls[0])).toEqual({ name: 'photos', quotaBytes: 1073741824 });
	});

	it('reads the bucket list and the instance capacity from one call', async () => {
		respondWith({
			status: 200,
			body: {
				buckets: [{ name: 'photos', quotaBytes: 100, usedBytes: 40, remainingBytes: 60 }],
				capacity: {
					allocatableBytes: 1000,
					allocatedBytes: 100,
					remainingBytes: 900,
					usedBytes: 40,
					unlimitedBuckets: 0
				}
			}
		});

		const answer = await api.bucketsWithCapacity();

		expect(calls).toHaveLength(1);
		expect(calls[0].url).toBe('/_mb/api/buckets');
		expect(answer.buckets[0].name).toBe('photos');
		expect(answer.capacity.remainingBytes).toBe(900);
	});

	it('posts a changed allocation to the quota route', async () => {
		respondWith({
			status: 200,
			body: {
				bucket: { name: 'photos', quotaBytes: 2048 },
				capacity: { allocatableBytes: 4096, allocatedBytes: 2048, remainingBytes: 2048 }
			}
		});

		const saved = await api.setBucketQuota('photos', 2048);

		expect(calls[0].url).toBe('/_mb/api/buckets/quota');
		expect(calls[0].init.method).toBe('POST');
		expect(bodyOf(calls[0])).toEqual({ name: 'photos', quotaBytes: 2048 });
		// The response carries the new instance total, so the page that made the
		// change does not have to re-list to find out what is left.
		expect(saved.capacity.remainingBytes).toBe(2048);
	});

	it('surfaces the server refusal rather than pre-empting it', async () => {
		respondWith({ status: 409, body: { error: 'bucket already holds more than that' } });

		await expect(api.setBucketQuota('photos', 1)).rejects.toBeInstanceOf(ApiError);
	});

	it('leaves zero meaning unlimited on the way back', async () => {
		respondWith({
			status: 200,
			body: {
				buckets: [{ name: 'legacy', quotaBytes: 0, usedBytes: 5, remainingBytes: null }],
				capacity: {
					allocatableBytes: 100,
					allocatedBytes: 0,
					remainingBytes: 100,
					usedBytes: 5,
					unlimitedBuckets: 1
				}
			}
		});

		const answer = await api.bucketsWithCapacity();

		expect(answer.buckets[0].quotaBytes).toBe(0);
		// null, not zero: there is no remainder to draw for a bucket with no
		// allocation, and a zero would render as a full progress bar.
		expect(answer.buckets[0].remainingBytes).toBeNull();
		expect(answer.capacity.unlimitedBuckets).toBe(1);
	});
});
