# nmap_l2 built-in probe and matcher provenance

All production code/data in this component is **CLEAN-ROOM REIMPLEMENT**. Nmap behavior informed the architecture at research time only; no Nmap source, `nmap-service-probes`, `nmap-services`, `nmap-os-db`, regex corpus, or NSE material is copied or read at Runtime.

| Probe / matcher ID | Behavior | Independent source | Provenance |
|---|---|---|---|
| `passive-ssh-rfc4253` | Require reply to begin with the SSH protocol identification prefix `SSH-`; retain the bounded software-version token as evidence | RFC 4253, SSH Transport Layer Protocol, protocol version exchange | CLEAN-ROOM REIMPLEMENT |
| `passive-ftp-rfc959` | Require an FTP-style `220` greeting **and** explicit case-insensitive `FTP` text before classifying as FTP | RFC 959, File Transfer Protocol reply semantics; conservative One-OS false-positive rule | CLEAN-ROOM REIMPLEMENT |
| `passive-smtp-rfc5321` | Require an SMTP-style `220` greeting plus explicit SMTP-family text (`SMTP`, `ESMTP`, `Postfix`, `Exim`, or `Sendmail`) | RFC 5321 greeting semantics plus independently authored conservative product tokens | CLEAN-ROOM REIMPLEMENT |
| `passive-http-rfc9112` | Require response bytes to begin with an HTTP/1.0 or HTTP/1.1 status line; optionally retain bounded `Server` header evidence | RFC 9110 / RFC 9112 HTTP semantics | CLEAN-ROOM REIMPLEMENT |
| `http-head-rfc9110` | After TCP connect, send one `HEAD / HTTP/1.0` request with `Host` and `Connection: close`, then apply the HTTP matcher | RFC 9110 HEAD method semantics and HTTP field semantics | CLEAN-ROOM REIMPLEMENT |
| discovery fallback ports `80,443,22` | Small product-owned TCP liveness fallback set when caller supplies none | One-OS product engineering choice; **not** derived from Nmap frequency/top-port data | INDEPENDENT PRODUCT DATA |
| AUTO HTTP ports `80,8000,8080,8888` | Select the bounded HTTP HEAD probe on a small explicit product port set; all other AUTO endpoints remain passive | One-OS product engineering choice; no Nmap database/frequency data | INDEPENDENT PRODUCT DATA |

## False-positive policy

The matcher intentionally refuses several common shortcuts:

- a `220` greeting by itself is not FTP or SMTP;
- an `SSH-` substring away from byte zero is not SSH evidence;
- an `HTTP/1.1` substring in arbitrary payload is not an HTTP response;
- a port number alone is not a service match;
- a banner/product string is evidence, not verified device identity.

Device identity is resolved only by the application Device DB above this L2 family.
