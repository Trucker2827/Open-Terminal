from __future__ import annotations

import base64
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
except ImportError:  # pragma: no cover - exercised by users without optional deps.
    hashes = None
    serialization = None
    padding = None


DEFAULT_KEYS_PATH = Path("/Users/haydarevich/Documents/SimplyMoney/Keysandetc/kalshi_keys.rtf")
PEM_RE = re.compile(
    r"-----BEGIN (?:RSA )?PRIVATE KEY-----.*?-----END (?:RSA )?PRIVATE KEY-----",
    re.DOTALL,
)


@dataclass(frozen=True)
class KalshiCredentials:
    key_id: str
    private_key: object

    def headers(self, method: str, path: str) -> dict[str, str]:
        timestamp = str(int(time.time() * 1000))
        signature = sign_request(self.private_key, timestamp, method, path)
        return {
            "KALSHI-ACCESS-KEY": self.key_id,
            "KALSHI-ACCESS-TIMESTAMP": timestamp,
            "KALSHI-ACCESS-SIGNATURE": signature,
            "accept": "application/json",
        }


def load_credentials(path: str | Path = DEFAULT_KEYS_PATH) -> KalshiCredentials:
    if serialization is None:
        raise RuntimeError(
            "cryptography is required for Kalshi auth. Run: python3 -m pip install -e ."
        )

    text = _read_secret_text(Path(path))
    key_id = _extract_key_id(text)
    pem = _extract_private_key_pem(text)
    private_key = serialization.load_pem_private_key(pem.encode("utf-8"), password=None)
    return KalshiCredentials(key_id=key_id, private_key=private_key)


def sign_request(private_key: object, timestamp: str, method: str, path: str) -> str:
    if hashes is None or padding is None:
        raise RuntimeError(
            "cryptography is required for Kalshi auth. Run: python3 -m pip install -e ."
        )
    message = f"{timestamp}{method.upper()}{path.split('?', 1)[0]}".encode("utf-8")
    signature = private_key.sign(
        message,
        padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH),
        hashes.SHA256(),
    )
    return base64.b64encode(signature).decode("utf-8")


def _read_secret_text(path: Path) -> str:
    if path.suffix.lower() == ".rtf":
        return subprocess.check_output(
            ["textutil", "-convert", "txt", "-stdout", str(path)],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    return path.read_text(encoding="utf-8")


def _extract_key_id(text: str) -> str:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for index, line in enumerate(lines):
        if ":" not in line:
            if "api key id" in line.lower() and index + 1 < len(lines):
                return lines[index + 1].strip()
            continue
        label, value = line.split(":", 1)
        if "api key id" in label.lower():
            candidate = value.strip()
            if candidate:
                return candidate
    raise ValueError("Could not find Kalshi API key id in keys file.")


def _extract_private_key_pem(text: str) -> str:
    match = PEM_RE.search(text)
    if not match:
        raise ValueError("Could not find RSA private key PEM block in keys file.")
    lines = [line.strip() for line in match.group(0).splitlines() if line.strip()]
    return "\n".join(lines) + "\n"
