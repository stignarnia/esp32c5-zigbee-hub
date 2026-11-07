"""Build the "prov" NVS partition image for esp32c5-zigbee-hub.

Secrets come from environment variables (never argv, which `ps` shows):
  PROV_SSID, PROV_PSK            primary Wi-Fi (required)
  PROV_SSID2, PROV_PSK2          backup Wi-Fi (optional)
  PROV_HOSTNAME, PROV_COUNTRY    optional
  PROV_LANRESET                  optional nonce: turns tailnet-only access off once
  PROV_ADMIN_PW                  admin password (stored only as a PBKDF2 hash)

Usage: python -I mkprov.py <idf_path> <out.bin> <size>
"""

import base64
import csv
import hashlib
import os
import secrets
import subprocess
import sys
import tempfile

PBKDF2_ITERS = 10000  # verified on the C5 with hardware SHA; keep login < 1 s

# NVS limits: key <= 15 chars, string value <= 4000 bytes incl. NUL.
FIELDS = [
    ("ssid", "PROV_SSID", 32),
    ("psk", "PROV_PSK", 64),
    ("ssid2", "PROV_SSID2", 32),
    ("psk2", "PROV_PSK2", 64),
    ("hostname", "PROV_HOSTNAME", 32),
    ("country", "PROV_COUNTRY", 2),
    ("lanreset", "PROV_LANRESET", 32),
]


def admin_hash(password: str) -> str:
    salt = secrets.token_bytes(16)
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, PBKDF2_ITERS)
    b64 = lambda b: base64.b64encode(b).decode()
    return f"pbkdf2_sha256${PBKDF2_ITERS}${b64(salt)}${b64(dk)}"


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    idf_path, out_bin, size = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]

    rows = [("key", "type", "encoding", "value"), ("prov", "namespace", "", "")]
    for key, env, maxlen in FIELDS:
        value = os.environ.get(env, "")
        if not value:
            continue
        if len(value.encode()) > maxlen:
            print(f"mkprov: {env} longer than {maxlen} bytes", file=sys.stderr)
            return 1
        rows.append((key, "data", "string", value))
    if not os.environ.get("PROV_SSID"):
        print("mkprov: PROV_SSID is required", file=sys.stderr)
        return 1
    pw = os.environ.get("PROV_ADMIN_PW", "")
    if pw:
        rows.append(("admin_hash", "data", "string", admin_hash(pw)))

    gen = os.path.join(idf_path, "components", "nvs_flash", "nvs_partition_generator",
                       "nvs_partition_gen.py")
    # The CSV holds the PSK in clear: create it 0600 next to the output, delete after.
    fd, csv_path = tempfile.mkstemp(suffix=".csv", dir=os.path.dirname(out_bin))
    try:
        with os.fdopen(fd, "w", newline="") as f:
            csv.writer(f).writerows(rows)
        subprocess.run([sys.executable, "-I", gen, "generate", csv_path, out_bin, size],
                       check=True, stdout=subprocess.DEVNULL)
    finally:
        os.unlink(csv_path)
    os.chmod(out_bin, 0o600)
    return 0


if __name__ == "__main__":
    sys.exit(main())
