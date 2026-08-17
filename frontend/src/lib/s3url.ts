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
export function objectUrl(session: Session, bucket: string, key: string): string {
	const path = key
		.split('/')
		.map((segment) => encodeURIComponent(segment))
		.join('/');

	if (session.s3Domain) {
		return `${location.protocol}//${bucket}.${session.s3Domain}/${path}`;
	}
	return `${location.protocol}//${location.hostname}:${session.s3Port}/${bucket}/${path}`;
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
