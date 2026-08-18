# Deployment secrets

`deploy/docker-compose.yml` reads the console administrator's password from
`admin_password.txt` in this directory. Create it before the first start:

```bash
umask 077
head -c 24 /dev/urandom | base64 > deploy/secrets/admin_password.txt
```

A trailing newline is stripped. The password must be at least 12 characters; a
shorter one aborts startup with the reason, and so does a missing file — there
is no default password, by design.

The file is only read to create or reset the account. Once MonoBucket has
provisioned it the verifier lives in the data volume, so the file can be
removed and the server still starts. Leaving it in place means every restart
resets the password back to whatever it contains: the right behaviour for a
lost password, the wrong one after you have changed it from the console.
