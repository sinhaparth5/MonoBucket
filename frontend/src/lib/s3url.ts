import type { Session } from '$lib/api';

/// The address an S3 client would use for an object.
///
/// With `MONOBUCKET_S3_DOMAIN` set the server accepts virtual-host addressing,
/// where the bucket is a subdomain; without it, path style is the only form that
/// works, because there is no way to tell `bucket.example.com` from a host that
/// simply is not us.
export interface Endpoint {
	/// The authority the browser will send as `Host`.
	host: string;
	secure: boolean;
}

/// Where this bucket answers.
///
/// Presigning needs the same answer the address bar will arrive at, because the
/// host is *inside the signature* — so both callers below go through here
/// rather than each deciding for themselves, and getting it wrong presents as
/// SignatureDoesNotMatch on a link that has already been handed to somebody.
///
/// `MONOBUCKET_S3_PUBLIC_URL` is preferred over anything derived from the
/// browser. Behind a reverse proxy the console is loaded from a different
/// hostname on a different port from the S3 listener, so `location` describes
/// the console and not the endpoint; the fallback below is only correct when a
/// client reaches this server directly.
export function s3Endpoint(session: Session, bucket: string): Endpoint {
	const base = session.s3PublicUrl ? new URL(session.s3PublicUrl) : null;

	return {
		host: session.s3Domain
			? `${bucket}.${session.s3Domain}`
			: (base?.host ?? `${location.hostname}:${session.s3Port}`),
		// From the stated endpoint when there is one: the console may be served
		// over TLS by a proxy that talks plain HTTP to a listener, or the other
		// way round, and the link has to name the scheme the browser will use.
		secure: base ? base.protocol === 'https:' : location.protocol === 'https:'
	};
}

/// AWS's unreserved set is `A-Za-z0-9-_.~`, one character narrower than
/// encodeURIComponent's, which leaves `!'()*` alone. The difference is invisible
/// to a browser but not to a reader: the presigned URL beside this one is
/// encoded server-side to AWS's rules because the signature covers the bytes,
/// and two spellings of the same key sitting one above the other look like a bug.
function encodeSegment(segment: string): string {
	return encodeURIComponent(segment).replace(
		/[!'()*]/g,
		(char) => '%' + char.charCodeAt(0).toString(16).toUpperCase()
	);
}

export function objectUrl(session: Session, bucket: string, key: string): string {
	const { host, secure } = s3Endpoint(session, bucket);
	const path = key.split('/').map(encodeSegment).join('/');
	const prefix = session.s3Domain ? '' : `/${bucket}`;

	return `${secure ? 'https' : 'http'}://${host}${prefix}/${path}`;
}

/// Puts text on the clipboard, falling back to selecting it when the API is
/// unavailable. `navigator.clipboard` needs a secure context, and a console
/// served over plain HTTP to anything other than localhost does not have one —
/// which is the common case for this server.
export async function copyText(text: string, field?: HTMLInputElement): Promise<boolean> {
	try {
		await navigator.clipboard.writeText(text);
		return true;
	} catch {
		field?.select();
		return false;
	}
}
