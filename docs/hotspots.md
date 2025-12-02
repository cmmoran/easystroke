# Potential freeze/crash hotspots (feat/cmmoran-synergy)

This branch adds Synergy-awareness and pointer-barrier logic on top of Easystroke's existing X11 gesture loop. The areas below look risky enough to investigate or harden because they can monopolize the X server or leave the pointer in a bad state, which would feel like a complete window-manager freeze.

## 1) Barrier lifecycle and geometry are fixed at startup
* Pointer barriers on all four edges are created once in the `XState` constructor with hard-coded offsets and no error handling or cleanup (`XFixesCreatePointerBarrier`). They never get destroyed or re-created if monitors are hot-plugged, rotated, or their size changes. That leaves the server with stale barriers that can trap the pointer or generate continuous barrier notifications long after the layout changes.
* The construction also ignores `NULL` returns from the `XFixesCreatePointerBarrier` calls, so later `XIBarrierReleasePointer` calls may operate on invalid IDs, which can trigger X errors during motion handling.
* **How to fix:**
  * Listen for RANDR change notifications and rebuild the four barriers whenever geometry changes; destroy old barriers before creating new ones so the X server never sees a stale `PointerBarrier`.
  * Convert the hard-coded inset values into configuration or derive them from the active monitor rectangles; clamp to safe values when multiple monitors are disconnected.
  * Check every `XFixesCreatePointerBarrier` return value, log failures, and skip subsequent `XIBarrierReleasePointer` calls when a barrier ID is zero. Optionally wrap barrier creation/destruction in helper functions that guard against double-free or invalid IDs.

## 2) Unbounded event pumping under heavy XI/Barrier traffic
* The main X event hook (`XState::handle`) drains the queue in a `while (XPending(dpy))` loop without yielding back to GLib. If barrier hits/leaves or XI motion events start flooding (e.g., pointer pinned on a stale barrier), the handler can spin indefinitely and starve GTK's UI thread. That looks exactly like a frozen desktop even though the process is still alive.
* **How to fix:**
  * Replace the tight `while (XPending)` drain with a bounded batch (e.g., process N events, then return so the main loop can repaint). Maintain a tail-call `g_idle_add` to continue draining when the loop is congested.
  * Drop redundant motion events by coalescing XI motion into the latest position before dispatch, or by using `XIfEvent` with a short timeout instead of unconditional draining.
  * Add a watchdog log/metric when more than a threshold of barrier or motion events are seen per cycle so you can detect and debug storms before they freeze the UI.

## 3) Grab toggling during motion/Barrier leave
* The Synergy detector flips between `grabber->suspend()` and `grabber->resume()` directly inside XI motion and barrier-leave handlers whenever it infers control changes. These calls rebuild XInput grabs on the spot while events are still flowing, which can transiently ungrab the pointer or trigger re-grabs mid-stream. Combined with the tight event loop above, a rapid sequence of barrier crossings could repeatedly tear down and re-establish grabs, risking pointer lockups or X server instability.
* **How to fix:**
  * Move grab toggling to the idle/GTK context: set flags in the XI handler and schedule a deferred update via `g_idle_add` so the grabs are reconstructed away from the hot path.
  * Debounce the state changes—e.g., require stability for 50–100 ms or a minimum distance/time since the last toggle—to avoid churn when hovering near a barrier.
  * Track the current grab state and no-op repeated `suspend`/`resume` calls so that back-to-back barrier notifications do not rebuild grabs unnecessarily.
