# Deployment secrets

`docker-compose.yml` reads the console administrator's password from
`admin_password.txt` in this directory. Create it before the first start:

```bash
umask 077
head -c 24 /dev/urandom | base64 > secrets/admin_password.txt
```

A trailing newline is stripped, so `echo` works too. The password must be at
least 12 characters; a shorter one aborts startup with the reason.

The file is only read to create or reset the account. Once MonoBucket has
provisioned it, the verifier lives in the data volume — the file can be removed
and the server still starts. Leaving it in place means every restart resets the
password back to whatever it contains, which is the right behaviour for a lost
password and the wrong one after you have changed it from the console.

Nothing in this directory is committed; `.gitignore` sees to that.
