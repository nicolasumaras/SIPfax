# PPP bulk-upload acceptance receiver

Use with DialUpLab XP 1.2.0 or newer. Earlier clients can ignore the upload
options and perform a GET, which is not upload qualification.

On the PPP server, start a temporary receiver before dialing:

```sh
python3 tools/ppp-upload-fixture.py --freebind --audit /var/log/sipfax/ppp-bulk-upload.jsonl
```

`--freebind` uses Linux IP_FREEBIND to bind 10.64.0.1 before PPP brings that
address up. The receiver listens only on that address, port 8084. Stop it after
testing. It is a lab fixture without authentication; do not expose it publicly.

After dialing, POST to DialUpLab's authenticated
`/api/v1/connections/{connectionId}/probe` with:

```json
{"url":"http://10.64.0.1:8084/upload?attempt=UNIQUE_ID","uploadBytes":1048576,"timeoutSeconds":600}
```

The API caller must allow more than 600 seconds for the response. Start with
12,345 bytes before the 1 MiB transfer. Do not run concurrent probes.

Require all of the following for acceptance:

- Client reports POST, sourceIpv4 10.64.0.2, uploadComplete true,
  uploadBytesWritten equal to the requested size, complete true and HTTP 200.
- The server audit record for the unique attempt reports peer 10.64.0.2,
  status 200, the exact bytesReceived count and patternValid true.
- Client uploadSha256 and server sha256 match the independently generated
  payload (byte n is n modulo 256). The response is the server's 64-byte ASCII
  hexadecimal digest, so bodySha256 must equal SHA256 of that ASCII string.
- A packet capture on the PPP interface confirms the transfer traverses PPP;
  inspect RAS error counters and link survival before disconnecting.
- After disconnect, SIPfax reports zero sessions, media lines and PPP leases.

`uploadComplete` describes writes to the local request stream. It alone does
not prove delivery. The server hashes received bytes and returns 422 for a
corrupted pattern, or 400 for an invalid/incomplete request. Its JSONL audit is
append-only; retain failed attempts. The body limit is 1 MiB and the per-socket
inactivity timeout is 600 seconds. Use a service lifetime bound as well.

Run fixture unit and local HTTP integration tests:

```sh
python3 -B -m unittest discover -s tools/tests -p test_ppp_upload_fixture.py
```

Save the raw client probe response and the receiver's JSONL audit, then compare
both against an independently generated payload:

```sh
python3 tools/audit-ppp-upload.py --probe client-probe.json \
  --server-audit ppp-bulk-upload.jsonl --attempt UNIQUE_ID --bytes 1048576
```

The command rejects GET responses from older clients, incomplete or corrupt
uploads, missing/nonzero RAS error counters, reset connection duration, and
missing or duplicate server records for the attempt. Its `payloadVerified`
result covers client/server payload evidence only. It does not replace the PPP
packet capture, connection-survival check, or teardown checks above. Keep a
separate unique attempt ID for every probe and retain unsuccessful records.
