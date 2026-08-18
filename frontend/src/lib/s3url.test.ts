import { afterEach, describe, expect, it, vi } from 'vitest';
import type { Session } from './api';
import { objectUrl, s3Endpoint } from './s3url';

// Which host this file picks is not cosmetic: it is what gets signed into a
// presigned URL, and a wrong answer only shows up as SignatureDoesNotMatch on a
// link somebody has already been sent.

function session(overrides: Partial<Session> = {}): Session {
	return {
		authenticated: true,
		username: 'admin',
		role: 'administrator',
		permissions: [],
		usingDefaultCredentials: false,
		s3Port: 9000,
		s3Domain: '',
		s3PublicUrl: '',
		version: 'test',
		...overrides
	};
}

/// The console's own address, which is not the S3 endpoint behind a proxy.
function servedFrom(href: string) {
	vi.stubGlobal('location', new URL(href));
}

afterEach(() => {
	vi.unstubAllGlobals();
});

describe('the S3 endpoint the console builds links against', () => {
	it('uses the stated public origin rather than the console it was loaded from', () => {
		servedFrom('https://console.wepyxis.space/buckets');

		const endpoint = s3Endpoint(
			session({ s3PublicUrl: 'https://s3.wepyxis.space' }),
			'test-bucket'
		);

		// Not console.wepyxis.space:9000, which is the console's hostname glued
		// to the listener's container-internal port — an address that exists
		// nowhere.
		expect(endpoint.host).toBe('s3.wepyxis.space');
		expect(endpoint.secure).toBe(true);
	});

	it('keeps a non-default port from the stated origin', () => {
		servedFrom('https://console.wepyxis.space/');

		expect(s3Endpoint(session({ s3PublicUrl: 'http://192.0.2.10:9000' }), 'b').host).toBe(
			'192.0.2.10:9000'
		);
	});

	it('takes the scheme from the origin, not from the page', () => {
		// A console served over TLS by a proxy that reaches the listener on plain
		// HTTP: the link has to name what the browser will actually use.
		servedFrom('https://console.wepyxis.space/');

		expect(s3Endpoint(session({ s3PublicUrl: 'http://s3.internal:9000' }), 'b').secure).toBe(false);
	});

	it('falls back to the console hostname and the S3 port when nothing is stated', () => {
		servedFrom('http://192.0.2.10:9001/buckets');

		const endpoint = s3Endpoint(session(), 'b');

		// Correct only for a direct deployment, which is exactly when it applies.
		expect(endpoint.host).toBe('192.0.2.10:9000');
		expect(endpoint.secure).toBe(false);
	});

	it('prefers virtual-host addressing over the origin host when a domain is set', () => {
		servedFrom('https://console.wepyxis.space/');

		const endpoint = s3Endpoint(
			session({ s3PublicUrl: 'https://s3.wepyxis.space', s3Domain: 's3.wepyxis.space' }),
			'photos'
		);

		expect(endpoint.host).toBe('photos.s3.wepyxis.space');
		expect(endpoint.secure).toBe(true);
	});
});

describe('object URLs', () => {
	it('is path style against the public origin', () => {
		servedFrom('https://console.wepyxis.space/');

		expect(
			objectUrl(session({ s3PublicUrl: 'https://s3.wepyxis.space' }), 'test-bucket', 'Payslip.pdf')
		).toBe('https://s3.wepyxis.space/test-bucket/Payslip.pdf');
	});

	it('drops the bucket from the path when it is in the host', () => {
		servedFrom('https://console.wepyxis.space/');

		expect(
			objectUrl(
				session({ s3PublicUrl: 'https://s3.wepyxis.space', s3Domain: 's3.wepyxis.space' }),
				'photos',
				'a/b.txt'
			)
		).toBe('https://photos.s3.wepyxis.space/a/b.txt');
	});

	it("encodes to AWS's unreserved set, which is narrower than the browser's", () => {
		servedFrom('https://console.wepyxis.space/');

		expect(
			objectUrl(session({ s3PublicUrl: 'https://s3.wepyxis.space' }), 'b', "a file (1)'s.pdf")
		).toBe('https://s3.wepyxis.space/b/a%20file%20%281%29%27s.pdf');
	});
});
