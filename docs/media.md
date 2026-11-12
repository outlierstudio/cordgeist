# Media import

`media push` copies one leased file through the accepted daemon. `stage` only
writes bytes onto this host. Staging success is not import success. A remote
path or URL is not a host file, and this command does not fetch one.

OCR stays unavailable. `pickerVerified` is not set from software tests. A
copied file remains when indexing fails. Repeating the same `--seq` returns
the original destination and does not create another final asset.

```sh
cordgeist --runtime "$RUNTIME" daemon start
cordgeist --runtime "$RUNTIME" acquire --device ID --reason "import" --lease-file "$RUNTIME/lease.json"
cordgeist --runtime "$RUNTIME" screenshot --lease-file "$RUNTIME/lease.json" --output "$RUNTIME/shot.png"
cordgeist --runtime "$RUNTIME" tap --lease-file "$RUNTIME/lease.json" --frame FRAME --x X --y Y
# Keep the lease held until media push completes.

printf '%s\n' "$TOKEN" | cat - "$RUNTIME/shot.png" | \
  cordgeist --runtime "$RUNTIME" stage --token-file "$RUNTIME/mcp.token" --kind image
cordgeist --runtime "$RUNTIME" media push \
  --lease-file "$RUNTIME/lease.json" \
  --source "$RUNTIME/staging/FILE" \
  --kind image
cordgeist --runtime "$RUNTIME" release --lease-file "$RUNTIME/lease.json"
```

The token is the first stdin line and must match the mode `0600` token file.
Do not put it in the command line. Software imports land under
`/sdcard/Pictures/Cordgeist` or `/sdcard/Movies/Cordgeist` with an opaque name.
Native imports use the exact leased phone through bounded ADB transfer. Sources
must be regular owned files inside the private runtime/staging root. Destinations
are bound to the opaque lease and sequence; new leases cannot overwrite earlier
sequence destinations. copied/indexed/pickerVerified remain independent.

Note 9 SM-N960F / Android 10 build QP1A.190711.020.N960FXXU4DSLB live copy
and same-sequence retry were measured: copied known, indexed error (scan-file
timeout), picker unknown, no URI. This is an honest partial result; no gallery
visibility is claimed. Broader historical G1–G4 media acceptance remains outside v1.
