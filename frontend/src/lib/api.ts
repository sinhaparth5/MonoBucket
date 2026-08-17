// The console's view of the C++ API.
//
// Everything crosses `/_mb/api`, which only answers on the console listener and
// only with a session cookie. The browser never holds an S3 secret: the S3
// listener speaks SigV4 and this one speaks sessions, and keeping them apart is
// what lets console access be revoked without rotating storage credentials.

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

export interface Session {
	authenticated: boolean;
	accessKey: string;
	usingDefaultCredentials: boolean;
	s3Port: number;
	s3Domain: string;
	version: string;
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

	login: (accessKey: string, secretKey: string) =>
		request<{ accessKey: string; expiresInSeconds: number }>('/login', {
			method: 'POST',
			body: JSON.stringify({ accessKey, secretKey })
		}),

	logout: () => request<{ signedOut: boolean }>('/logout', { method: 'POST' }),

	overview: () => request<Overview>('/overview'),

	series: () => request<Series>('/series'),

	buckets: () => request<{ buckets: Bucket[] }>('/buckets').then((r) => r.buckets),

	createBucket: (name: string) =>
		request<Bucket>('/buckets', { method: 'POST', body: JSON.stringify({ name }) }),

	deleteBucket: (name: string) =>
		request<{ deleted: string }>(`/buckets?name=${encodeURIComponent(name)}`, {
			method: 'DELETE'
		}),

	setBucketAccess: (name: string, publicRead: boolean) =>
		request<Bucket>('/buckets/access', {
			method: 'POST',
			body: JSON.stringify({ name, publicRead })
		}),

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

	deleteObject: (bucket: string, key: string) =>
		request<{ deleted: string }>(
			`/objects?bucket=${encodeURIComponent(bucket)}&key=${encodeURIComponent(key)}`,
			{ method: 'DELETE' }
		)
};
