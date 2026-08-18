// Presentation helpers shared by every view. Kept here rather than inline so a
// byte count reads the same on the overview, in a listing and in a tooltip.

const BINARY_UNITS = ['B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB'] as const;

/// Binary units, because that is what an object store's own numbers mean.
/// Decimal units would disagree with `du`, the disk gauge and the S3 API.
export function formatBytes(bytes: number, digits = 1): string {
	if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';

	let value = bytes;
	let unit = 0;
	while (value >= 1024 && unit < BINARY_UNITS.length - 1) {
		value /= 1024;
		unit += 1;
	}
	// Whole bytes never need a decimal point; anything scaled does.
	return `${value.toFixed(unit === 0 ? 0 : digits)} ${BINARY_UNITS[unit]}`;
}

/// The units an allocation is entered in. Binary throughout, so the figure the
/// operator types is the figure the server enforces — a "100 GB" that became
/// 93.1 GiB in the bucket list would look like a bug in the arithmetic.
export const ALLOCATION_UNITS = [
	{ label: 'MiB', bytes: 1024 ** 2 },
	{ label: 'GiB', bytes: 1024 ** 3 },
	{ label: 'TiB', bytes: 1024 ** 4 }
] as const;

export type AllocationUnit = (typeof ALLOCATION_UNITS)[number]['label'];

/// Splits a byte count into the largest unit that leaves a whole number, so an
/// allocation set as "2 TiB" comes back out of the form as 2 TiB rather than
/// 2048 GiB.
export function splitAllocation(bytes: number): { amount: number; unit: AllocationUnit } {
	for (const { label, bytes: scale } of [...ALLOCATION_UNITS].reverse()) {
		if (bytes >= scale && bytes % scale === 0) return { amount: bytes / scale, unit: label };
	}
	return { amount: Math.max(1, Math.round(bytes / ALLOCATION_UNITS[0].bytes)), unit: 'MiB' };
}

export function allocationBytes(amount: number, unit: AllocationUnit): number {
	const scale = ALLOCATION_UNITS.find((entry) => entry.label === unit)?.bytes ?? 1;
	return Math.round(amount * scale);
}

/// How full something is, clamped to 0..1. An allocation reduced to below what
/// is stored would otherwise draw a bar past the end of its own track.
export function fillFraction(used: number, total: number): number {
	if (!Number.isFinite(total) || total <= 0) return 0;
	return Math.min(1, Math.max(0, used / total));
}

const COUNT = new Intl.NumberFormat(undefined);

export function formatCount(value: number): string {
	return COUNT.format(Math.round(value));
}

/// Rates below 1 keep a decimal so a trickle of traffic does not render as
/// zero, which reads as "nothing is happening" when something is.
export function formatRate(value: number, suffix = '/s'): string {
	if (!Number.isFinite(value) || value <= 0) return `0${suffix}`;
	if (value < 10) return `${value.toFixed(1)}${suffix}`;
	return `${COUNT.format(Math.round(value))}${suffix}`;
}

export function formatPercent(fraction: number, digits = 1): string {
	if (!Number.isFinite(fraction)) return '0%';
	return `${(fraction * 100).toFixed(digits)}%`;
}

/// Two units at most: "3d 4h" tells you what you need, "3d 4h 17m 9s" does not.
export function formatDuration(seconds: number): string {
	if (!Number.isFinite(seconds) || seconds < 1) return '0s';

	const days = Math.floor(seconds / 86400);
	const hours = Math.floor((seconds % 86400) / 3600);
	const minutes = Math.floor((seconds % 3600) / 60);
	const rest = Math.floor(seconds % 60);

	if (days > 0) return `${days}d ${hours}h`;
	if (hours > 0) return `${hours}h ${minutes}m`;
	if (minutes > 0) return `${minutes}m ${rest}s`;
	return `${rest}s`;
}

const DATE_TIME = new Intl.DateTimeFormat(undefined, {
	dateStyle: 'medium',
	timeStyle: 'short'
});

export function formatTimestamp(ms: number): string {
	return DATE_TIME.format(new Date(ms));
}

const CLOCK = new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit' });

export function formatClock(ms: number): string {
	return CLOCK.format(new Date(ms));
}

/// "1 object", "2 objects". Only regular plurals, which is every noun the
/// console counts.
export function plural(count: number, noun: string): string {
	return `${formatCount(count)} ${noun}${count === 1 ? '' : 's'}`;
}

/// Splits an object key into the folder path the browser walks and the leaf it
/// displays. S3 has no directories; the delimiter is the only thing that makes
/// `a/b/c.txt` look like one, so the split happens here and nowhere else.
export function leafName(key: string, prefix: string): string {
	const relative = key.startsWith(prefix) ? key.slice(prefix.length) : key;
	return relative || key;
}
