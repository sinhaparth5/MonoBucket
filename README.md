<p align="center">
  <img src="frontend/src/lib/assets/monobucket-logo.svg" width="176" alt="MonoBucket logo">
</p>

<h1 align="center">MonoBucket</h1>

<p align="center">
  Small S3-compatible object storage with a built-in admin dashboard.
</p>

MonoBucket packages a C++20 storage server and a Svelte dashboard into one
binary. It runs without sidecars or an external database.

> [!WARNING]
> MonoBucket is pre-alpha software. Do not store the only copy of important
> data in it. See the [roadmap](ROADMAP.md) before using it outside a test setup.

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

## Project links

[Roadmap](ROADMAP.md) · [Changelog](CHANGELOG.md)

MonoBucket is licensed under [GPL-3.0-or-later](LICENSE).
