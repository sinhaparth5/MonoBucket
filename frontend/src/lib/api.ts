// The console's view of the C++ API.
//
// Everything crosses `/_mb/api`, which only answers on the console listener and
// only with a session cookie. The browser never holds an S3 secret: the S3
// listener speaks SigV4 and this one speaks sessions, and keeping them apart is
// what lets console access be revoked without rotating storage credentials.
//
// The one exception proves it — a freshly minted or rotated credential is
// returned once, to be shown once. It is never stored here and never fetched
// again; `credentials()` answers without secrets by design.

const BASE = '/_mb/api';

export class ApiError extends Error {
	readonly status: number;

	constructor(status: number, message: string) {
		super(message);
		this.name = 'ApiError';
		this.status = status;
	}

	get unauthorized(): boolean {
		return this.status === 401;
	}

	/// Signed in, but not entitled. Distinct from `unauthorized` because the
	/// responses differ: a 401 belongs on the login page and a 403 belongs
	/// where the reader already is, with the reason.
	get forbidden(): boolean {
		return this.status === 403;
	}
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
	let response: Response;
	try {
		response = await fetch(`${BASE}${path}`, {
			// The session cookie is HttpOnly, so it only travels if the request
			// asks for credentials explicitly.
			credentials: 'same-origin',
			...init,
			headers: {
				...(init.body ? { 'Content-Type': 'application/json' } : {}),
				...init.headers
			}
		});
	} catch {
		// A dead server and a refused connection are the same thing to the user:
		// the console cannot reach MonoBucket.
		throw new ApiError(0, 'cannot reach the server');
	}

	const text = await response.text();
	const payload = text ? safeParse(text) : null;

	if (!response.ok) {
		const message =
			payload && typeof payload === 'object' && 'error' in payload
				? String((payload as { error: unknown }).error)
				: `request failed with ${response.status}`;
		throw new ApiError(response.status, message);
	}

	return payload as T;
}

function safeParse(text: string): unknown {
	try {
		return JSON.parse(text);
	} catch {
		return null;
	}
}

// --- Shapes ----------------------------------------------------------------

/// The three roles the server defines. Kept as a union rather than a string so
/// a typo in a comparison is a compile error instead of a control that never
/// appears.
export type RoleName = 'administrator' | 'operator' | 'readonly';

/// Permission identifiers as the server spells them. The console uses these to
/// decide what to render; the server decides what to allow. A page that shows a
/// button it should not is a cosmetic bug, and a page that hides one is a
/// courtesy — neither is the enforcement.
export type PermissionName =
	| 'bucket:read'
	| 'bucket:write'
	| 'object:read'
	| 'object:write'
	| 'settings:read'
	| 'capacity:write'
	| 'credential:read'
	| 'credential:write'
	| 'user:read'
	| 'user:write'
	| 'audit:read';

export interface Session {
	authenticated: boolean;
	/// The person signed in, never a credential. The console has no S3 access
	/// key to show, and that is the separation working rather than a field that
	/// went missing.
	username: string;
	role: RoleName;
	/// What this session may do. Empty when signed out.
	permissions: PermissionName[];
	usingDefaultCredentials: boolean;
	s3Port: number;
	s3Domain: string;
	version: string;
}

/// True when the session holds every permission listed.
export function can(
	session: { permissions: readonly PermissionName[] } | null | undefined,
	...needed: PermissionName[]
): boolean {
	if (!session) return false;
	return needed.every((permission) => session.permissions.includes(permission));
}

export interface User {
	username: string;
	role: RoleName;
	disabled: boolean;
	createdAt: string;
	createdAtMs: number;
	updatedAt: string;
	updatedAtMs: number;
	/// Null on an account whose password has never been replaced.
	passwordChangedAt: string | null;
	passwordChangedAtMs: number;
}

/// The catalogue the server ships alongside the user list, so the role picker
/// can only ever offer roles this build actually has.
export interface RoleInfo {
	name: RoleName;
	description: string;
	permissions: PermissionName[];
}

export interface AuditEntry {
	sequence: number;
	at: string;
	atMs: number;
	/// Empty when the request never got as far as naming an identity.
	actor: string;
	/// A dotted verb: `user.create`, `credential.revoke`, `authz.denied`.
	action: string;
	target: string;
	/// False for a refusal, which is the half of the log worth reading.
	allowed: boolean;
	detail: string;
}

export interface Credential {
	accessKeyId: string;
	description: string;
	/// The account the key acts as. A signed request never exceeds what this
	/// user may do, and disabling them stops the key on its next request.
	owner: string;
	createdAt: string;
	createdAtMs: number;
	/// Null until the secret has been replaced at least once.
	rotatedAt: string | null;
	rotatedAtMs: number;
}

/// What create and rotate answer with. The secret is present exactly here and
/// nowhere else: no later read of the credential list returns it, so a page
/// that navigates away has thrown it away.
export interface IssuedCredential extends Credential {
	secretKey: string;
}

export interface Overview {
	server: {
		version: string;
		region: string;
		uptimeSeconds: number;
		residentBytes: number;
		workerThreads: number;
		s3Port: number;
		consolePort: number;
		/// Across both listeners. Drogon counts connections per process, so this
		/// includes the console tab that is asking.
		connections: number;
	};
	storage: {
		engine: string;
		buckets: number;
		objects: number;
		bytes: number;
		uploads: number;
		orphanBlobs: number;
		diskTotalBytes: number;
		diskAvailableBytes: number;
		engineGauges: Record<string, number>;
	};
	capacity: Capacity;
	io: {
		queued: number;
		active: number;
		completed: number;
		rejected: number;
		threads: number;
		limit: number;
	};
	cache: {
		backend: string;
		healthy: boolean;
		entries: number;
		bytes: number;
		limitBytes: number;
		hits: number;
		misses: number;
		hitRatio: number;
		evictions: number;
		rejections: number;
		errors: number;
	};
	s3: {
		requests: number;
		succeeded: number;
		clientErrors: number;
		serverErrors: number;
		shed: number;
		authFailures: number;
		anonymous: number;
		bytesIn: number;
		bytesOut: number;
	};
}

export interface Sample {
	atMs: number;
	spanMs: number;
	requests: number;
	succeeded: number;
	clientErrors: number;
	serverErrors: number;
	shed: number;
	bytesIn: number;
	bytesOut: number;
	cacheHits: number;
	cacheMisses: number;
	objects: number;
	storedBytes: number;
	residentBytes: number;
	cacheBytes: number;
	ioQueued: number;
	ioActive: number;
	connections: number;
}

export interface Series {
	intervalSeconds: number;
	capacity: number;
	samples: Sample[];
}

export interface Bucket {
	name: string;
	createdAt: string;
	createdAtMs: number;
	publicRead: boolean;
	hasPolicy: boolean;
	corsRules: number;
	/// The bucket's storage allocation. Zero means unlimited — buckets made by
	/// plain S3 CreateBucket land there unless the server sets a default.
	quotaBytes: number;
	usedBytes: number;
	/// Multipart parts stored against an upload that has not completed. Charged
	/// to the allocation, but not yet part of any object.
	pendingBytes: number;
	/// null for an unlimited bucket: there is no remainder to draw.
	remainingBytes: number | null;
}

/// What the instance may hand out, and what it already has.
export interface Capacity {
	allocatableBytes: number;
	allocatedBytes: number;
	remainingBytes: number;
	usedBytes: number;
	/// Buckets with no allocation. While any exist, `remainingBytes` is an
	/// upper bound rather than a promise — those buckets can consume capacity
	/// nothing has reserved against them.
	unlimitedBuckets: number;
}

export interface CorsRule {
	id: string;
	allowedOrigins: string[];
	allowedMethods: string[];
	allowedHeaders: string[];
	exposeHeaders: string[];

	// null means the rule sets no max age and the browser chooses. Zero is a
	// different instruction — do not cache this preflight at all — so the two
	// must not collapse into one another.
	maxAgeSeconds: number | null;
}

export interface StoredObject {
	key: string;
	size: number;
	etag: string;
	contentType: string;
	lastModified: string;
	lastModifiedMs: number;
}

export interface ObjectPage {
	bucket: string;
	publicRead: boolean;
	prefix: string;
	delimiter: string;
	objects: StoredObject[];
	prefixes: string[];
	truncated: boolean;
	nextAfter: string;
}

export interface ObjectDetail extends StoredObject {
	bucket: string;
	sha256: string;
	userMetadata: Record<string, string>;
}

export interface Presigned {
	url: string;
	method: string;
	expiresInSeconds: number;
	expiresAtMs: number;
}

export interface PresignQuery {
	bucket: string;
	key: string;
	/// The authority the recipient's browser will send. Signed, so the console
	/// supplies the name it itself reached the server by — see $lib/s3url.
	host: string;
	secure: boolean;
	expiresSeconds: number;
}

export interface BucketPolicy {
	bucket: string;
	/// The document verbatim, or empty when the bucket has none. Never
	/// reformatted on the way through: a policy is read by people as well as by
	/// the server, and re-emitting somebody's JSON with different whitespace is
	/// a diff nobody asked for.
	policy: string;
	/// What the server derived from that document, which is the half that
	/// actually decides whether an unsigned GET works.
	publicRead: boolean;
}

export interface Setting {
	key: string;
	value: string | number | boolean;
	/// The `MONOBUCKET_*` variable behind it, or empty where there is none.
	env: string;
}

export interface ServerConfig {
	version: string;
	settings: Setting[];
	cacheBackendActive: string;
	usingDefaultCredentials: boolean;
}

export interface ListQuery {
	bucket: string;
	prefix?: string;
	delimiter?: string;
	after?: string;
	limit?: number;
}

// --- Calls -----------------------------------------------------------------

export const api = {
	session: () => request<Session>('/session'),

	login: (username: string, password: string) =>
		request<{
			username: string;
			role: RoleName;
			permissions: PermissionName[];
			expiresInSeconds: number;
		}>('/login', {
			method: 'POST',
			body: JSON.stringify({ username, password })
		}),

	logout: () => request<{ signedOut: boolean }>('/logout', { method: 'POST' }),

	overview: () => request<Overview>('/overview'),

	series: () => request<Series>('/series'),

	buckets: () => request<{ buckets: Bucket[] }>('/buckets').then((r) => r.buckets),

	/// The list and the instance capacity arrive together, because a bucket's
	/// allocation only means something beside what is left to allocate.
	bucketsWithCapacity: () => request<{ buckets: Bucket[]; capacity: Capacity }>('/buckets'),

	createBucket: (name: string, quotaBytes: number) =>
		request<Bucket>('/buckets', { method: 'POST', body: JSON.stringify({ name, quotaBytes }) }),

	setBucketQuota: (name: string, quotaBytes: number) =>
		request<{ bucket: Bucket; capacity: Capacity }>('/buckets/quota', {
			method: 'POST',
			body: JSON.stringify({ name, quotaBytes })
		}),

	deleteBucket: (name: string) =>
		request<{ deleted: string }>(`/buckets?name=${encodeURIComponent(name)}`, {
			method: 'DELETE'
		}),

	setBucketAccess: (name: string, publicRead: boolean) =>
		request<Bucket>('/buckets/access', {
			method: 'POST',
			body: JSON.stringify({ name, publicRead })
		}),

	bucketCors: (name: string) =>
		request<{ bucket: string; rules: CorsRule[] }>(
			`/buckets/cors?bucket=${encodeURIComponent(name)}`
		).then((r) => r.rules),

	setBucketCors: (name: string, rules: CorsRule[]) =>
		request<{ bucket: string; rules: CorsRule[] }>('/buckets/cors', {
			method: 'POST',
			body: JSON.stringify({ name, rules })
		}).then((r) => r.rules),

	clearBucketCors: (name: string) =>
		request<{ bucket: string; rules: CorsRule[] }>('/buckets/cors', {
			method: 'DELETE',
			body: JSON.stringify({ name })
		}).then((r) => r.rules),

	bucketPolicy: (name: string) =>
		request<BucketPolicy>(`/buckets/policy?bucket=${encodeURIComponent(name)}`),

	setBucketPolicy: (name: string, policy: string) =>
		request<BucketPolicy>('/buckets/policy', {
			method: 'POST',
			body: JSON.stringify({ name, policy })
		}),

	clearBucketPolicy: (name: string) =>
		request<BucketPolicy>('/buckets/policy', {
			method: 'DELETE',
			body: JSON.stringify({ name })
		}),

	config: () => request<ServerConfig>('/config'),

	credentials: () =>
		request<{ credentials: Credential[] }>('/credentials').then((r) => r.credentials),

	createCredential: (description: string) =>
		request<IssuedCredential>('/credentials', {
			method: 'POST',
			body: JSON.stringify({ description })
		}),

	rotateCredential: (accessKeyId: string) =>
		request<IssuedCredential>('/credentials/rotate', {
			method: 'POST',
			body: JSON.stringify({ accessKeyId })
		}),

	revokeCredential: (accessKeyId: string) =>
		request<{ revoked: string }>(`/credentials?accessKeyId=${encodeURIComponent(accessKeyId)}`, {
			method: 'DELETE'
		}),

	users: () => request<{ users: User[]; roles: RoleInfo[] }>('/users'),

	createUser: (username: string, password: string, role: RoleName) =>
		request<User>('/users', {
			method: 'POST',
			body: JSON.stringify({ username, password, role })
		}),

	/// Role and status only. A password goes through `setPassword`, because who
	/// may change one and what they have to present first are different there.
	updateUser: (username: string, changes: { role?: RoleName; disabled?: boolean }) =>
		request<User & { endedSessions: number }>('/users', {
			method: 'PATCH',
			body: JSON.stringify({ username, ...changes })
		}),

	deleteUser: (username: string) =>
		request<{ deleted: string; revokedCredentials: number; endedSessions: number }>(
			`/users?username=${encodeURIComponent(username)}`,
			{ method: 'DELETE' }
		),

	/// Omit `username` to change your own, which requires the current password
	/// and hands back a fresh session cookie. Pass one to reset somebody else's,
	/// which requires `user:write` and does not ask for theirs.
	setPassword: (
		newPassword: string,
		options: { username?: string; currentPassword?: string } = {}
	) =>
		request<{ username: string; endedSessions: number }>('/users/password', {
			method: 'POST',
			body: JSON.stringify({ newPassword, ...options })
		}),

	audit: (limit = 200) =>
		request<{ entries: AuditEntry[]; capacity: number }>(`/audit?limit=${limit}`),

	objects: (query: ListQuery) => {
		const params = new URLSearchParams({ bucket: query.bucket });
		if (query.prefix) params.set('prefix', query.prefix);
		if (query.delimiter) params.set('delimiter', query.delimiter);
		if (query.after) params.set('after', query.after);
		if (query.limit) params.set('limit', String(query.limit));
		return request<ObjectPage>(`/objects?${params}`);
	},

	object: (bucket: string, key: string) =>
		request<ObjectDetail>(
			`/object?bucket=${encodeURIComponent(bucket)}&key=${encodeURIComponent(key)}`
		),

	// Signed on the server: the browser has a session cookie, not an S3 secret,
	// and handing it one to sign with would defeat keeping the two apart.
	presign: (query: PresignQuery) =>
		request<Presigned>('/presign', { method: 'POST', body: JSON.stringify(query) }),

	deleteObject: (bucket: string, key: string) =>
		request<{ deleted: string }>(
			`/objects?bucket=${encodeURIComponent(bucket)}&key=${encodeURIComponent(key)}`,
			{ method: 'DELETE' }
		)
};

export interface UploadHandle {
	/// Resolves with the stored object once the server has committed it.
	done: Promise<StoredObject>;
	/// Aborts the transfer. The server discards a partial payload rather than
	/// publishing it, so a cancelled upload leaves no object behind.
	cancel: () => void;
}

/// Sends one file and reports progress as it goes.
///
/// XMLHttpRequest rather than fetch, for the one thing it still does that fetch
/// does not: report how much of a request body has gone out. `fetch` can stream
/// a request, but only over HTTP/2 and only with a duplex flag no browser
/// implements for uploads yet, so a progress bar built on it would be a
/// spinner wearing a percentage.
export function uploadObject(
	bucket: string,
	key: string,
	file: File,
	onProgress: (sentBytes: number) => void
): UploadHandle {
	const transfer = new XMLHttpRequest();

	const done = new Promise<StoredObject>((resolve, reject) => {
		transfer.upload.addEventListener('progress', (event) => onProgress(event.loaded));

		transfer.addEventListener('load', () => {
			const payload = safeParse(transfer.responseText);
			if (transfer.status >= 200 && transfer.status < 300) {
				resolve(payload as StoredObject);
				return;
			}
			const message =
				payload && typeof payload === 'object' && 'error' in payload
					? String((payload as { error: unknown }).error)
					: `upload failed with ${transfer.status}`;
			reject(new ApiError(transfer.status, message));
		});

		transfer.addEventListener('error', () => reject(new ApiError(0, 'cannot reach the server')));
		transfer.addEventListener('abort', () => reject(new ApiError(0, 'cancelled')));

		const target = `${BASE}/upload?bucket=${encodeURIComponent(bucket)}&key=${encodeURIComponent(key)}`;
		transfer.open('PUT', target);
		transfer.withCredentials = true;
		// The browser's guess, which is what every S3 client sends too. An empty
		// type means the file had no recognisable extension, and the server's
		// own default says the same thing more honestly.
		if (file.type) transfer.setRequestHeader('Content-Type', file.type);
		transfer.send(file);
	});

	return { done, cancel: () => transfer.abort() };
}
