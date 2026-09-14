# Experiment 03 Phase 1B — Market Data Wire Protocol and Decoder

## Scope

This document specifies the synthetic wire protocol that `md_decoder.h` decodes
and the exact validation the decoder performs. It covers the byte layout, the
decoder's status semantics, the stream contract, and the three independent
validity layers a message passes through before it can reach a book.

The protocol is **synthetic and deliberately simple**. It is not FIX, not ITCH,
not SBE, and not modelled on any venue. It exists so the decoder has a
byte-level contract written down exactly and testable against literal constants
rather than against itself.

This phase is **correctness only**. Nothing here is timed, no benchmark binary
exists, and no number in this document is a duration.

## Where the decoder sits

```
raw bytes
    -> decode_one()          md_decoder.h        this document
    -> llmd::MdMessage       md_message.h        the typed vocabulary
    -> MarketDataPipeline    market_data_pipeline.h   Phase 1A, FROZEN
    -> the order book        types.h
```

Phase 1B adds the first arrow and nothing else. **No Phase-1A file was
modified**, no Phase-1A semantic changed, and no counter, state or transition
was added to the sequencer. The decoder produces the same `MdMessage` values the
Phase-1A tests already construct by hand, so the frozen state machine cannot
tell whether its input came from a fixture or from bytes.

## Byte order

**All multi-byte integers are BIG ENDIAN.** Signed fields are two's complement.

Bytes are assembled by explicit shifts and masks. The implementation uses:

- no `reinterpret_cast` to an integer pointer,
- no packed struct,
- no unaligned integer load,
- no assumption about the host's byte order.

`md_wire_protocol.h` refuses all four on purpose. The wire format is an
*external* contract, so it is written as byte offsets and assembled by hand; a
`struct` would make the wire ABI a property of the compiler, since padding,
alignment and member order are implementation details no protocol should
inherit. A packed struct would trade that for unaligned loads, which are
undefined behaviour on strict-alignment targets.

## Header — 12 bytes, always

Offsets are from the start of the message.

| Offset | Bytes | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 1 | `uint8` | `message_type` | 1, 2 or 3 |
| 1 | 1 | `uint8` | `version` | exactly 1 |
| 2 | 2 | `uint16` | `payload_length` | big endian |
| 4 | 8 | `uint64` | `sequence` | big endian |

Version is currently **1**. `payload_length` describes the payload only — it
excludes the 12-byte header.

## Message types

| ID | Name | `payload_length` | Total size | Payload |
|---|---|---|---|---|
| 1 | `SnapshotBegin` | 0 | 12 | none |
| 2 | `Level` | 17 | 29 | 17 bytes, below |
| 3 | `SnapshotEnd` | 0 | 12 | none |

The IDs are pinned as numbers, not as enumeration order: `MessageType` in
`md_wire_protocol.h` is a separate type from the internal `llmd::MdKind`, so
reordering or renaming the internal enumeration cannot silently move a wire
value. A test asserts the three IDs by number.

## Level payload — 17 bytes

Offsets are from the start of the **payload**, i.e. from message byte 12.

| Offset | Bytes | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 1 | `uint8` | `side` | 0 = Bid, 1 = Ask |
| 1 | 8 | `int64` | `price_tick` | big endian, must be `> 0` |
| 9 | 8 | `int64` | `quantity` | big endian, must be `>= 0` |

`price_tick` is a **tick index** — a position in the book's price domain — not a
currency price. That is why `<= 0` is structurally invalid rather than "a very
cheap instrument": zero and negatives are not positions at all.

`quantity == 0` is a well-formed **delete**. `quantity < 0` is not well formed.

### Worked examples

`SnapshotBegin` at sequence 1 — 12 bytes:

```
01 01 00 00 00 00 00 00 00 00 00 01
^^ ^^ ^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^
|  |  |     sequence = 1
|  |  payload_length = 0
|  version = 1
message_type = 1
```

`Level`, sequence `0x0102030405060708`, Bid, price 100, quantity 20 — 29 bytes:

```
02 01 00 11 01 02 03 04 05 06 07 08 00 00 00 00 00 00 00 00 64 00 00 00 00 00 00 00 14
^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^ ^^ ^^^^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^
header (12) sequence                side price_tick = 100        quantity = 20
```

## Decoder API

```cpp
struct DecodeOutcome {
    DecodeStatus status;
    std::size_t  consumed;
};

DecodeOutcome decode_one(std::span<const std::byte> input, MdMessage& out) noexcept;
```

### Statuses

| Status | Meaning | Terminal? |
|---|---|---|
| `Ok` | Exactly one message decoded | — |
| `NeedMoreData` | The span is a strict prefix of a message | no — read again |
| `InvalidVersion` | Header version is not 1 | yes |
| `InvalidType` | `message_type` is not 1, 2 or 3 | yes |
| `InvalidLength` | `payload_length` disagrees with `message_type` | yes |
| `InvalidSide` | `Level` side is neither 0 nor 1 | yes |
| `InvalidPrice` | `Level` `price_tick <= 0` | yes |
| `InvalidQuantity` | `Level` `quantity < 0` | yes |

`NeedMoreData` is **not an error**. It is the ordinary answer for a partial
read, and it is the only status meaning "call me again with more bytes". Every
other non-`Ok` status is a terminal property of the bytes themselves:
re-reading them will not help, and the caller must resynchronise.

Two alternatives were rejected because they look simpler:

- **A single `Invalid` status** would collapse "wait for more" into "give up" —
  the difference between a decoder that works on a socket and one that discards
  valid data whenever a read lands mid-message.
- **Folding the three field-level statuses into one `Malformed`** would make a
  venue-side bug indistinguishable from corruption in the counters an operator
  reads.

### The outcome invariant

```
consumed != 0   if and only if   status == Ok
```

On `Ok`, `consumed` is **exactly** the encoded size of the message decoded — 12
for a bracket, 29 for a `Level`. Never more, so the caller can advance without
re-deriving the length; never less, so a caller advancing by it cannot stall on
a message it already consumed.

On **every** failure — `NeedMoreData` included — `consumed` is 0 and `out` is
**untouched**. A partial message is never published. This matters more than it
looks: a caller that trusted a partially written `MdMessage` would apply half a
level update to a live book, and `NeedMoreData` is the case that happens
constantly on a real socket.

### Validation order

1. Fewer than 12 bytes → `NeedMoreData`.
2. Read `message_type`, `version`, `payload_length`, `sequence`.
3. `version != 1` → `InvalidVersion`. Checked first because it is the field that
   says how to read everything else.
4. Dispatch on `message_type`; unassigned → `InvalidType`.
5. `payload_length` checked **against the message type**, before waiting for
   bytes. A frame that declares the wrong length is malformed whatever arrives
   next, and waiting for a length that can never be satisfied would let one
   corrupt header stall the decoder indefinitely.
6. For `Level`, fewer than 29 bytes total → `NeedMoreData`.
7. `side`, then `price_tick`, then `quantity`.

## Stream semantics

`decode_one` reads **at most one** message from the **front** of a byte span. The
caller advances by `consumed` and calls again:

```
decode_one([msg1][msg2][msg3])  ->  msg1, consumed = size(msg1)
decode_one(        [msg2][msg3])  ->  msg2, consumed = size(msg2)
decode_one(                [msg3])  ->  msg3, consumed = size(msg3)
```

**Extra bytes after a complete first message are not `InvalidLength`.** This is a
byte-stream decoder, not a one-packet-per-call parser. A decoder that demanded
the span hold exactly one message could not be fed by a socket read, which is the
entire point of having one. Trailing bytes beyond a complete message are simply
left for the next call.

A prefix of the next message is likewise fine: after the last complete message in
a read, the remaining bytes return `NeedMoreData` and the caller keeps them.

## Three layers of validity

A message is checked by three independent layers, and **each rejects things the
others accept**. Keeping them apart is what lets the system distinguish "the
venue sent me a price I do not trade" from "the venue sent me garbage".

| Layer | Question | Owner | Example rejection |
|---|---|---|---|
| **Wire validity** | Are these bytes a well-formed message of this protocol? | `md_decoder.h` | `InvalidSide`, `InvalidLength`, `NeedMoreData` |
| **Sequence validity** | Is the message in order for this stream? | `market_data_pipeline.h` | `Stale`, `GapDetected`, `Rejected` |
| **Book validity** | Is it applicable to this book? | `types.h` | `OutOfRange`, `InvalidUpdate` |

### The boundary, stated explicitly

A **positive** `price_tick` outside the order book's configured tick range is
**wire-valid**. It decodes cleanly. For example `price_tick = 999999999` — far
above the default `kDefaultTickMax` of 200 000 — returns `Ok` with
`consumed == 29`.

That same message, applied in sequence, becomes `ApplyResult::OutOfRange` inside
the book: the sequence **is consumed** and the book **stays synced**. It is a
different fact with a different consequence from a decode failure.

The decoder does not enforce the tick range because it has no book and no
configuration. A decoder that did would be reaching across the seam, and the
pipeline would lose the ability to tell an untraded price from corruption.

### What the decoder deliberately does not do

- It does not check sequence order, duplicates, or gaps.
- It does not check the tick range against any book.
- It does not validate that a snapshot is best-first, uncrossed, or internally
  consistent — that is `validate_snapshot()`'s contract, one layer up.
- It does not allocate, and it holds no state between calls. All stream state
  belongs to the caller.

## Performance contract

`decode_one` is:

- **allocation-free** — it writes into a caller-provided `MdMessage` and
  constructs no string, vector or other owning object;
- **bounds-checked** — every read is behind an explicit size test;
- **exception-free on the normal path** — `noexcept`, with failures returned as
  statuses rather than thrown.

`MdMessage` stays a value type: assigned once, at the end, and only on success.

**No performance claim is made and none was measured.** The contract above is a
statement about what the code does, not about how fast it is.

## The encoder

`md_encoder.h` is a **test and fixture** encoder, not the hot path. It allocates,
it is not optimised, and it exists so a test needing one field can get a valid
frame without hard-coding 29 bytes. `append_raw` sets every header field
verbatim, which is how malformed frames are built.

The encoder is never the only witness to the layout. `md_decoder_tests.cpp`
compares against **literal byte constants written out by hand from this
document**, so an encoder and decoder sharing a wrong convention would still
fail. This is not hypothetical: reversing both the read and write helpers to
little-endian leaves every round-trip suite green — stream decoding, truncation,
malformed frames and the end-to-end integration all pass — and fails only the
literal-byte suites.

## Testing

`market-data-pipeline/tests/md_decoder_tests.cpp`, six suites:

| Suite | What it establishes |
|---|---|
| exact wire bytes | Literal expected bytes for `SnapshotBegin`, `Level` Bid, `Level` Ask, `SnapshotEnd`; signed fields pinned as literal two's-complement bytes; message IDs pinned by number |
| big-endian helpers | The read/write helpers directly, including `-1`, `0`, all-ones and a ±300 round-trip sweep |
| stream decoding | Five messages in one span, exact per-message `consumed`, trailing bytes tolerated |
| truncation | Size 0; every header truncation 1–11; **every `Level` truncation 12–28**; all `NeedMoreData`, `consumed == 0`, output unpublished |
| malformed frames | Every status: bad version, unassigned type, wrong length for all three types, bad side, non-positive price, negative quantity — each asserting `consumed == 0` **and** that `out` was not modified |
| bytes → decoder → pipeline | One focused fixture: encoded bytes through `decode_one` into `MarketDataPipeline<FlatOrderBook>` and `<MapOrderBook>`, agreeing on outcomes, state, position and top of book |

The truncation suite is exhaustive over prefix lengths rather than sampling
them, because the boundary between "header complete" and "payload complete" is
where a decoder that publishes partial state would show up — a `Level` truncated
to 20 bytes has already read a valid header declaring 17 payload bytes.

**The 1550-trace Phase-1A differential corpus is deliberately not run through the
byte decoder.** That corpus exists to exercise the sequencer; running it through
an encoder and back would test the encoder against the decoder rather than
either against its contract. One integration fixture establishes that the pieces
compose.

### Verification runs

| Check | Result |
|---|---|
| Clean Release build | 0 warnings, 0 errors under `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` |
| Full CTest | 33/33 |
| ASan | clean, both Experiment 03 suites |
| UBSan | clean, both Experiment 03 suites |
| `LLDB_SELFTEST_FAIL=1` | exit 1 (non-zero, as the guard requires) |

No TSan run: Phase 1 is single-threaded.

## Status

**Phase 1B — Binary Protocol / Decoder Correctness: COMPLETE / FROZEN.**

| File | Contents |
|---|---|
| `market-data-pipeline/include/md_wire_protocol.h` | Byte layout, message IDs, offsets, big-endian helpers |
| `market-data-pipeline/include/md_decoder.h` | `DecodeStatus`, `DecodeOutcome`, `decode_one` |
| `market-data-pipeline/include/md_encoder.h` | Test/fixture encoder |
| `market-data-pipeline/tests/md_decoder_tests.cpp` | Six suites, all green |
| `docs/MARKET_DATA_PROTOCOL.md` | This document |

Phase 1A files are unmodified. No counter, state or transition was added to the
sequencer, and no Phase-1A test was changed.

**Phase 2 — Decoder thread → SPSC → book thread — is NOT STARTED.** Nothing in
this phase measures, times, or threads anything, and this document makes no claim
about how the decoder would behave once a transport and a second thread are
placed under it.
