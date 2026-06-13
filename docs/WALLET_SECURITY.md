# Wallet Security Policy (MANDATORY)

This document is a **hard gate**. The crypto-wallet / on-chain transaction feature was
removed from OpenMarketTerminal. It MUST NOT be re-enabled — and no transaction-building
or signing code may be merged — unless **every MUST in this document is satisfied and
verified by review**. See also [`CRYPTO_WALLET_CONNECT.md`](./CRYPTO_WALLET_CONNECT.md)
for the connect flow and [`CLEAN_ROOM`](../OpenMarketTerminal/docs/CLEAN_ROOM.md) for the
project's no-exploitation stance.

The wallet is the single highest-consequence surface in the app: a bug or a hidden
instruction can drain a user's funds irreversibly. Treat this file the way you'd treat a
checklist for arming something dangerous.

---

## 0. Core principle

> **Do not trust the transaction-builder. Verify the transaction.**

The user — and an independent, auditable checker built into the app — MUST be able to
confirm that a transaction does *exactly* what the user intended and touches *nothing
else*, **without trusting the code that built it**. A skim, an extra recipient, an inflated
fee, or a wrong amount MUST be **structurally detectable and rejected**, even if the
builder (or a future malicious update, or the upstream the project forked from) is
compromised.

If the only thing standing between a user and a drained wallet is "the wallet extension
shows a decoded transaction and the user reads it carefully," that is **not sufficient**.

---

## 1. MUST — Non-custodial, always

- **M1.1** The app MUST NOT generate, store, import, transmit, log, or otherwise hold a
  private key, secret key, seed phrase, or mnemonic — ever, in any form, on disk, in
  memory longer than necessary, in logs, in crash dumps, or over the network.
- **M1.2** Signing MUST be performed only by the user's own wallet (browser extension such
  as Phantom/Solflare/Backpack, or a hardware wallet). The app receives **signatures and
  public addresses only**.
- **M1.3** `CryptoCredentials` / any session state MUST expose only the **public** address.
  A grep for `secretKey`, `mnemonic`, `bip39`, `keypair.*generate`, `Keypair.fromSecret`,
  `privateKey` in app source MUST return nothing in the wallet path.

## 2. MUST — Independent pre-sign verifier (the skim-killer)

Before the wallet is asked to sign **anything**, an independent verifier MUST run and MUST
**fail closed** (block, do not sign) on any uncertainty.

- **M2.1 Simulate.** The transaction MUST be simulated against the RPC
  (`simulateTransaction`) and the **net balance deltas for the user's account** read back.
- **M2.2 Plain-language disclosure.** The user MUST be shown, in human terms, the exact
  effect before approving, e.g.:
  > "You pay **1.00 SOL** (+ ~0.00005 network fee). You receive **≥ 950,000 TOKEN**.
  > **No other account is debited from your wallet.**"
- **M2.3 Intent == effect.** The verifier MUST re-derive the expected effect from the
  user's *stated intent* (input token + amount, output token, min-output / max-slippage,
  recipient = the user's own wallet) and assert the simulated/decoded transaction matches.
  Any deviation → **reject, do not sign**.
- **M2.4 Enumerate every instruction and every writable account.** If **any** instruction
  moves SOL or an SPL token to an address that is not the user or the legitimate swap
  route, the verifier MUST **block and name the address**.
- **M2.5 Independence.** The verifier MUST be **separate, small, auditable code** that does
  NOT trust metadata attached by the builder/quote server. It derives "expected" from user
  intent and checks "actual" from the decoded/simulated transaction. It is the
  most-reviewed code in the wallet feature.
- **M2.6 Fail closed.** No simulation result, an RPC error, an un-decodable instruction, an
  unknown program, a mismatch, or any exception → **deny**. Never "allow on error."

## 3. MUST — Program allowlist

- **M3.1** Only transactions whose instructions touch a **pinned allowlist of program IDs**
  (the specific DEX/aggregator, the SPL Token program, the System program for the expected
  ops, ATA program, etc.) may be signed. Any unknown program ID → block.
- **M3.2** The allowlist MUST be a hardcoded constant in the verifier, reviewed on change —
  not fetched from the network and not supplied by the builder/quote server.

## 4. MUST — User-set caps, enforced locally

- **M4.1** The user sets **max input amount, max slippage, and max priority/total fee**.
- **M4.2** These caps MUST be enforced against the **simulated result**, not against values
  the builder/quote server claims. A tx exceeding any cap → block.

## 5. MUST — No hidden recipient or skim

- **M5.1** OpenMarketTerminal takes **zero** cut. The verifier MUST assert there is **no
  transfer to any address other than the user and the swap route**.
- **M5.2** If an optional fee is ever introduced, it MUST be (a) **off by default**,
  (b) shown as its **own explicit line item** the user approves, and (c) verifiable by the
  user against a known, disclosed address. A fee MUST NEVER be injected silently or bundled
  into the swap instruction.
- **M5.3** No hardcoded recipient, "donation," "fee," or "treasury" address may appear in
  any transfer/sign path. (A token **mint** address used only as a price-lookup key is
  fine; a transfer **destination** is not.)

## 6. MUST — Lock the signing surface

- **M6.1 CSP.** The wallet HTML's Content-Security-Policy MUST restrict `connect-src` to
  `self` + the local bridge (`127.0.0.1`/`localhost`) + the RPC endpoint only. No remote
  JS, no remote CSS, no `unsafe-eval`. The page MUST NOT load any script it didn't ship.
- **M6.2 Pinned, verified web3.js.** Any vendored wallet library (e.g. `@solana/web3.js`)
  MUST be hash-pinned and the hash re-verified at build against the genuine upstream
  release. A mismatch fails the build. Do not load wallet JS from a CDN at runtime.
- **M6.3 Local bridge is loopback + token-gated.** The C++↔page bridge MUST bind
  `127.0.0.1` only and require a per-session token (as `TerminalMcpBridge` does). Never
  `0.0.0.0`.
- **M6.4 No auto-update of signing code.** The auto-updater stays removed (see
  `git log` / the removed `UpdateService`). If an update mechanism is ever added, it MUST
  be **cryptographically signature-verified** (not SHA-only against an author-controlled
  URL) before any wallet/signing code is replaced.

## 7. MUST — Transaction lifecycle hygiene

- **M7.1** Build → simulate → disclose → sign **one fresh transaction at a time**. No blind
  pre-signing, no batch-signing, no auto-signing, no "approve all."
- **M7.2** Use a **recent blockhash with short validity**; reject stale transactions so a
  swapped/replayed tx can't be slipped in.
- **M7.3** Prefer **building the transaction locally** from a public quote + pinned
  instruction layouts over fetching a pre-built unsigned tx from a remote server. If a
  remote-built tx is unavoidable, the §2 verifier is **mandatory**, not optional.

## 8. SHOULD — Strongest tiers

- **S8.1** Support hardware wallets (e.g. Ledger) for clear-signing on a separate device.
- **S8.2** Sign application releases; publish the verifier source so the community can
  confirm there is no skim.
- **S8.3** Log every signed transaction (hash + decoded effect, **no secrets**) to the
  local immutable audit trail so the user has a record.

---

## Forbidden patterns (automatic reject in review)

- Any private key / seed in app code, logs, settings, crash dumps, or network calls.
- A transfer to a hardcoded/operator/"fee"/"donation" address.
- Signing a transaction that was not independently simulated and matched to user intent.
- "Allow on error" / failing open anywhere in the sign path.
- Loading remote JS into the wallet page, or an unverified/un-pinned web3.js.
- Fetching the program allowlist, caps, or "expected effect" from the network or the
  builder.
- Re-enabling any auto-update that can replace signing code without signature verification.

---

## PR checklist (paste into any wallet PR)

```
- [ ] Non-custodial: no private key/seed generated/stored/logged/sent (M1)
- [ ] Independent pre-sign verifier present, fail-closed (M2)
- [ ] Simulated net deltas shown to user in plain language before signing (M2.1–2.2)
- [ ] Intent==effect assertion; deviation blocks signing (M2.3)
- [ ] Every instruction/writable account enumerated; unexpected recipient blocks (M2.4)
- [ ] Program allowlist hardcoded + enforced (M3)
- [ ] User caps (input/slippage/fee) enforced against simulation (M4)
- [ ] Zero operator cut; no hidden/hardcoded recipient (M5)
- [ ] CSP locked; web3.js hash-pinned + build-verified; bridge loopback+token (M6)
- [ ] One fresh tx at a time; recent blockhash; no blind/auto/batch signing (M7)
- [ ] Adversarial re-audit of the tx-builder + verifier completed (see below)
```

## Re-audit trigger

The transaction-builder is the natural place a hidden skim could be reintroduced. **Any**
change that builds, decodes, or signs a transaction — or changes the verifier, the
allowlist, the caps, the CSP, or the vendored wallet JS — REQUIRES an adversarial security
re-audit (assume the author may be hostile) **before merge**. Treat a wallet change like
arming a payment: default to "no" until proven safe.

---

## Verifier interface sketch (illustrative, not binding)

```cpp
// The independent pre-sign gate. Returns Approve ONLY when the decoded+simulated
// transaction provably matches the user's stated intent and touches nothing else.
// Everything else (mismatch, unknown program, unexpected recipient, RPC/sim error,
// cap exceeded, exception) returns Deny. Fail-closed by construction.
struct SwapIntent {
    QString  input_mint;        // what the user spends (e.g. SOL)
    quint64  max_input;         // hard cap, user-set
    QString  output_mint;       // what the user expects to receive
    quint64  min_output;        // slippage floor, user-set
    quint64  max_fee_lamports;  // total fee cap, user-set
    QString  user_owner;        // the user's own wallet (the ONLY allowed recipient)
};

enum class Verdict { Approve, Deny };

struct VerifyResult {
    Verdict verdict = Verdict::Deny;     // default DENY
    QString human_summary;               // "You pay X, receive Y, nothing else leaves."
    QString deny_reason;                 // populated on Deny
};

// MUST: simulate against RPC, decode every instruction, diff vs intent, enforce the
// program allowlist + caps, assert no transfer leaves user_owner except along the
// allowed swap route, and DENY on any error. No network-supplied "expected" values.
VerifyResult verify_before_sign(const QByteArray& unsigned_tx,
                                const SwapIntent& intent);
```

The signing call site MUST be:

```cpp
auto r = verify_before_sign(tx, intent);
if (r.verdict != Verdict::Approve) { show_blocked(r.deny_reason); return; }
show_confirmation(r.human_summary);   // user sees plain-English effect
if (!user_explicitly_approved()) return;
wallet_extension_sign_and_send(tx);   // extension shows its own decode too (defense in depth)
```
