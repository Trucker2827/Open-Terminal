# Public seed data

OpenTerminal distributes historical public market data through a separate,
read-only seed database. The application database must never be published: it
can contain credentials, accounts, portfolios, orders, fills, and local state.

## Security boundary

`scripts/security/public_seed_data.py` creates a new SQLite database from an
exact table-and-column allowlist. It never copies the source database. Export
fails if a selected source table gains an unreviewed column or if an exported
text value resembles a credential, request header, private identifier, local
filesystem path, or local username.

The initial allowlist contains only:

- `edge_prediction_raw_ticks`
- `edge_prediction_market_snapshots`
- `market_data`

Adding a table or column requires a code change and security review.

## Release workflow

Create a versioned ZIP outside the repository:

```sh
python3 scripts/security/public_seed_data.py export \
  --source "$HOME/Library/Application Support/org.openterminal.OpenTerminal/data/openmarketterminal.db" \
  --output /tmp/public-seed-v1.zip
```

Verify it independently before uploading it as a GitHub Release asset:

```sh
python3 scripts/security/public_seed_data.py verify \
  --bundle /tmp/public-seed-v1.zip
shasum -a 256 /tmp/public-seed-v1.zip
```

The release notes must publish that bundle SHA-256. Clients download only over
HTTPS and require the expected checksum:

```sh
python3 scripts/security/public_seed_data.py download \
  --url https://github.com/OWNER/REPO/releases/download/TAG/public-seed-v1.zip \
  --sha256 PUBLISHED_64_HEX_SHA256 \
  --output /tmp/public-seed-v1.zip
```

For a build that ships with data already present, stage the verified loose
files after download:

```sh
python3 scripts/security/public_seed_data.py stage \
  --bundle /tmp/public-seed-v1.zip \
  --output-dir openmarketterminal-qt/resources/public_seed
```

Both staged files are ignored by Git. CMake copies them into release output
only when both exist at configure time.

## First launch

At startup, `PublicSeedBootstrap` verifies the manifest, exact allowlist, and
database checksum before atomically installing `public-seed.sqlite` under the
active profile's data directory. It never opens, replaces, or merges into
`openmarketterminal.db`. An existing seed with different contents is preserved
and reported instead of overwritten.
