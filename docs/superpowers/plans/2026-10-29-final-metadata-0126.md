# 0.12.6 final targeted repair

Goal: finish the already-authorized overnight catalog work without altering original audio or clearing valid caches.

Evidence: 0.12.5 completed 35,716/35,723 current views (34,984 accurate, 732 reference), seven unresolved records. Native read-only probe proves two normalized Korean leaf names have different sizes and mtimes; exactly one matches the indexed version. Another row has stale zero size/mtime while the exact original is now a stable 11,185,384-byte MP3. Four files have no audio payload and one indexed file is missing on the NAS.

- [x] RED/GREEN final-leaf disambiguation using a unique positive indexed size and mtime, checking every candidate through fresh stat. Keep directory ambiguity, insufficient metadata and all nonunique/unstable cases rejected.
- [x] Review the small source delta independently; preserve old fallback, original identity and no-overwrite checks.
- [x] Back up the completed metadata/ledger; verify and refresh only track 24057 through the Index API, preserving id, library, relative path, favorite and tags. Record the intentional metadata delta separately from personal data.
- [x] Bump 0.12.6, run prescribed Mac build and patched Cocoa checks, relink explicit test driver, then run a local catalog/cache accounting pass and the remaining targeted repair. This is not a filesystem rescan.
- [x] Audit current views with the actual core reader, per-library counts, pending causes and reference flags. Install and inspect the final APP, retain the GUI local-network limitation unless a new real connection is observed.
- Final publication is recorded separately in the local return artifact: push private source/tag, update hashes, package evidence and stop this one-night heartbeat after acceptance. Source metadata is finalized before creating its own archive.

Review focus: no metadata-based guess when both candidates match; unreadable or changed alternatives must not be ignored. Source bytes/names remain untouched; size/mtime changes apply only to audited index row 24057. No new NAS test root or privileged network helper. Do not turn reference views into accurate views merely to increase success counts.

Evidence so far: private path-repair suite39/39 and independent read-only review; actual target21379 repaired through hardlink fileId144271 with SHA e194a91f2761fca8b6cb283dec953679cc9d3657343f5150f6864a6ba50636b0. Track24057 full SHA21e3f40575711bad4b595e3b069d3467136d59984b7db6ae9406ff24de51dcb2, exact Qt/NFD path binding retained. IndexAPI updated only size0→11185384 and mtime0→1772510950000; all catalog identities/favorites/tags/libraries unchanged. Final actual core reader35718 valid (34986 accurate+732reference),5missing. Two installed GUI tracks displayed waves and advanced muted seek; full hearing not performed.
