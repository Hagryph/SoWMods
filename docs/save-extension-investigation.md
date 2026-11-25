# Save Extension Investigation

Status: read-only reverse engineering. No live save was modified and no modified
save was loaded. Findings apply to the executable analyzed in the local Ghidra
project; full vanilla-game acceptance still needs a controlled test.

## Outer Container

`FUN_14119a2c8` reads the local/platform file and recognizes `SOM2` and `SOM3`.
`FUN_14052b1cc` writes the current `SOM3` form:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0x00 | 4 | `SOM3` magic |
| 0x04 | 4 | Plaintext CRC value used when constructing the encryption key |
| 0x08 | 4 | Encrypted payload byte count |
| 0x0c | 4 | Expected decrypted payload byte count |
| 0x10 | variable | Encrypted payload |

The writer computes the CRC with `FUN_140505400`, constructs key/IV inputs, encrypts
the serialized data, and writes `16 + encryptedBytes` bytes. The `SOM3` key inputs
also depend on a value in the save-operation object at `+0x930`; copying ciphertext
between accounts is not part of this investigation.

The outer reader allocates/reads the physical file, but passes the **header's
encrypted byte count**, not the remaining file length, to its decryption update.
It then checks decryption finalization and that the resulting plaintext length
equals the header's expected plaintext length. No physical-file-length equality
check was found in this function.

Consequently, a footer after `0x10 + encryptedBytes`, leaving the header and
encrypted bytes untouched, appears invisible to this outer decoder. This is a
static finding, not proof that every platform/cloud/UI path accepts it.

Six existing local saves were inspected. Each has a 16-byte header, an encrypted
payload length matching `fileLength - 16`, and payload alignment of 16 bytes.
For the inspected primary save: file length 1,348,400; encrypted length 1,348,384;
expected plaintext length 1,348,378. No existing trailing extension was found.

## Internal Chunks

`FUN_14052e798` writes chunks using a 16-byte header: type/id (u32), version (u32),
and payload length (u64), followed by the chunk payload.

`FUN_140ce7890` reads and dispatches a chunk. `FUN_140cdd960` resolves a registered
handler by id/version; an unknown id returns status **3**, not success (1). The
reader seeks past an unrecognized chunk using its length. However,
`FUN_1417c65fc` accumulates non-success statuses in a load-status bitmask at
`saveManager + 0x1c0`. Unknown chunks therefore cannot currently be called
vanilla-compatible merely because their bytes can be skipped.

## Persistence Limitation

The normal writer regenerates its output from registered serializers. It does not
copy arbitrary bytes from the old physical file. An external footer would therefore
be discarded on the next ordinary save unless an extension-aware writer reinserts
it. Unknown internal chunks are not automatically preserved either.

The same generated outer buffer is passed to the platform/cloud write and local
file write. The local writer also writes a numbered backup and rotates older
backups. Appending a footer only after local file close would miss other copies
and is not a sufficient save integration.

## Recommended Integration Boundary

A potential same-file extension should be attached to the **final generated save
buffer before local/platform/backup output**, not added as an unknown native chunk:

1. Leave the native 16-byte header and encrypted payload unchanged.
2. Follow them with a bounded, versioned, namespaced extension and an end marker
   containing extension length/integrity information.
3. On load, locate the extension using the native encrypted length and validate
   its bounds, version, and integrity independently.
4. On save, serialize one consistent mod-data snapshot into the same outgoing
   buffer/write operation used for the native save, including all destinations.
5. Never carry old-slot mod data into a new/replaced save. New games, load failure,
   backup restoration, and saves without an extension must reset extension state.

This would physically bind metadata to its save without a separate save-id file
or a second game-save operation. It still requires a mod to understand/preserve
the extension. A vanilla writer dropping the footer should not undo native ring
changes, but it would lose custom metadata.

Before implementation is considered safe, use an isolated, backed-up save to test
vanilla load with a footer, normal re-save, backup load, Steam Cloud behavior,
malformed/truncated extensions, and removing the mod. No such live compatibility
test was performed during this investigation.
