# Trade Provenance Context Index

**Status:** Proposed — architecture and safety contract only; no runtime implementation.
**Date:** 2026-08-11
**Initial scope:** Kalshi paper/live trade provenance.

## Decision

Open Terminal should adopt the useful part of the “context graph” idea: preserve
the typed relationships between evidence, models, gates, decisions, orders,
fills, and outcomes so the complete reason for a trade remains queryable.

It should **not** introduce a graph as a new source of trading authority. The
graph is a disposable, read-only index reconstructed from records that already
own the truth. If the index is absent, corrupt, stale, locked, or disabled,
admission, risk, sizing, submission, and cancellation outcomes must remain
equivalent to behaviour without it.

The first deliverable is therefore a **trade provenance context index**, not a
general knowledge graph and not an agent-memory system.

## Problem

Open Terminal already records the pieces of a decision, but they live in
different stores:

- connector observations carry source and freshness provenance;
- immutable decision envelopes carry accepted/rejected signals and a content
  hash;
- `edge_decision_journal` records decisions and later outcomes;
- the Kalshi evidence directory carries calibrator reports, the sealed gate,
  the append-only bot ledger, quarantine records, and settlement feeds;
- `trade_audit` records prepare/submit allow-or-deny decisions;
- `kalshi_live_orders` and `kalshi_live_fills` record venue execution;
- the local JSONL lake mirrors selected decisions, model outputs, snapshots,
  and broker events.

Today a reviewer reconstructs the chain by knowing file names, schemas, joins,
and timestamp conventions. That makes it too easy to miss a cross-family trust
leak, use the wrong report, or accept a plausible narrative unsupported by the
actual execution record.

The index must make both distinct admission paths directly inspectable:

```text
source observation
  -> family-scoped model report
  -> model-family trust verdict
  -> paper decision / paper lifecycle / paper settlement
  -> sealed promotion-gate family verdict
  -> live decision / venue order / fill / settlement / realised P&L
```

The model-family trust verdict decides whether the bot may collect a paper
sample for that family. The sealed promotion gate judges the accumulated paper
record and decides whether a live decision may be admitted. They are different
authorities over different transitions and must never be represented as one
generic “PASS.”

## Existing authorities

The index must preserve, not blur, authority boundaries.

| Record | Authority | Notes |
|---|---|---|
| Connector observation | Connector result plus `Provenance` | Source URL, fetch time, cache and staleness metadata. |
| Generic model decision | `decision_envelopes` | Immutable and content-hashed by database triggers and `DecisionOrchestrator`. |
| Edge decision/outcome | `edge_decision_journal` | Decision row whose outcome fields may be updated later; every observed version must be distinguishable. |
| Kalshi model evidence | Calibrator report artifact | Authoritative only for the exact family declared in its `by_family` entry. A pooled block is diagnostic only. |
| Promotion criteria | `kalshi-bot-gate-params.json` | Sealed preregistration record. |
| Published promotion result | `kalshi-bot-gate.json` | One scorer, many readers. Admission reads this artifact; the index must not re-score it. |
| Kalshi paper record | All generations of `kalshi-bot-decisions.jsonl` | Complete append-only decision, order-book, and paper-settlement record. |
| Evidence exclusion | `kalshi-bot-evidence-quarantine.jsonl` | Append-only reason a position may not count toward a family gate. |
| Prepare/submit decision | `trade_audit` | Insert-only service record of intent, risk snapshot, and allow/deny result. |
| Live execution | `kalshi_live_orders`, `kalshi_live_fills` | Venue lifecycle and fills. |
| Data lake datasets | Local JSONL lake | A replica/analysis surface, never higher authority than the source record it mirrors. |

When two sources disagree, the index records the disagreement. It must not
silently select the more convenient value.

## Safety constitution

These requirements govern every implementation phase.

1. **No authority.** No graph query, node, edge, score, cluster, or agent output
   may arm trading, change risk, size a position, create/cancel an order, or
   override a deterministic gate.
2. **Separate and disposable.** The index lives in a separate application-level
   SQLite file, provisionally `AppPaths::data()/context-index.db`, not
   `openmarketterminal.db`, `workspace.db`, or the Kalshi evidence directory.
   Deleting it is a supported recovery procedure.
3. **Asynchronous and non-blocking.** Indexing happens after authoritative
   writes. Index failure or lag cannot change the result returned by a trading
   write and cannot hold a lock required by the execution path.
4. **Deterministic relationships only.** Phase 1 edges come from stable IDs,
   explicit fields, sealed hashes, or exact artifact references. No LLM,
   similarity score, ticker guess, or temporal proximity may create an edge.
5. **Exact family scope.** Kalshi family identity uses the same canonical
   implementation as the gate (`KalshiBotGate::family_of`), never a copied
   parser or a display taxonomy.
6. **Unknown stays unknown.** Missing evidence creates an unresolved record or
   invariant failure, never an inferred relationship.
7. **Versions are never conflated.** When an authoritative source retains its
   versions, the index connects them with `supersedes`. When a source overwrites
   history, an observed older version is only a disposable cache entry and can
   never be presented as durable authority. Missing source history remains an
   explicit provenance gap.
8. **Scope cannot cross accidentally.** Account, venue, paper/live mode, and
   source scope are part of identity and are checked on every executable
   lineage edge. A source is explicitly `profile:<id>`, `legacy-shared`, or
   `unknown`; shared evidence is never attributed to the active profile merely
   because that profile happened to run the materializer.
9. **Allowlisted payloads only.** Credentials, authentication headers, private
   keys, cookies, raw secret-bearing responses, and unbounded `raw_json` are
   never copied into the index.
10. **Observable staleness.** Readers see the last complete batch, source
    cursors, unresolved count, invariant failures, and indexing lag. Stale or
    incomplete indexing is never presented as current.

## Initial ontology

### Node kinds

| Kind | Canonical identity |
|---|---|
| `source_observation` | provider + source identity + observation time + content hash |
| `contract_family` | venue + exact series family, for example `kalshi:KXGOLDH` |
| `market_contract` | venue + market/contract identifier |
| `model_report` | report kind + generated time + content hash |
| `model_trust_verdict` | model-report version + exact family |
| `sealed_gate` | parameters content hash |
| `gate_verdict` | published verdict content hash + exact family |
| `decision` | authoritative decision ID + content hash/version |
| `order` | venue + client order ID, falling back to the authoritative draft ID before venue acceptance |
| `fill` | venue + fill key/trade ID |
| `settlement` | source settlement identity or paper position identity + resolution event |
| `quarantine` | quarantine event identity + content hash |

The graph stores compact, allowlisted metadata for inspection. Full payloads
remain in their authoritative table or artifact and are reached through an
`authority_ref` plus a content hash.

The application-level location is deliberate. `openmarketterminal.db` and the
Kalshi evidence directory are legacy shared stores, while the local data lake
is profile-scoped. A profile-local index would duplicate the shared record and,
worse, imply provenance the source does not contain. One application-level
index can preserve the real scope of each source without changing any source.

### Provenance stamps required before executable lineage

The current sources do not yet support every exact edge this design requires:

- bot decision rows copy the calibrator's track record and `generated_at_ms`,
  but not a hash of the exact report object passed to `decide()`;
- live decision rows copy `gate_ts_ms`, but not a hash of the exact published
  gate object that `permit()` read or the params seal it evaluated;
- some audit/lake representations derive an identifier after the database
  write rather than carrying the authoritative row/draft identity explicitly.

Calibrator and gate reports are atomically overwritten. A timestamp match is
not proof of object identity, and temporal proximity is not an acceptable edge.
Before the index emits `derived_from`, `paper_admitted_by`, or
`live_authorized_by` for these paths, the authoritative decision/audit row must
carry bounded, non-authorizing stamps:

- canonical report content hash and report schema/version;
- exact family used for trust evaluation;
- published gate content hash, gate timestamp, and params seal;
- stable decision, draft, audit-event, client-order, and position identifiers
  needed by the applicable transition.

Adding these fields changes auditability, not authority: admission and execution
must continue reading and enforcing the same existing objects. Historical rows
that predate a stamp remain indexable as nodes but their exact edge is
`context_unresolved`; the materializer must not backfill it from timestamps.

All stamps use one shared canonicalization implementation and carry its
algorithm/schema version. C++ and Python producers may not independently invent
JSON ordering or number formatting rules and then call the results the same
hash.

### Edge kinds

| Relation | Required meaning |
|---|---|
| `belongs_to_family` | Contract, trust/gate verdict, decision, or position has an exact family. |
| `derived_from` | A model/verdict/decision explicitly cites its input artifact or snapshot. |
| `paper_admitted_by` | A paper decision was admitted by model trust for the same exact family. |
| `evaluated_by` | A paper settlement record was evaluated by a particular sealed promotion-gate version. |
| `live_authorized_by` | A live decision was admitted by the published promotion verdict for the same exact family. |
| `submitted_as` | A decision/draft produced an order. |
| `filled_as` | An order produced a venue fill. |
| `settled_as` | A paper position or live fill reached a settlement/outcome. |
| `excluded_by` | A decision/settlement was removed from promotion evidence by a quarantine event. |
| `supersedes` | A later content version of the same authoritative record replaces an earlier view. |

Each edge carries its producer version, evidence reference, observation time,
effective time when distinct, batch ID, and all applicable scope fields.

## Storage model

The implementation should use ordinary SQLite, not a graph server. The graph
is small enough for indexed adjacency queries and gains more from deployability
and rebuildability than from a separate database product.

The provisional schema contains:

- `context_batches`: capture interval, producer version, source snapshot
  metadata, committed timestamp, and canonical digest;
- `context_nodes`: typed identity, authority reference, source/content hash,
  timestamps, scope, schema version, and bounded metadata JSON;
- `context_edges`: typed from/to relationship, evidence reference, scope,
  timestamps, and producer version;
- `context_source_cursors`: per-source replay cursor and source identity;
- `context_unresolved`: a durable reason an expected relationship could not be
  established;
- `context_invariant_failures`: stable failure code, evidence references,
  first/last observed batch, and resolution status.

Foreign keys are enabled. Nodes and edges are append-only within a version;
current-state SQL views choose the latest non-superseded version. Only one
materializer writes the file. Readers use independent read-only connections.

Content hashes establish object identity and detect change. They are not a
signature and do not prove that the producer or underlying claim was correct.

## Process and locking boundary

The materializer runs as a separate headless `openterminalcli` worker, not in
the Kalshi bot tick, trading daemon request handler, or GUI event thread. It can
be started/stopped independently and is not auto-enabled until shadow soak.
This separation is the enforceable form of “index failure cannot block
trading,” not merely a promise at a call site.

It opens the application database read-only, runs no migrations, and takes only
short deferred read snapshots. Source rows are copied into bounded in-memory
batches; the source read transaction is closed before writing
`context-index.db`. The index uses WAL for one writer plus read-only query
clients.

SQLite rows and filesystem artifacts do not share a global transaction. A
batch therefore records `capture_started_at` and `capture_finished_at` and may
connect records only through exact IDs/hashes carried by the records. It must
not claim all sources represent one instant merely because they were indexed
in one batch.

## Materialization protocol

The useful K3 idea is retained as a deterministic two-pass batch:

1. Capture source identities and bounds: database watermark/row hashes,
   artifact size and hash, JSONL generation list and byte offsets.
2. Parse and canonicalize every node in the bounded source snapshot.
3. Insert nodes for the complete batch.
4. Resolve explicit relationships only after the full node set exists.
5. Write unresolved relationships and invariant results.
6. Compute a canonical digest and commit the whole batch in one transaction.
7. Publish the batch as current only after commit.

For JSONL input, a partial final line is not indexed and is retried next batch.
Kalshi bot ledger generations are read through the existing canonical
generation-order helper. For atomically replaced JSON reports, the materializer
checks file identity before and after reading and retries when it changed.

The materializer must be replay-based, not EventBus-dependent. Runtime events
may wake it, but a deleted index rebuilt from disk must produce the same current
authoritative projection without replaying in-process events. Historical
versions no longer retained by any source are outside that equivalence claim.

## Required invariants

The first release is useful only if it detects concrete safety failures.

1. **Family isolation:** every `paper_admitted_by` and `live_authorized_by` path
   has the same exact family at trust/gate verdict, decision, contract, and
   settlement.
2. **No pooled authority:** a pooled diagnostic report/verdict has zero outgoing
   `paper_admitted_by` or `live_authorized_by` edges.
3. **Two gates stay distinct:** a paper decision cites the exact model-family
   trust object it read; a live decision cites the published promotion-gate
   artifact it read. Neither transition may substitute the other verdict or a
   locally re-derived score.
4. **Sealed criteria:** the referenced gate parameters verify their seal and
   the verdict names that same parameter version.
5. **Complete execution chain:** every fill has one resolvable order; every
   live order has its draft/audit lineage when those fields are required.
6. **Outcome lineage:** every settlement counted by a gate traces to that
   family's decision record unless explicitly connected to a quarantine event.
7. **Mode/scope/account isolation:** executable paths cannot cross paper/live,
   profile, legacy-shared, or account boundaries. A link involving an unknown
   scope is unresolved unless the authoritative record supplies the missing
   attribution.
8. **No future evidence:** source/model/gate timestamps cannot postdate the
   decision that consumed them.
9. **No silent disappearance while observed:** a source record present in a
   prior complete batch and absent in a later one is reported. After deleting
   the disposable index, only history retained by authoritative or append-only
   mirror sources can be rebuilt; the index never claims otherwise.
10. **Replica humility:** a data-lake copy cannot override a conflicting
    authoritative row or artifact.

The historical pooled-commodities defect is a primary regression fixture: a
single model trust flag spanning Gold, Silver, and WTI must produce an invariant
failure, not three valid paper-admission paths. The producer/model split and
family-specific trust path landed before this design; the index verifies that
the property remains true rather than claiming to introduce it.

## Initial read-only queries

The first query surface is fixed and typed, not arbitrary agent-generated SQL:

- `provenance status` — last complete batch, lag, cursors, unresolved and
  invariant-failure counts;
- `provenance explain-decision <id>` — evidence-to-decision path with hashes and
  timestamps;
- `provenance explain-order <id>` — decision through order/fill/settlement;
- `provenance audit-family-isolation` — cross-family and pooled-authority
  violations;
- `provenance unresolved` — relationships the materializer refused to guess;
- `provenance impact <authority-ref>` — downstream decisions/executions that
  explicitly depend on one artifact version;
- `provenance verify-rebuild` — rebuild the current authoritative projection
  into a temporary database and compare canonical digests. Observed-only cache
  history from overwritten sources is excluded from this equivalence claim.

An eventual GUI “Explain this trade” view may render these fixed query results.
It must display authority references and missing links, not a persuasive prose
story that papers over them.

## Failure behaviour and rollback

- The indexer has an independent enable switch that defaults off until shadow
  validation completes. It is not in the constitutional `cli.*` trading
  namespace because it grants no authority.
- A failure logs without modifying the source record and, when the index remains
  writable, updates index health.
- Lock contention, disk-full, parse error, or invariant failure leaves the
  prior complete batch readable.
- Disabling the indexer requires no restart of the trading daemon.
- Recovery is: stop the indexer, move `context-index.db` aside, rebuild into a
  new file, verify the digest/invariants, then atomically publish the new file.
- Removing the feature means deleting its reader/indexer integration and the
  disposable index. No authoritative migration must be reversed.

## Verification contract

Each implementation PR needs tests that survive Release/NDEBUG and execute by
exact name in CI.

Required coverage:

1. ontology and canonical-ID validation;
2. deterministic two-pass materialization over fixed fixtures;
3. idempotent repeated ingestion;
4. rebuild of the same bounded authoritative source snapshot into an empty
   database yields the same current-projection canonical digest;
5. crash/SQL failure during pass two publishes no partial batch;
6. truncated JSONL final line is deferred, not fabricated or lost;
7. rotated Kalshi ledger generations retain canonical chronological order;
8. source row mutation creates `supersedes` history;
9. cross-family authorization, pooled authority, cross-mode, and cross-profile
   edges are refused and reported;
10. missing relationship becomes unresolved, not a guessed edge;
11. graph database unavailable/locked leaves admission, risk, order submission,
    and cancellation results unchanged;
12. payload allowlist excludes secrets and oversized raw responses;
13. index lag and stale batch state are visible;
14. a neuter for every safety invariant demonstrates that its test fails for
    the intended reason.

Shadow acceptance requires the index-derived trace for a representative paper
record to match direct authoritative queries, with every mismatch classified,
before the query UI is enabled.

## Delivery sequence

One concern per PR:

1. **This specification.** Architecture, ontology, authority matrix, safety
   constitution, acceptance tests. No production code.
2. **Index foundation.** Separate database wrapper/schema, bounded metadata,
   batch transaction, status, and rebuild command over fixtures only.
3. **Authoritative provenance stamps.** Add content hashes and stable IDs to
   newly written decision/audit rows without changing admission, risk, or
   execution semantics. Historical unstamped rows remain unresolved.
4. **Kalshi evidence lineage.** Family, calibrator report, gate params/verdict,
   quarantine, and paper decision ingestion in shadow mode.
5. **Execution lineage.** Draft/audit, live order, fill, paper/live settlement,
   and outcome links.
6. **Invariant auditor and fixed CLI queries.** No GUI and no generic query
   language.
7. **Operational soak.** Run shadow indexing, measure lag/locks/unresolved
   records, and compare against direct queries.
8. **Explain-trade UI.** Read-only presentation after shadow acceptance.

The per-family calibrator/model split and family-specific decision path are
already present on the design's base. The index depends on and audits that
contract; a missing `by_family` entry is unresolved/failing and never falls back
to a pooled block. This project must not be represented as the change that fixed
the earlier pooling defect.

## Explicit non-goals

- no 300-agent swarm;
- no Neo4j, hosted graph service, or new deployment dependency;
- no graph-based trade authorization or risk override;
- no automatic causal/correlation edges;
- no fuzzy entity resolution in the executable lineage;
- no company/supplier/regulatory research graph in Phase 1;
- no engineering issue/PR/test/agent-memory graph in Phase 1;
- no generic graph visualizer as the first product surface;
- no rewriting or consolidation of the existing authoritative stores;
- no preregistration of commodity families as a side effect of this work.

## Later extension boundary

After trade provenance has rebuilt deterministically and survived operational
soak, a separate advisory graph may connect research claims, documents,
findings, tests, issues, and pull requests. Such edges require citations and may
represent conflicting claims side by side. They remain outside the trading
authority path and outside this design's first implementation sequence.
