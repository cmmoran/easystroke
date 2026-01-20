# Easystroke Codebase Overview (Evidence-Based)

This report is a code-grounded analysis of the current Easystroke fork. It focuses on the **actual behavior** in the repository and highlights end-to-end flow, state machines, hotspots, complexity, and crash risks. No fixes or redesign are proposed here.

---

## 1) High-level architecture overview

### Major subsystems and responsibilities

- **Process entry + global wiring**
  - `main.cc: App::on_activate()` opens the X display (`XOpenDisplay`), initializes prefs/actions (`PrefDB::init`, `ActionDBWatcher::init`), constructs `XState` and `Grabber`, sets up tracing (`init_trace()`), and connects the X11 fd to the GTK main loop via `Glib::IOSource` connected to `XState::handle()`. It also calls `XTestGrabControl(dpy, True)` and forces enter events using `XGrabPointer`/`XUngrabPointer`.
  - Files: `main.cc`, `main.h`.

- **Input capture + device management (X11/XI2)**
  - `Grabber` discovers XI2 devices (`Grabber::init_xi`, `Grabber::new_device`), manages active/excluded devices, and applies XI grabs (`Grabber::set`, `Grabber::grab_xi`, `Grabber::grab_xi_devs`).
  - `Children` watches root window child changes and keeps window-to-frame mappings for app selection (`Children::handle`, `get_app_window`).
  - Files: `grabber.cc`, `grabber.h`.

- **Event dispatch + handler stack**
  - `XState` reads X events, filters them, and dispatches XI2 events into `Handler` stack (`XState::handle`, `handle_event`, `handle_xi2_event`).
  - `Handler::replace_child` swaps active handlers and updates grabs.
  - Files: `handler.cc`, `handler.h`.

- **Gesture capture + recognition**
  - `StrokeHandler` builds `PreStroke` and produces `Stroke` on finish, optionally with timeouts and tracing (`StrokeHandler::motion`, `finish`, `timeout`).
  - `Stroke::compare` uses dynamic programming (`stroke_compare`) for matching.
  - Files: `handler.cc`, `gesture.cc`, `gesture.h`, `stroke.c`, `stroke.h`.

- **Action resolution + execution**
  - `ActionListDiff::handle` and `handle_advanced` match strokes to actions and return `Action` instances; handlers then run them (`Action::run` or specialized handlers).
  - Files: `actiondb.cc`, `actiondb.h`, `actions.cc`.

- **UI + preferences**
  - GTK UI in `win.cc`, `prefs.cc`, `actions.cc` with persistence in `prefdb.cc` and `actiondb.cc`.
  - Files: `win.cc`, `prefs.cc`, `actions.cc`, `prefdb.cc`.

- **Trace rendering**
  - `Trace` and subclasses (`Shape`, `Composite`, `Annotate`, `Fire`, `Water`) render gesture overlays.
  - Files: `trace.h`, `shape.cc`, `composite.cc`, `annotate.cc`, `fire.cc`, `water.cc`.

---

## 2) Detailed event flow walkthrough

### Step-by-step flow: raw input → gesture → action

1) **X11 fd → GTK main loop**
   - `main.cc: App::on_activate` creates a `Glib::IOSource` on `ConnectionNumber(dpy)` and connects it to `XState::handle()`.

2) **Event pump**
   - `XState::handle()` calls `drain_pending_events_batch(64)`.
   - `drain_pending_events_batch` pulls X events via `XNextEvent` and first offers them to `grabber->handle(ev)` (window-tree tracking); unhandled events go to `XState::handle_event(ev)`.
   - Files: `handler.cc` (`XState::handle`, `drain_pending_events_batch`), `grabber.cc` (`Children::handle`).

3) **Window tracking / active app**
   - `XState::handle_event` processes `EnterNotify` events to update `current_app_window` via `get_app_window(w)`.
   - `Grabber::update` uses `current_app_window` to select per-app action lists and apply exceptions.
   - Files: `handler.cc` (`handle_enter_leave`), `grabber.cc` (`get_app_window`, `Grabber::update`).

4) **XI2 event routing**
   - `XState::handle_event` handles `GenericEvent` and calls `XGetEventData`, then `handle_xi2_event`.
   - Files: `handler.cc` (`handle_event`, `handle_xi2_event`).

5) **Button press → handler transition**
   - `handle_xi2_event` on `XI_ButtonPress` sets `current_dev`, updates `modifiers` for the first pressed button, and calls `H->press(...)` (top handler).
   - `IdleHandler::press` activates the current app window and replaces the handler with `StrokeHandler`.
   - Files: `handler.cc` (`handle_xi2_event`, `IdleHandler::press`, `Handler::replace_child`).

6) **Motion → stroke capture**
   - `StrokeHandler::motion` appends points to `PreStroke` and starts trace drawing after distance thresholds.
   - Files: `handler.cc` (`StrokeHandler::motion`), `trace.h`.

7) **Release → stroke recognition**
   - `StrokeHandler::release` calls `finish(0)` to produce a `Stroke` and matches it via `ActionListDiff::handle`.
   - Depending on the action type, it transitions into `ButtonHandler`, `IgnoreHandler`, `ScrollHandler`, or runs the action directly.
   - Files: `handler.cc` (`StrokeHandler::release`), `actiondb.cc` (`ActionListDiff::handle`).

8) **Advanced handling**
   - `AdvancedHandler` is created from `StrokeHandler` for complex mappings, replay/remap, and scroll behavior. It uses `ActionListDiff::handle_advanced` to map actions.
   - Files: `handler.cc` (`AdvancedHandler`), `actiondb.cc` (`handle_advanced`).

### Event suppression, deferral, replay

- **Suppression**: `Grabber::set` decides when to use `XIGrabButton`/`XIGrabDevice` based on mode and suspension. This blocks other clients from receiving events during capture.
  - Files: `grabber.cc` (`Grabber::set`, `grab_xi`, `grab_xi_devs`).

- **Deferral**: `XState::queue` defers actions while the handler stack is non-idle; `Handler::replace_child` flushes queued actions when idle.
  - Files: `handler.cc` (`XState::queue`, `Handler::replace_child`).

- **Replay**: `XTestFake*` calls are used in handlers to synthesize motion/clicks/keys after suppression.
  - Files: `handler.cc` (`IgnoreHandler`, `ButtonHandler`, `AdvancedHandler`), `main.cc` (`SendKey::run`, `SendText::run`).

---

## 3) State management analysis

### Grabber state

- **States**: `Grabber::State { NONE, BUTTON, SELECT, RAW }` with XI-grab state `GrabState { GrabNo, GrabYes, GrabRaw }`.
- **Key transition logic**: `Grabber::set` computes `act` based on `suspended`, `active`, `disabled`, and handler `current` state. It sets XI grabs via `grab_xi` + `grab_xi_devs` and applies XGrabPointer when entering `SELECT`.
- **Invariants**: `grabbed` reflects the applied grab mode; no updates if unchanged.
- Files: `grabber.cc`, `grabber.h`.

### Handler stack

- **Topology**: `IdleHandler` (root) → `StrokeHandler` → `AdvancedHandler` or specialized handlers (scroll, ignore, button).
- **Transition**: `Handler::replace_child` deletes old child, assigns new, calls `grabber->grab(new->grab_mode())`, then flushes queued actions if idle.
- Files: `handler.cc`, `handler.h`.

### Gesture recognition (StrokeHandler)

- **State**: `button`, `trigger`, `cur` (`PreStroke`), `is_gesture`, `drawing`, `last`, `orig`, timeout params, rocker flags.
- **Timeouts**: `init_connection` for gesture init and `connections` for ongoing timeout windows.
- **Invariants**:
  - `cur` is created in `init()` unless `do_instant()` is taken.
  - `trace->start/end` must remain balanced; `Trace::end()` is defensive.
- Files: `handler.cc` (`StrokeHandler`), `trace.h`.

### Advanced handler state

- Tracks `remap_from/remap_to`, `click_time`, `replay_button`, `sticky_mods`, action maps, rankings, and rocker flags.
- Behavior depends on action type (scroll/ignore/button/key) and replays input using XTest.
- Files: `handler.cc` (`AdvancedHandler`).

### Fork-specific state: Synergy control and pointer barriers

- `XState` tracks `controlled`, `target_controlled`, `prevState`, and counters to detect "cycling" behavior.
- `request_control_state` debounces changes; `apply_control_state` toggles `grabber->suspend()`/`resume()`.
- Pointer barriers rebuilt on RandR events.
- Files: `handler.cc` (`get_mouse_state`, `is_cycling_detected`, `request_control_state`, `apply_control_state`, `rebuild_pointer_barriers`).

---

## 4) Identified hotspots

### 4.1 XI_HierarchyChanged fallthrough

- **File/function**: `handler.cc: XState::handle_xi2_event`
- **Why risky**: `case XI_HierarchyChanged:` lacks a `break`, so it falls through to `XI_BarrierHit` and reinterprets the event as `XIBarrierEvent`. This is a direct crash vector during device changes.
- **Assumption**: Event types are handled independently.
- **Failure mode**: Invalid `reinterpret_cast` on non-barrier event.

### 4.2 Pointer barrier handling with partial failure

- **File/function**: `handler.cc: rebuild_pointer_barriers`, `handle_xi2_event`
- **Why risky**: `rebuild_pointer_barriers` logs if barrier creation failed but does not disable barrier logic. `XI_BarrierHit/Leave` still uses barrier handles, which may be zero for failed barriers.
- **Assumption**: Barriers exist if `XI_BarrierHit/Leave` fires.
- **Failure mode**: Unexpected barrier handling, inconsistent control state.

### 4.3 Device removal + raw pointer

- **File/function**: `grabber.cc: hierarchy_changed`, `handler.cc: current_dev usage`
- **Why risky**: `current_dev` is a raw pointer to `xi_devs` entry. On device removal, `xi_devs.erase(...)` frees the `shared_ptr`, and `current_dev` may become stale unless it matches device ID and is nulled.
- **Assumption**: `current_dev` always matches event device or is null.
- **Failure mode**: Use-after-free in handlers referencing `current_dev`.

### 4.4 Experimental event mutation (magic constants)

- **File/function**: `handler.cc: handle_xi2_event`, `StrokeHandler::release`
- **Why risky**: On experimental path, `event->detail` is mutated (`+100`) to signal a fake default button. `StrokeHandler::release` interprets `b > 100` with special handling. This is cross-cutting and brittle.
- **Assumption**: No other code depends on `event->detail` semantics.
- **Failure mode**: Mismatched button IDs or unexpected action mappings.

### 4.5 Name-based rocker behavior

- **File/function**: `actiondb.cc: ActionListDiff::handle_advanced`
- **Why risky**: Rocker gestures are hard-wired to action names (`"Back"`/`"Forward"`), not action types. This couples gesture semantics to user-visible strings.
- **Assumption**: Those names exist and match localization/user config.
- **Failure mode**: Rocker gestures silently fail or map incorrectly.

### 4.6 Primary output fallback without bounds check

- **File/function**: `handler.cc: XState::get_primary_monitor_center`
- **Why risky**: Fallback uses `screenRes->outputs[0]` without checking `noutput > 0`.
- **Assumption**: At least one output exists.
- **Failure mode**: Out-of-bounds memory access.

### 4.7 Timeout connection lifetime

- **File/function**: `handler.cc: StrokeHandler::init`/destructor
- **Why risky**: `init_connection` is not explicitly disconnected in `~StrokeHandler`. Behavior depends on `sigc::connection` destructor semantics.
- **Assumption**: Disconnect on destruction is safe and automatic.
- **Failure mode**: Use-after-free if timeout fires post-destruction.

---

## 5) Complexity assessment

- **Input dispatch mixes concerns**: `XState` handles event dispatch, window activation, pointer-barrier logic, and Synergy-style control in the same class (`handler.cc`). This blurs responsibilities between low-level input handling and higher-level system control.

- **Gesture capture intertwined with replay**: `StrokeHandler` creates strokes and `AdvancedHandler` replays events (XTest) while still in the input pipeline. This interleaves recognition and event synthesis in ways that are hard to reason about.

- **Experimental flag is cross-cutting**: `experimental` changes grabbing, modifies XI events, and alters stroke release behavior across multiple modules (`grabber.cc`, `handler.cc`). This increases complexity and makes behavior mode-dependent.

- **Rocker gestures embed action-name semantics**: The rocker path uses string names for action selection (`"Back"`, `"Forward"`), which couples UI naming to core behavior.

- **Window tracking located in grabber**: The `Children` window tree management and frame mapping are embedded in input/grab logic (`grabber.cc`). This conflates event-grab responsibilities with window management.

---

## 6) Crash-risk summary (ranked)

1) **XI_HierarchyChanged fallthrough → invalid cast**
   - `handler.cc: XState::handle_xi2_event` missing `break` causes barrier handling on hierarchy events.

2) **Primary output fallback without bounds check**
   - `handler.cc: get_primary_monitor_center` uses `outputs[0]` without checking `noutput`.

3) **Raw pointer `current_dev` after device removal**
   - `grabber.cc: hierarchy_changed` erases device; multiple handlers assume `current_dev` valid.

4) **XI2 event reinterpretation assumptions**
   - `handler.cc: handle_xi2_event` casts `XIDeviceEvent*` to `XIRawEvent*` for `XI_RawMotion`.

5) **Timeout connection lifetime**
   - `handler.cc: StrokeHandler` `init_connection` is not explicitly disconnected.

6) **Experimental event mutation**
   - `handler.cc: handle_xi2_event` modifies `event->detail` and relies on magic thresholds in `StrokeHandler::release`.

---

## 7) Open questions (require runtime validation)

- **sigc++ timeout lifetime**: Does destruction of `sigc::connection` guarantee no callbacks after `StrokeHandler` destruction?
- **XI2 event layout assumptions**: Are `XIDeviceEvent` vs `XIRawEvent` layout assumptions safe for all target servers?
- **Pointer barrier behavior on multi-head**: Barriers are built using global screen dims; need runtime observation on multi-monitor setups.
- **Rocker gesture action names**: Are `"Back"`/`"Forward"` names guaranteed and localized? If not, rocker mapping may be fragile.
- **`current_app_window` accuracy under grabs**: Enter/Leave events are filtered on grab/mode; verify correct app selection under heavy grabbing.

---

## Legacy vs fork-specific vs GTK/X11 responsibilities

- **Legacy behavior (core pipeline)**
  - Gesture creation & comparison: `StrokeHandler` → `Stroke::create` → `Stroke::compare` (`handler.cc`, `gesture.cc`, `stroke.c`).
  - XI2 grabbing and event dispatch: `Grabber::init_xi`/`set`, `XState::handle_xi2_event` (`grabber.cc`, `handler.cc`).
  - XTest replay for actions: `SendKey::run`, `ButtonHandler`, `IgnoreHandler` (`main.cc`, `handler.cc`).

- **Fork-specific behavior**
  - Rocker gestures: `Stroke` has `isRockerLeft/isRockerRight`; `StrokeHandler::finish` sets rocker state; `ActionListDiff::handle_advanced` selects `Back`/`Forward` based on rocker flags.
  - Experimental flag: alters grab logic and event processing (`Grabber::grab_xi`, `handle_xi2_event`, `StrokeHandler::release`).
  - Synergy-style control and pointer barriers: `XState` cycling detection and control-state toggling (`handler.cc`).

- **GTK responsibilities**
  - UI dialogs, actions, prefs, icons (`win.cc`, `actions.cc`, `prefs.cc`).

- **X11/XI2 responsibilities**
  - Grabbing, event routing, pointer barriers, XTest injection (`grabber.cc`, `handler.cc`, `main.cc`).

---

End of report.
