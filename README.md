<p align="center">
  <img src="frontend/src/lib/assets/monobucket-logo.svg" width="176" alt="MonoBucket logo">
</p>

<h1 align="center">MonoBucket</h1>

<p align="center">
  Small S3-compatible object storage with a built-in admin dashboard.
</p>

MonoBucket packages a C++20 storage server and a Svelte dashboard into one
binary. It runs without sidecars or an external database.

## What you get

- An S3 API that works with AWS CLI, boto3, and rclone
- Multipart uploads, presigned URLs, bucket policies, CORS, and SigV4
- A browser dashboard for buckets, objects, uploads, metrics, and settings
- One container for `linux/amd64` and `linux/arm64`

## Run it

```bash
docker run --rm \
  -p 9000:9000 \
  -p 9001:9001 \
  -e MONOBUCKET_ROOT_ACCESS_KEY=monobucket \
  -e MONOBUCKET_ROOT_SECRET_KEY=change-me-please \
  -v monobucket-data:/data \
  ghcr.io/sinhaparth5/monobucket:2026.08.0
```

The S3 API listens on [localhost:9000](http://localhost:9000). The dashboard is
at [localhost:9001](http://localhost:9001).

```bash
AWS_ACCESS_KEY_ID=monobucket \
AWS_SECRET_ACCESS_KEY=change-me-please \
aws --endpoint-url http://localhost:9000 s3 ls
```

Change both credentials before exposing the server. The full list of settings
is in [.env.example](.env.example).

## Known limitations

Every project has them; these are MonoBucket's, and they are worth reading
before you rely on it:

- **Concurrent single-PUT uploads are not memory-bounded.** Peak resident memory
  tracks concurrency × object size — four simultaneous 32 MiB PUTs reach about
  151 MiB. MonoBucket's own write path streams the body into the payload tree in
  fixed-size chunks; the residency is below it, in Drogon, which spills a body
  past `MONOBUCKET_MAX_MEMORY_BODY_BYTES` to a temporary file and then hands the
  handler a memory map of the whole file. The pages are file-backed and
  evictable rather than anonymous, so this is a weaker hazard than holding the
  object on the heap, but it is resident. Multipart uploads are unaffected —
  each part is bounded by the client's part size, which is why the AWS CLI and
  rclone moving large objects stay flat. Fixing it means adopting Drogon's
  request-streaming API.
- **`CopyObject` is not implemented** and answers 501 rather than silently
  storing the header's value as an object.
- **No `io_uring` backend.** Blocking I/O runs on a bounded thread pool. Under
  the workloads measured so far the request path, not the I/O pool, is the
  limit.
- **No TLS to Redis.** `rediss://` is refused at startup with the reason;
  terminate TLS in front of Redis or use a private network.
- **Object versioning, lifecycle rules, server-side encryption and IAM-style
  policies** beyond the root credential do not exist.

Two deliberate deviations from S3, both refused at write time rather than stored
as data nothing can later address: object keys containing control characters,
and keys containing a path traversal segment.

## Project links

[Changelog](CHANGELOG.md)

MonoBucket is licensed under [GPL-3.0-or-later](LICENSE).
