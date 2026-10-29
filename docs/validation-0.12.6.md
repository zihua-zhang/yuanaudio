# 0.12.6 Mac overnight acceptance, 2026-10-29

The two existing libraries finished at 35,718 valid current waveform views out of 35,723 indexed tracks. An independent reader used the real Index, waveKey and loadWaveView on a read-only catalog snapshot and the existing parent peaks directory: 34,986 accurate, 732 reference, five missing. The GUI shows the same totals. References remain references. The original 2,072 historical path failures were retried successfully; old cumulative failure2170 was not a corrupt-file count.

| Library | Indexed | Valid | Accurate | Reference | Missing |
|---|---:|---:|---:|---:|---:|
| 爱看互动音乐库/音乐 | 24,904 | 24,903 | 24,429 | 474 | 1 |
| 张良源音乐库 | 10,819 | 10,815 | 10,557 | 258 | 4 |

5,872 verified compatibility mappings use NAS hardlinks. No fallback audio copies were needed in this batch. First full content verification and unchanged-version reuse remain in place. No company source audio was renamed, overwritten or deleted.

## Targeted changes

- `application/src/path_repair.cpp`: only a final file leaf may be disambiguated using unique positive indexed size and mtime; every normalized candidate must remain stable through fresh stats and a second listing. Intermediate directory, unknown, unreadable and nonunique cases remain refused. Both copy and hardlink callers supply the indexed version.
- `application/tests/path_repair_tests.cpp`: RED reproduction plus20 data cases for both routes, metadata distinctions, unknown/changed candidates, added alternatives and intermediate-directory refusal. Private suite39/39; official CTest includes this suite.
- CMake and both window title/settings sources: consistent0.12.6 version. Release documentation explains safety and remaining limits.
- Outside source, tested temporary Index utility refreshed only track24057's size0→11,185,384 and mtime0→1772510950000 ms, after full original SHA and Qt/NFD descriptor/path checks. A backup and prior exception evidence were retained. All track IDs, library IDs, relative paths, favorites, tags and libraries remain unchanged. The full catalog hash intentionally changes for this audited version update: d64e3ad8baa18a03fee2eac30e5b67ad4e3239c5627ec46ed897c4a6dd4f06ba.

Real NAS track21379 had two normalized leaf versions. Fresh stats uniquely matched fileId144271/4,899,606 bytes/indexedmtime; the other14,982,009-byte version was excluded. The selected original SHA is e194a91f2761fca8b6cb283dec953679cc9d3657343f5150f6864a6ba50636b0. Actual hardlink repair generated an accurate view. Track24057 SHA21e3f40575711bad4b595e3b069d3467136d59984b7db6ae9406ff24de51dcb2 remained stable before/after metadata update and generated an accurate view.

The installed0.12.6 business-code build displayed both waves and advanced muted seeks:21379 from0:53 to1:19/1:52,24057 from2:02 to2:20/4:16. A final settings-version-label correction was then rebuilt and installed through the complete prescribed entry. Final canonical app launches with the default formal profile, displays0.12.6 including settings and matching cache totals, and actually loads the installed patched Cocoa plugin. No full hearing review was performed.

## Remaining five source records

- 20299: readable zero-byte WAV; no audio data.
- 30893,30933,31101: three identical1005-byte MP3s, SHA5db98f9374b26c37eb11da7f29b3a89ab90f96dacea08611b566bc5d9751a2ac; ID3 header plus one MPEG Info frame, declared audio frame count0, EOF after that frame. No audio payload to recover through a path change.
- 35624: indexed MP3 absent from its actual NAS parent, including normalized-name search. A same-title FLAC exists elsewhere in the existing index; it was not substituted or used to silently repoint the track.

These records need restored/re-exported originals or explicit source selection. Their evidence is retained; no manual exception deletion or approximate-to-accurate relabeling was used.

## Verification boundaries

The final `bash 开始构建.command` passed37/37 CTest,12/12 native Cocoa, patched plugin deployment, arm64/self-contained dependency checks, ad hoc codesign, packaged decoder regression and DMG verification. Patched Cocoa UUID7E1D7A37-DA91-3487-9E00-2BA05809FB83, official Qt6.11.2 patch8148c48deb8e4345bacb797cb75c011df6f797fa. Runtime lsof confirms the final installed path. One initial native attempt had hierarchy focus and notification assertion failures (10/12); the same source rerun and final complete build each passed12/12. The original failure log is retained; its cause is not claimed proven.

GUI native SMB remains unaccepted.0.12.5 was blocked at macOS27.0.1 local-network identity authorization before SMB login.0.12.6 UI testing did not reach a new network operation, so this report does not invent a fresh denial or claim it fixed. The explicit developer CLI runs the same business code with normal user permissions and runtime-only credentials; its success does not prove GUI authorization. No root network proxy, TCC changes or global protection bypass were introduced.

Developer ID signing and notarization remain incomplete. The earlier QA cleanup preserved original SHA/size/mtime/inode, but NAS link count was still2 after test-root removal and restoration was not proven; formal hardlinks now legitimately add links. This report does not assert link count1.

All build/test logs, read-only probes, before/after metadata evidence, actual-view audit, source version, APP/DMG hashes and remaining-source details are included in the local return artifact. No company audio, live database, account credentials or private signing keys are included in source/Git upload. Backups remain local. Final source identity is the release Git commit/tag reported with the artifact; business delta is based on221fabc9f69c3a952dd13e85aa095e0c7ad581fa.
