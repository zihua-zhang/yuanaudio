# Vendored libsmb2

- Upstream: https://github.com/sahlberg/libsmb2
- Commit: `727feaaa6d0f4fed1bae57745fbc2c5db9a30e64`
- Source archive: https://codeload.github.com/sahlberg/libsmb2/tar.gz/727feaaa6d0f4fed1bae57745fbc2c5db9a30e64
- Archive SHA-256: `ebb5118d338e452b5e8896f0dab4783babe6a368b0c892f1c1a34cf5fdc5f3f7`
- Library license: LGPL-2.1-or-later; see COPYING and LICENCE-LGPL-2.1.txt.
- Exported source tree only; no embedded Git repository or prebuilt binary.

Local patch (2026-10-29): `lib/init.c` makes ambient `NTLM_USER_FILE`
credential lookup a no-op when `SMB2_RUNTIME_CREDENTIALS_ONLY=1` is defined.
This prevents user/domain setters from opening or overriding credentials from
an environment-selected file. The app explicitly supplies runtime credentials.
No protocol or cryptography has been changed.

Build target `smb2` with this definition and disable Kerberos, GSSAPI,
examples and utilities. Use SMB3 negotiation and required signing in the client.
Set AUTOMOC OFF for this C library. The application retains the complete
library source and its application sources/build instructions for relinking;
distribution must also retain LGPL notices and the required relinking route.
