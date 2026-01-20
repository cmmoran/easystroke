# Stability Plan (Derived From docs/overview.md)

This plan lists **itemized, priority-ordered improvements** that preserve current behavior while increasing stability and performance. It is derived from the authoritative analysis in `docs/overview.md` and focuses on minimal, behavior-preserving changes.

---

## Priority 0 (Immediate crash-risk eliminations)

1) **Fix XI_HierarchyChanged fallthrough** (completed 2026-01-20)
   - **Why**: `XState::handle_xi2_event` falls through from `XI_HierarchyChanged` into `XI_BarrierHit`, reinterpreting the event as `XIBarrierEvent` (direct crash vector).
   - **Target**: `handler.cc: XState::handle_xi2_event`.
   - **Change type**: Single-line control flow guard (add `break`).

2) **Guard primary output fallback when no outputs exist** (completed 2026-01-20)
   - **Why**: `get_primary_monitor_center` uses `screenRes->outputs[0]` without checking `noutput`, which can be out of bounds.
   - **Target**: `handler.cc: XState::get_primary_monitor_center`.
   - **Change type**: Defensive bounds check with fallback or early return.

3) **Harden pointer barrier event handling when barriers are missing** (completed 2026-01-20)
   - **Why**: `rebuild_pointer_barriers` can partially fail; barrier hit/leave handlers still assume valid barrier IDs.
   - **Target**: `handler.cc: rebuild_pointer_barriers`, `handle_xi2_event`.
   - **Change type**: Early exits or checks for nonzero barrier IDs before use.

4) **Guard XI2 cookie data size before reinterpret casts**
   - **Why**: `handle_xi2_event` reinterprets `cookie->data` as XI2 structs without size checks; truncated or malformed events can crash.
   - **Target**: `handler.cc: XState::handle_xi2_event`.
   - **Change type**: Defensive size validation or safe fallback before casting.

5) **Null-check `get_xi_dev` in experimental release path**
   - **Why**: The experimental path dereferences `grabber->get_xi_dev(event->deviceid)->name` without guarding against device removal.
   - **Target**: `handler.cc: XState::handle_xi2_event` (XI_ButtonRelease).
   - **Change type**: Guard `get_xi_dev` result; skip remap if missing.

---

## Priority 1 (High-risk stability improvements)

1) **Make `current_dev` lifetime-safe across device removal** (completed 2026-01-20)
   - **Why**: `current_dev` is a raw pointer into `xi_devs` entries; device removal can invalidate it.
   - **Target**: `handler.cc: XState::current_dev` usages, `grabber.cc: hierarchy_changed`.
   - **Change type**: Defensive nulling and ID consistency checks before dereference.

2) **Ensure all timeout connections are cleanly disconnected** (completed 2026-01-20)
   - **Why**: `StrokeHandler::init_connection` depends on `sigc::connection` destructor semantics; a late timeout can fire after destruction.
   - **Target**: `handler.cc: StrokeHandler`.
   - **Change type**: Explicit disconnect in destructor or RAII helper to enforce disconnection.

3) **Constrain XI2 event reinterpretation to correct event types** (completed 2026-01-20)
   - **Why**: `XIDeviceEvent*` is reinterpreted as `XIRawEvent*` based on evtype; if mismatched, behavior is undefined.
   - **Target**: `handler.cc: handle_xi2_event`, `handle_raw_motion`.
   - **Change type**: Validate event type and size before cast; short-circuit on mismatch.

4) **Avoid pre-casting XI2 cookie data to `XIDeviceEvent`**
   - **Why**: `handle_xi2_event` reads `event->evtype` after casting cookie data to `XIDeviceEvent`, which can be invalid for non-device events.
   - **Target**: `handler.cc: XState::handle_xi2_event`.
   - **Change type**: Use cookie `evtype` to select struct type before casting.

5) **Harden RawMotion valuator assumptions**
   - **Why**: RawMotion handling assumes valuators 0/1 map to X/Y; devices with different layouts can misbehave or crash.
   - **Target**: `handler.cc: XState::handle_raw_motion`.
   - **Change type**: Validate valuator mask/indices before indexing raw values.

---

## Priority 2 (Behavior-preserving robustness and diagnostics)

1) **Add targeted invariant checks in handler transitions**
   - **Why**: `Handler::replace_child` controls grab mode transitions and queued callbacks; errors here can lead to stuck grabs or reentrancy issues.
   - **Target**: `handler.cc: Handler::replace_child`, `XState::queue`.
   - **Change type**: Debug-only assertions/verbosity checks that do not alter behavior.

2) **Strengthen state-reset on screen changes**
   - **Why**: Pointer barrier logic depends on screen geometry and state counters; stale state can cause incorrect control transitions.
   - **Target**: `handler.cc: update_screen_metrics`, `handle_randr_event`.
   - **Change type**: Explicitly reset all related tracking variables (already partially done; complete the set).

3) **Reduce event-data mutation footprint in experimental mode**
   - **Why**: `event->detail` is mutated to `100+` to signal special behavior; this leaks magic constants across the pipeline.
   - **Target**: `handler.cc: handle_xi2_event`, `StrokeHandler::release`.
   - **Change type**: Encapsulate mutation in a local variable to avoid changing the original event struct.

4) **Clarify rocker gesture mapping in action matching**
   - **Why**: Rocker gestures are hard-wired to action names `"Back"`/`"Forward"`, which is fragile and localization-dependent.
   - **Target**: `actiondb.cc: ActionListDiff::handle_advanced`.
   - **Change type**: Add a clear, behavior-preserving mapping layer or normalization step (still matching existing names by default).

5) **Skip pointer barrier rebuild with invalid screen geometry**
   - **Why**: If screen metrics are zero/invalid during RandR changes, barriers are rebuilt with bad coordinates.
   - **Target**: `handler.cc: XState::update_screen_metrics`, `rebuild_pointer_barriers`.
   - **Change type**: Guard rebuild on valid dimensions; log and skip on invalid geometry.

---

## Priority 3 (Performance-neutral cleanup that preserves behavior)

1) **Clean up XI2 mask allocations to avoid leaks**
   - **Why**: `initialize_xi_mask` allocates masks but never frees them. This is a small leak but persistent.
   - **Target**: `grabber.cc: initialize_xi_mask`, `Grabber::init_xi`.
   - **Change type**: Own masks in RAII wrapper or free on shutdown.

2) **Consolidate trace start/end and gesture abort paths**
   - **Why**: `StrokeHandler::finish`, `timeout`, and destructor all call `trace->end()`; this is safe but scattered.
   - **Target**: `handler.cc: StrokeHandler`.
   - **Change type**: Internal helper for trace shutdown to reduce sequencing mistakes (no behavioral change).

3) **Add low-cost logging around grab transitions**
   - **Why**: Grabbing/suspension state transitions are central to correctness. Logging only on verbose levels can help diagnose deadlocks without changing behavior.
   - **Target**: `grabber.cc: Grabber::set`, `XState::apply_control_state`.
   - **Change type**: Verbosity-gated logs; no functional change.

---

## Validation-focused tasks (no behavioral changes)

1) **Add lightweight runtime validation hooks**
   - **Why**: Several open questions require runtime validation (e.g., XI2 event layout, multi-monitor barriers).
   - **Targets**: `handler.cc`, `grabber.cc`.
   - **Change type**: Debug-only counters/logs triggered by verbose levels.

2) **Add minimal, opt-in tests for stroke comparison invariants**
   - **Why**: `stroke_compare` drives action matching. A tiny test ensures compatibility during refactors.
   - **Targets**: `stroke.c`, `gesture.cc` (test scaffolding only).
   - **Change type**: A minimal test harness (optional) verifying known comparisons are stable.

---

## Notes

- The items above intentionally avoid refactors or behavior changes. They focus on stability, clear invariants, and defensive guards.
- Each item maps directly to risks in `docs/overview.md` and targets specific code regions to minimize regression risk.
