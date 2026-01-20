# Easystroke Stability Plan

## Rules for Codex

- Assume docs/overview.md is correct and complete
- Never re-derive architecture unless explicitly asked
- Prefer null guards, asserts, logs over refactors
- If unsure, stop and ask instead of guessing

This document defines a **stability-first execution plan** for the Easystroke fork.

It is derived from an accepted, completed analysis (docs/overview.md).
That analysis is considered **authoritative and frozen**.

## Rules (Binding)

- docs/overview.md must NOT be re-read or re-analyzed unless explicitly requested.
- Items in this document are executed **one at a time**.
- Priority order must be respected (P0 → P3).
- Changes must preserve:
  - existing behavior
  - stability
  - performance
- No refactors unless explicitly stated.
- No speculative redesigns.
- If uncertainty arises, stop and ask instead of guessing.

This file is the **single source of truth** for progress tracking.

---

## Plan Evolution Rules

- File paths listed in items are **descriptive**, not binding.
- Structural changes (P2.x) may legitimately change file boundaries.
- When a structural item completes:
    - Update affected downstream items’ file references
    - WITHOUT changing their intent, scope, or priority
- No item may change *what* it is trying to achieve as a result of refactors.

This avoids stale references while preserving plan integrity.

---


## P0 — Crash Prevention (Highest Priority)

### P0.1 Guard RawMotion `current_dev` dereference
- **Files:** `handler.cc`
- **Risk:** XI_RawMotion may arrive before any ButtonPress, leaving `current_dev` null.
- **Symptom:** Null dereference at `current_dev->proximity_axis`.
- **Goal:** Add defensive null checks only.
- **Constraints:** No behavior change, no refactor.
- **Status:** DONE

### P0.2 Harden `StrokeHandler` constructor device assumptions
- **Files:** `handler.cc`
- **Risk:** `StrokeHandler` reads `xstate->current_dev->name` assuming it is valid.
- **Symptom:** Null dereference or stale device pointer.
- **Goal:** Validate device presence; safely bail if invalid.
- **Constraints:** No behavior change, no refactor.
- **Status:** DONE

### P0.3 Timeout / callback lifecycle safety
- **Files:** `handler.cc`
- **Risk:** `Glib::signal_timeout` callbacks firing after handler teardown.
- **Symptom:** Use-after-free or undefined behavior.
- **Goal:** Ensure all timeout callbacks are disconnected or safely invalidated.
- **Constraints:** Minimal change only.
- **Status:** DONE

---

## P1 — Stability Hardening (Low Risk, No Behavior Change)

### P1.1 Make fake button ID remapping explicit
- **Files:** `handler.cc`, `actions.cc`, `prefdb.cc`
- **Risk:** Synthetic button IDs (+100 / −100) are implicit and fragile.
- **Goal:** Centralize remapping logic and add guardrails.
- **Status:** DONE

### P1.2 Tighten grab state transitions
- **Files:** `handler.cc`, `grabber.cc`
- **Risk:** `Grabber::current` may drift from `Handler::grab_mode()`.
- **Goal:** Add lightweight assertions or logging for mismatches.
- **Status:** DONE

### P1.3 Guard synthetic event loopback
- **Files:** `handler.cc`, `main.cc`
- **Risk:** XTest-generated events may be re-captured unintentionally.
- **Goal:** Add low-cost detection/logging guard.
- **Status:** DONE

### P1.4 Make RawMotion ordering assumptions explicit
- **Files:** `handler.cc`
- **Risk:** RawMotion processed without prior ButtonPress context.
- **Goal:** Enforce safe ordering assumptions or skip invalid paths.
- **Status:** DONE

### P1.5 Add action DB unregistered-class diagnostic
- **Files:** `actiondb.cc`
- **Risk:** Action database load failures are opaque.
- **Goal:** Emit targeted diagnostics when the archive reports an unregistered class.
- **Constraints:** Logging only, no behavior change.
- **Status:** DONE

### P1.6 Refine action DB diagnostic with header token offsets
- **Files:** `actiondb.cc`
- **Risk:** Unregistered-class diagnostics may list user labels instead of class names.
- **Goal:** Report unknown tokens in the archive header region with offsets.
- **Constraints:** Logging only, no behavior change.
- **Status:** DONE

### P1.7 Add numeric-prefix token detection to action DB diagnostic
- **Files:** `actiondb.cc`
- **Risk:** Boost may serialize mangled or numeric-prefixed class names.
- **Goal:** Include alpha-containing tokens with leading digits in diagnostics.
- **Constraints:** Logging only, no behavior change.
- **Status:** DONE

### P1.8 Log Boost class-name table during action DB load
- **Files:** `actiondb.cc`
- **Risk:** Missing class names are not exposed by archive exceptions.
- **Goal:** Capture class names seen during text_iarchive load for debugging.
- **Constraints:** Logging only, no behavior change.
- **Status:** DONE

### P1.9 Add Action DB TOML migration path
- **Files:** `actiondb.cc`, `actiondb.h`, `main.cc` (+ new `actiondb_toml.cc`, `actiondb_toml.h`)
- **Risk:** Boost serialization version mismatches can prevent action DB load.
- **Goal:** Provide a TOML export/import path to migrate action DB across Boost versions.
- **Constraints:** Minimal migration-only behavior change.
- **Status:** DONE

---

## P2 — Structural Risk Reduction (No Behavior Change)

### P2.1 Separate window-tracking from Grabber
- **Files:** `grabber.cc`
- **Goal:** Isolate WM/window hierarchy logic into a dedicated module.
- **Constraints:** Pure structure change, no logic change.
- **Status:** DONE

### P2.2 Reduce `handler.cc` complexity
- Structural checkpoint: YES (file boundaries may change; intent is fixed)
- **Files:** `handler.cc` (+ new files)
- **Goal:** Move action replay handlers into focused compilation units.
- **Constraints:** No logic changes.
- **Status:** DONE

### P2.3 Make handler deferral rules explicit
- **Files:** `handler.cc`, `handler.h`
- **Goal:** Document and lightly formalize handler stack ordering invariants.
- **Status:** DONE

### P2.4 Stabilize select-window flow
- **Files:** `handler.cc`
- **Goal:** Improve robustness of ping/pong timeout handling.
- **Constraints:** Minimal tuning or configurability only.
- **Status:** DONE

---

## P3 — Regression Safety & Documentation

### P3.1 Add focused regression harness
- **Files:** new test(s) targeting `XState::handle_xi2_event`
- **Goal:** Validate null-device paths, fake button IDs, grab transitions.
- **Constraints:** Lightweight, no full test framework required.
- **Status:** TODO

### P3.2 Document critical invariants
- **Files:** `docs/overview.md` or new `docs/invariants.md`
- **Goal:** Explicitly record assumptions future changes must preserve.
- **Status:** TODO
