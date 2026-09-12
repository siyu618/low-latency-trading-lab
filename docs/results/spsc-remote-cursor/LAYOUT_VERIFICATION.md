# Experiment 02 Phase 3B — runtime layout verification

Phase 3B re-verifies the Phase-3A separated cursor layout on **every**
measured object, and adds its own claim about where the cached remote
cursors live. Neither claim is asserted from the type; both are checked on
the addresses of the object that actually ran, in every repetition.

## What is claimed

1. **The separated layout still holds, in BOTH variants.** `head` and
   `tail` are on different cache lines under the host's reported line size,
   and neither shares a line with the payload array.
2. **Each cached remote cursor sits on its OWN owner's line.** The
   producer-owned `cached_tail` shares the producer's `head` line; the
   consumer-owned `cached_head` shares the consumer's `tail` line.
3. **No new cross-thread false-sharing relationship is introduced.**
   `cached_tail` is not on the `tail` line, `cached_head` is not on the
   `head` line, and the two cached values are not on the same line as each
   other. The cached copies therefore add no line that two different threads
   both write.
4. **Both variants have the same footprint.** Identical `object_size` and
   identical `payload_offset_from_object_base`, checked between the two
   independently allocated processes of every pair.

## Per-process evidence

| session | variant | bytes | capacity | line size | head line | tail line | same line? | layout | cached_tail line | cached_head line | cached placement | object size | payload offset |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | baseline | 8 | 1024 | 128 | 43909380 | 43909381 | no | PASS | 43909380 | 43909381 | PASS | 8448 | 256 |
| 1 | cached | 8 | 1024 | 128 | 39452932 | 39452933 | no | PASS | 39452932 | 39452933 | PASS | 8448 | 256 |
| 1 | baseline | 8 | 4096 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 33024 | 256 |
| 1 | cached | 8 | 4096 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 33024 | 256 |
| 1 | baseline | 8 | 65536 | 128 | 37748992 | 37748993 | no | PASS | 37748992 | 37748993 | PASS | 524544 | 256 |
| 1 | cached | 8 | 65536 | 128 | 38797568 | 38797569 | no | PASS | 38797568 | 38797569 | PASS | 524544 | 256 |
| 1 | baseline | 32 | 1024 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 33024 | 256 |
| 1 | cached | 32 | 1024 | 128 | 44040448 | 44040449 | no | PASS | 44040448 | 44040449 | PASS | 33024 | 256 |
| 1 | baseline | 32 | 4096 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 131328 | 256 |
| 1 | cached | 32 | 4096 | 128 | 39846144 | 39846145 | no | PASS | 39846144 | 39846145 | PASS | 131328 | 256 |
| 1 | baseline | 32 | 65536 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 2097408 | 256 |
| 1 | cached | 32 | 65536 | 128 | 42991872 | 42991873 | no | PASS | 42991872 | 42991873 | PASS | 2097408 | 256 |
| 1 | baseline | 64 | 1024 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 65792 | 256 |
| 1 | cached | 64 | 1024 | 128 | 37748992 | 37748993 | no | PASS | 37748992 | 37748993 | PASS | 65792 | 256 |
| 1 | baseline | 64 | 4096 | 128 | 46137600 | 46137601 | no | PASS | 46137600 | 46137601 | PASS | 262400 | 256 |
| 1 | cached | 64 | 4096 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 262400 | 256 |
| 1 | baseline | 64 | 65536 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 4194560 | 256 |
| 1 | cached | 64 | 65536 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 4194560 | 256 |
| 2 | baseline | 8 | 1024 | 128 | 44695812 | 44695813 | no | PASS | 44695812 | 44695813 | PASS | 8448 | 256 |
| 2 | cached | 8 | 1024 | 128 | 38797572 | 38797573 | no | PASS | 38797572 | 38797573 | PASS | 8448 | 256 |
| 2 | baseline | 8 | 4096 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 33024 | 256 |
| 2 | cached | 8 | 4096 | 128 | 42991872 | 42991873 | no | PASS | 42991872 | 42991873 | PASS | 33024 | 256 |
| 2 | baseline | 8 | 65536 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 524544 | 256 |
| 2 | cached | 8 | 65536 | 128 | 37748992 | 37748993 | no | PASS | 37748992 | 37748993 | PASS | 524544 | 256 |
| 2 | baseline | 32 | 1024 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 33024 | 256 |
| 2 | cached | 32 | 1024 | 128 | 37748992 | 37748993 | no | PASS | 37748992 | 37748993 | PASS | 33024 | 256 |
| 2 | baseline | 32 | 4096 | 128 | 37748992 | 37748993 | no | PASS | 37748992 | 37748993 | PASS | 131328 | 256 |
| 2 | cached | 32 | 4096 | 128 | 44040448 | 44040449 | no | PASS | 44040448 | 44040449 | PASS | 131328 | 256 |
| 2 | baseline | 32 | 65536 | 128 | 38797568 | 38797569 | no | PASS | 38797568 | 38797569 | PASS | 2097408 | 256 |
| 2 | cached | 32 | 65536 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 2097408 | 256 |
| 2 | baseline | 64 | 1024 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 65792 | 256 |
| 2 | cached | 64 | 1024 | 128 | 39846144 | 39846145 | no | PASS | 39846144 | 39846145 | PASS | 65792 | 256 |
| 2 | baseline | 64 | 4096 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 262400 | 256 |
| 2 | cached | 64 | 4096 | 128 | 37748992 | 37748993 | no | PASS | 37748992 | 37748993 | PASS | 262400 | 256 |
| 2 | baseline | 64 | 65536 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 4194560 | 256 |
| 2 | cached | 64 | 65536 | 128 | 38797568 | 38797569 | no | PASS | 38797568 | 38797569 | PASS | 4194560 | 256 |
| 3 | baseline | 8 | 1024 | 128 | 45744388 | 45744389 | no | PASS | 45744388 | 45744389 | PASS | 8448 | 256 |
| 3 | cached | 8 | 1024 | 128 | 41550084 | 41550085 | no | PASS | 41550084 | 41550085 | PASS | 8448 | 256 |
| 3 | baseline | 8 | 4096 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 33024 | 256 |
| 3 | cached | 8 | 4096 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 33024 | 256 |
| 3 | baseline | 8 | 65536 | 128 | 38797568 | 38797569 | no | PASS | 38797568 | 38797569 | PASS | 524544 | 256 |
| 3 | cached | 8 | 65536 | 128 | 39846144 | 39846145 | no | PASS | 39846144 | 39846145 | PASS | 524544 | 256 |
| 3 | baseline | 32 | 1024 | 128 | 39846144 | 39846145 | no | PASS | 39846144 | 39846145 | PASS | 33024 | 256 |
| 3 | cached | 32 | 1024 | 128 | 44040448 | 44040449 | no | PASS | 44040448 | 44040449 | PASS | 33024 | 256 |
| 3 | baseline | 32 | 4096 | 128 | 34603264 | 34603265 | no | PASS | 34603264 | 34603265 | PASS | 131328 | 256 |
| 3 | cached | 32 | 4096 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 131328 | 256 |
| 3 | baseline | 32 | 65536 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 2097408 | 256 |
| 3 | cached | 32 | 65536 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 2097408 | 256 |
| 3 | baseline | 64 | 1024 | 128 | 44040448 | 44040449 | no | PASS | 44040448 | 44040449 | PASS | 65792 | 256 |
| 3 | cached | 64 | 1024 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 65792 | 256 |
| 3 | baseline | 64 | 4096 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 262400 | 256 |
| 3 | cached | 64 | 4096 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 262400 | 256 |
| 3 | baseline | 64 | 65536 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 4194560 | 256 |
| 3 | cached | 64 | 65536 | 128 | 38797568 | 38797569 | no | PASS | 38797568 | 38797569 | PASS | 4194560 | 256 |
| 4 | baseline | 8 | 1024 | 128 | 43188484 | 43188485 | no | PASS | 43188484 | 43188485 | PASS | 8448 | 256 |
| 4 | cached | 8 | 1024 | 128 | 40829200 | 40829201 | no | PASS | 40829200 | 40829201 | PASS | 8448 | 256 |
| 4 | baseline | 8 | 4096 | 128 | 38797568 | 38797569 | no | PASS | 38797568 | 38797569 | PASS | 33024 | 256 |
| 4 | cached | 8 | 4096 | 128 | 39846144 | 39846145 | no | PASS | 39846144 | 39846145 | PASS | 33024 | 256 |
| 4 | baseline | 8 | 65536 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 524544 | 256 |
| 4 | cached | 8 | 65536 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 524544 | 256 |
| 4 | baseline | 32 | 1024 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 33024 | 256 |
| 4 | cached | 32 | 1024 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 33024 | 256 |
| 4 | baseline | 32 | 4096 | 128 | 39846144 | 39846145 | no | PASS | 39846144 | 39846145 | PASS | 131328 | 256 |
| 4 | cached | 32 | 4096 | 128 | 35651840 | 35651841 | no | PASS | 35651840 | 35651841 | PASS | 131328 | 256 |
| 4 | baseline | 32 | 65536 | 128 | 40894720 | 40894721 | no | PASS | 40894720 | 40894721 | PASS | 2097408 | 256 |
| 4 | cached | 32 | 65536 | 128 | 35651840 | 35651841 | no | PASS | 35651840 | 35651841 | PASS | 2097408 | 256 |
| 4 | baseline | 64 | 1024 | 128 | 35651840 | 35651841 | no | PASS | 35651840 | 35651841 | PASS | 65792 | 256 |
| 4 | cached | 64 | 1024 | 128 | 38797568 | 38797569 | no | PASS | 38797568 | 38797569 | PASS | 65792 | 256 |
| 4 | baseline | 64 | 4096 | 128 | 41943296 | 41943297 | no | PASS | 41943296 | 41943297 | PASS | 262400 | 256 |
| 4 | cached | 64 | 4096 | 128 | 42991872 | 42991873 | no | PASS | 42991872 | 42991873 | PASS | 262400 | 256 |
| 4 | baseline | 64 | 65536 | 128 | 44040448 | 44040449 | no | PASS | 44040448 | 44040449 | PASS | 4194560 | 256 |
| 4 | cached | 64 | 65536 | 128 | 36700416 | 36700417 | no | PASS | 36700416 | 36700417 | PASS | 4194560 | 256 |

## Footprint equivalence, per (bytes, capacity)

The benchmark independently instantiates BOTH variants for the cell before
timing and refuses to run if their `object_size` or
`payload_offset_from_object_base` disagree. These columns are the recorded
result of that gate, read back from the two separately allocated processes.

| bytes | capacity | baseline object size | cached object size | baseline payload offset | cached payload offset | agree |
|---|---|---|---|---|---|---|
| 8 | 1024 | 8448 | 8448 | 256 | 256 | YES |
| 8 | 4096 | 33024 | 33024 | 256 | 256 | YES |
| 8 | 65536 | 524544 | 524544 | 256 | 256 | YES |
| 32 | 1024 | 33024 | 33024 | 256 | 256 | YES |
| 32 | 4096 | 131328 | 131328 | 256 | 256 | YES |
| 32 | 65536 | 2097408 | 2097408 | 256 | 256 | YES |
| 64 | 1024 | 65792 | 65792 | 256 | 256 | YES |
| 64 | 4096 | 262400 | 262400 | 256 | 256 | YES |
| 64 | 65536 | 4194560 | 4194560 | 256 | 256 | YES |
