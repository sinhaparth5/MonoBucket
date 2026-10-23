// Patchwatch reporter for Node services. No dependencies, Node 18+ (global fetch).
//
// Reports the Node runtime plus every production package from your lockfile
// (npm, pnpm, Yarn or Bun) to PATCHWATCH_URL, authenticated with PATCHWATCH_TOKEN.
// Copy this file into your project.
//
//   import { startPatchwatch } from './patchwatch.mjs';
//   startPatchwatch(); // on boot, then every 6 hours
//
// Or run it once, e.g. from cron or a deploy step: node patchwatch.mjs

import { existsSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { basename, dirname, join } from 'node:path';
import { pathToFileURL } from 'node:url';

const SIX_HOURS = 6 * 60 * 60 * 1000;

const LOCKFILES = {
    'package-lock.json': readNpm,
    'npm-shrinkwrap.json': readNpm,
    'pnpm-lock.yaml': readPnpm,
    'yarn.lock': readYarn,
    'bun.lock': readBun,
};

/**
 * Build the manifest: the Node runtime plus one entry per production package.
 * Patchwatch keeps one version per package name, so the copy closest to your
 * own dependencies wins over nested duplicates.
 */
export async function collectDependencies(
    lockfile = process.env.PATCHWATCH_LOCKFILE ?? findLockfile(),
) {
    const read = LOCKFILES[basename(lockfile)];

    if (!read) {
        throw new Error(
            basename(lockfile) === 'bun.lockb'
                ? 'bun.lockb is binary. Run `bun install --save-text-lockfile` (or upgrade to Bun 1.2+) to get bun.lock.'
                : `Unsupported lockfile ${lockfile}. Use one of: ${Object.keys(LOCKFILES).join(', ')}.`,
        );
    }

    const versions = new Map();

    for (const [name, version] of await read(lockfile)) {
        if (!versions.has(name)) {
            versions.set(name, version);
        }
    }

    return [
        { ecosystem: 'runtime', name: 'node', version: process.version },
        ...[...versions].map(([name, version]) => ({
            ecosystem: 'npm',
            name,
            version,
        })),
    ];
}

function findLockfile() {
    const found = Object.keys(LOCKFILES).find((file) => existsSync(file));

    if (!found) {
        throw new Error(
            `No lockfile in ${process.cwd()}. Set PATCHWATCH_LOCKFILE or copy one of ${Object.keys(LOCKFILES).join(', ')} into the image.`,
        );
    }

    return found;
}

/** package-lock.json v2/v3: every install path is listed and dev packages are flagged. */
async function readNpm(lockfile) {
    const lock = JSON.parse(await readFile(lockfile, 'utf8'));

    if (!lock.packages) {
        throw new Error(
            `${lockfile} has no "packages" map. Regenerate it with npm 7 or newer.`,
        );
    }

    return (
        Object.entries(lock.packages)
            // "" is your own project, links point at workspace folders, dev deps aren't deployed.
            .filter(
                ([path, entry]) =>
                    path !== '' && !entry.link && !entry.dev && entry.version,
            )
            .map(([path, entry]) => ({
                depth: path.split('node_modules/').length,
                name:
                    entry.name ??
                    path.slice(path.lastIndexOf('node_modules/') + 13),
                version: entry.version,
            }))
            .sort((a, b) => a.depth - b.depth)
            .map(({ name, version }) => [name, version])
    );
}

/**
 * pnpm-lock.yaml. v5/v6 flag dev packages; v9 doesn't, so walk the graph
 * from every importer's production dependencies through `snapshots`.
 */
async function readPnpm(lockfile) {
    const lock = parseYamlish(await readFile(lockfile, 'utf8'));
    const major = parseInt(lock.lockfileVersion, 10);

    if (major < 9) {
        // v6 keys look like /name@1.0.0(peer@2.0.0), v5 like /name/1.0.0_peer@2.0.0.
        return Object.entries(lock.packages ?? {})
            .filter(([, entry]) => entry.dev !== 'true')
            .map(([key]) => {
                const id = key.replace(/^\//, '').replace(/\(.*$/, '');
                const at =
                    major >= 6 ? id.lastIndexOf('@') : id.lastIndexOf('/');

                return [id.slice(0, at), id.slice(at + 1).replace(/_.*$/, '')];
            });
    }

    const roots = Object.values(lock.importers ?? {}).flatMap((importer) =>
        prodEntries(importer).map(([name, dep]) => pnpmId(name, dep.version)),
    );

    return walk(roots, (id) => {
        const snapshot = lock.snapshots?.[id];

        if (!snapshot) {
            return undefined; // link:, file: and workspace packages
        }

        const at = id.indexOf('@', 1);

        return {
            id,
            name: id.slice(0, at),
            version: id.slice(at + 1).replace(/\(.*$/, ''),
            deps: prodEntries(snapshot).map(([name, version]) =>
                pnpmId(name, version),
            ),
        };
    });
}

/** An npm alias (`"my-lodash": "lodash@4.17.21"`) already names the real package. */
function pnpmId(name, version) {
    return /^@?[^@(]+@/.test(version) ? version : `${name}@${version}`;
}

/**
 * yarn.lock, classic (v1) or Berry (v2+). Neither flags dev packages, so walk
 * from package.json's dependencies through each entry's descriptors.
 */
async function readYarn(lockfile) {
    const lock = parseYamlish(await readFile(lockfile, 'utf8'));
    // ponytail: roots are the package.json next to the lockfile only; workspace packages' own deps aren't walked
    const manifest = JSON.parse(
        await readFile(join(dirname(lockfile), 'package.json'), 'utf8'),
    );
    const entries = new Map();

    for (const [header, entry] of Object.entries(lock)) {
        // A header lists every range that resolved to this entry: "a@^1.0.0", "a@^1.2.0"
        for (const descriptor of header.split(/["']?,\s*["']?/)) {
            entries.set(descriptor.replace(/^["']|["']$/g, ''), entry);
        }
    }

    const roots = prodEntries(manifest).map(
        ([name, range]) => `${name}@${range}`,
    );

    return walk(roots, (descriptor) => {
        const at = descriptor.indexOf('@', 1);
        const name = descriptor.slice(0, at);
        // Berry prefixes registry ranges with npm:, package.json doesn't.
        const entry =
            entries.get(descriptor) ??
            entries.get(`${name}@npm:${descriptor.slice(at + 1)}`);

        if (!entry?.version || entry.version.endsWith('-use.local')) {
            return undefined; // workspace or unresolved
        }

        return {
            id: `${name}@${entry.version}`,
            name,
            version: entry.version,
            deps: prodEntries(entry).map(([dep, range]) => `${dep}@${range}`),
        };
    });
}

/**
 * bun.lock (Bun 1.2+), JSON with trailing commas. Packages are keyed by
 * install path ("parent/child" for a nested copy); dev packages aren't flagged.
 */
async function readBun(lockfile) {
    const lock = JSON.parse(
        (await readFile(lockfile, 'utf8')).replace(/,(\s*[}\]])/g, '$1'),
    );
    const roots = Object.values(lock.workspaces ?? {}).flatMap((workspace) =>
        prodEntries(workspace).map(([name]) => ({ name, parent: '' })),
    );

    return walk(roots, ({ name, parent }) => {
        // The closest nested copy (parent/name, grandparent/name, ...) wins over
        // the hoisted one, as in node_modules.
        let scope = parent;

        while (scope && !lock.packages?.[`${scope}/${name}`]) {
            scope = scope.slice(0, Math.max(0, scope.lastIndexOf('/')));
        }

        const key = scope ? `${scope}/${name}` : name;

        const [spec, , meta] = lock.packages?.[key] ?? [];
        const version = spec?.slice(spec.indexOf('@', 1) + 1);

        if (!version || version.startsWith('workspace:')) {
            return undefined;
        }

        return {
            id: key,
            name,
            version,
            deps: prodEntries(meta ?? {}).map(([dep]) => ({
                name: dep,
                parent: key,
            })),
        };
    });
}

/** dependencies + optionalDependencies of a manifest or lock entry; devDependencies are left out. */
function prodEntries(manifest) {
    return [
        ...Object.entries(manifest.dependencies ?? {}),
        ...Object.entries(manifest.optionalDependencies ?? {}),
    ];
}

/**
 * Breadth-first walk from the root refs, so the copy closest to your own
 * dependencies comes first. resolve(ref) returns a node or undefined to skip it.
 */
function walk(roots, resolve) {
    const seen = new Set();
    const found = [];
    const queue = [...roots];

    for (let i = 0; i < queue.length; i++) {
        const node = resolve(queue[i]);

        if (node && !seen.has(node.id)) {
            seen.add(node.id);
            found.push([node.name, node.version]);
            queue.push(...node.deps);
        }
    }

    return found;
}

/**
 * Just enough YAML for pnpm-lock.yaml and yarn.lock (classic and Berry):
 * indentation-nested maps of scalar values. Lists and multi-line strings are
 * ignored, since none of the fields read above use them.
 */
function parseYamlish(text) {
    const root = {};
    const stack = [{ indent: -1, map: root }];
    const unquote = (value) =>
        /^(["']).*\1$/.test(value) ? value.slice(1, -1) : value;

    for (const raw of text.split(/\r?\n/)) {
        const line = raw.trim();

        if (line === '' || line.startsWith('#') || line.startsWith('- ')) {
            continue;
        }

        const indent = raw.length - raw.trimStart().length;

        while (indent <= stack.at(-1).indent) {
            stack.pop();
        }

        const parent = stack.at(-1).map;

        if (line.endsWith(':')) {
            const map = {};
            parent[unquote(line.slice(0, -1))] = map;
            stack.push({ indent, map });
            continue;
        }

        let key;
        let value;

        if (line[0] === '"' || line[0] === "'") {
            const close = line.indexOf(line[0], 1);
            key = line.slice(1, close);
            value = line.slice(close + 1).replace(/^:/, '');
        } else {
            // YAML is `key: value`, classic yarn.lock is `key value`.
            const match =
                line.match(/^(\S+?):\s+(.*)$/) ?? line.match(/^(\S+)\s+(.*)$/);

            if (!match) {
                continue;
            }

            [, key, value] = match;
        }

        parent[key] = unquote(value.trim());
    }

    return root;
}

/** Send one heartbeat. Throws on a missing config, network error or non-2xx response. */
export async function reportOnce({
    url = process.env.PATCHWATCH_URL,
    token = process.env.PATCHWATCH_TOKEN,
    lockfile,
} = {}) {
    if (!url || !token) {
        throw new Error('Set PATCHWATCH_URL and PATCHWATCH_TOKEN.');
    }

    const response = await fetch(url, {
        method: 'POST',
        headers: {
            Authorization: `Bearer ${token}`,
            'Content-Type': 'application/json',
            Accept: 'application/json',
        },
        body: JSON.stringify({
            dependencies: await collectDependencies(lockfile),
        }),
        signal: AbortSignal.timeout(30_000),
    });

    if (!response.ok) {
        throw new Error(
            `Patchwatch heartbeat failed: ${response.status} ${await response.text()}`,
        );
    }
}

/**
 * Report now and then on an interval. Failures are logged, never thrown, so a
 * Patchwatch outage can't take your service down. Returns a function that stops it.
 */
export function startPatchwatch({ intervalMs = SIX_HOURS, ...options } = {}) {
    const run = () =>
        reportOnce(options).catch((error) =>
            console.warn(`[patchwatch] ${error.message}`),
        );

    void run();
    const timer = setInterval(run, intervalMs);
    timer.unref(); // don't keep the process alive just for heartbeats

    return () => clearInterval(timer);
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
    await reportOnce();
    console.log('[patchwatch] heartbeat sent');
}
