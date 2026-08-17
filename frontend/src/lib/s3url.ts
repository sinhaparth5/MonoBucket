import type { Session } from '$lib/api';

/// The address an S3 client would use for an object.
///
/// Built in the browser, not on the server: `MONOBUCKET_HOST` is normally
/// 0.0.0.0, so the only hostname known to reach this deployment is the one the
/// console was itself loaded from. The server contributes the parts the browser
/// cannot know — the S3 port and the endpoint domain, if one is configured.
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

/// Where this bucket answers, as far as this browser can tell.
///
/// Presigning needs the same answer the address bar would arrive at, because the
/// host is inside the signature — so both callers below go through here rather
/// than each deciding for themselves.
export function s3Endpoint(session: Session, bucket: string): Endpoint {
	return {
		host: session.s3Domain
			? `${bucket}.${session.s3Domain}`
			: `${location.hostname}:${session.s3Port}`,
		secure: location.protocol === 'https:'
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
