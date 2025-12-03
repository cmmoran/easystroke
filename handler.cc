/*
 * Copyright (c) 2008-2012, Thomas Jaeger <ThJaeger@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "handler.h"
#include "main.h"
#include "trace.h"
#include "win.h" // Why?
#include "prefs.h" // Why?
#include <gtkmm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XTest.h>
#include <X11/XKBlib.h>
#include <X11/Xproto.h>
#include <cmath>  // std::abs(float)
using std::abs;

XState *xstate = nullptr;

extern Window get_app_window(Window w);

extern Source<Window> current_app_window;
extern boost::shared_ptr<Trace> trace;


boost::shared_ptr<sigc::slot<void, RStroke> > stroke_action;

static XAtom EASYSTROKE_PING("EASYSTROKE_PING");

bool XState::idle() {
    return !handler->child;
}

void XState::queue(sigc::slot<void> f) {
    if (idle()) {
        f();
        XFlush(dpy);
    } else
        queued.push_back(f);
}

void XState::handle_enter_leave(XEvent &ev) {
    if (ev.xcrossing.mode == NotifyGrab)
        return;
    if (ev.xcrossing.detail == NotifyInferior)
        return;
    const Window w = ev.xcrossing.window;
    if (ev.type == EnterNotify) {
        current_app_window.set(get_app_window(w));
        if (verbosity >= 3)
            printf("Entered window 0x%lx -> 0x%lx\n", w, current_app_window.get());
    } else printf("Error: Bogus Enter/Leave event\n");
}

#define H (handler->top())

void XState::handle_event(XEvent &ev) {
    if (randr_event_base >= 0 &&
        (ev.type == randr_event_base + RRScreenChangeNotify || ev.type == randr_event_base + RRNotify)) {
        handle_randr_event(ev);
        return;
    }

    switch (ev.type) {
        case EnterNotify:
        case LeaveNotify:
            handle_enter_leave(ev);
            return;

        case PropertyNotify:
            if (current_app_window.get() == ev.xproperty.window && ev.xproperty.atom == XA_WM_CLASS)
                current_app_window.notify();
            return;

        case ButtonPress:
            if (verbosity >= 3)
                printf("Press (master): %d (%d, %d) at t = %ld\n", ev.xbutton.button, ev.xbutton.x, ev.xbutton.y,
                       ev.xbutton.time);
            H->press_master(ev.xbutton.button, ev.xbutton.time);
            return;

        case ClientMessage:
            if (ev.xclient.window != ping_window)
                return;
            if (ev.xclient.message_type == *EASYSTROKE_PING) {
                if (verbosity >= 3)
                    printf("Pong\n");
                H->pong();
            }
            return;

        case MappingNotify:
            if (ev.xmapping.request == MappingPointer)
                update_core_mapping();
            if (ev.xmapping.request == MappingKeyboard || ev.xmapping.request == MappingModifier)
                XRefreshKeyboardMapping(&ev.xmapping);
            return;
        case GenericEvent:
            if (ev.xcookie.extension == grabber->opcode && XGetEventData(dpy, &ev.xcookie)) {
                handle_xi2_event(static_cast<XIDeviceEvent *>(ev.xcookie.data));
                XFreeEventData(dpy, &ev.xcookie);
            }
    }
}

void XState::activate_window(Window w, Time t) {
    static XAtom _NET_ACTIVE_WINDOW("_NET_ACTIVE_WINDOW");
    static XAtom _NET_WM_WINDOW_TYPE("_NET_WM_WINDOW_TYPE");
    static XAtom _NET_WM_WINDOW_TYPE_DOCK("_NET_WM_WINDOW_TYPE_DOCK");
    static XAtom WM_PROTOCOLS("WM_PROTOCOLS");
    static XAtom WM_TAKE_FOCUS("WM_TAKE_FOCUS");

    if (w == get_window(ROOT, *_NET_ACTIVE_WINDOW)) {
        printf("Ignoring ROOT active window\n");
        return;
    }

    const Atom window_type = get_atom(w, *_NET_WM_WINDOW_TYPE);
    if (window_type == *_NET_WM_WINDOW_TYPE_DOCK) {
        printf("Ignoring dock window\n");
        return;
    }

    XWMHints *wm_hints = XGetWMHints(dpy, w);
    if (wm_hints) {
        bool input = wm_hints->input;
        XFree(wm_hints);
        if (!input)
            return;
    }

    if (!has_atom(w, *WM_PROTOCOLS, *WM_TAKE_FOCUS)) {
        return;
    }

    XWindowAttributes attr;
    if (XGetWindowAttributes(dpy, w, &attr) && attr.override_redirect) {
        printf("Ignoring override_redirect window\n");
        return;
    }

    if (verbosity >= 3) {
        printf("Giving focus to window 0x%lx\n", w);
    }

    icccm_client_message(w, *WM_TAKE_FOCUS, t);
}

Window XState::get_window(Window w, Atom prop) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop_return = nullptr;

    if (XGetWindowProperty(dpy, w, prop, 0, sizeof(Atom), False, XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop_return) != Success)
        return None;
    if (!prop_return)
        return None;
    const Window ret = *reinterpret_cast<Window *>(prop_return);
    XFree(prop_return);
    return ret;
}

Atom XState::get_atom(Window w, Atom prop) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop_return = nullptr;

    if (XGetWindowProperty(dpy, w, prop, 0, sizeof(Atom), False, XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop_return) != Success)
        return None;
    if (!prop_return)
        return None;
    const Atom atom = *reinterpret_cast<Atom *>(prop_return);
    XFree(prop_return);
    return atom;
}

bool XState::has_atom(Window w, Atom prop, Atom value) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop_return = nullptr;

    if (XGetWindowProperty(dpy, w, prop, 0, sizeof(Atom), False, XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop_return) != Success)
        return None;
    if (!prop_return)
        return None;
    const auto atoms = reinterpret_cast<Atom *>(prop_return);
    bool ans = false;
    for (unsigned long i = 0; i < nitems; i++)
        if (atoms[i] == value)
            ans = true;
    XFree(prop_return);
    return ans;
}

void XState::icccm_client_message(Window w, Atom a, Time t) {
    static XAtom WM_PROTOCOLS("WM_PROTOCOLS");
    XClientMessageEvent ev;
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = *WM_PROTOCOLS;
    ev.format = 32;
    ev.data.l[0] = a;
    ev.data.l[1] = t;
    XSendEvent(dpy, w, False, 0, reinterpret_cast<XEvent *>(&ev));
}

static void print_coordinates(XIValuatorState *valuators, double *values) {
    int n = 0;
    for (int i = valuators->mask_len - 1; i >= 0; i--)
        if (XIMaskIsSet(valuators->mask, i)) {
            n = i + 1;
            break;
        }
    bool first = true;
    int elt = 0;
    for (int i = 0; i < n; i++) {
        if (first)
            first = false;
        else
            printf(", ");
        if (XIMaskIsSet(valuators->mask, i))
            printf("%.3f", values[elt++]);
        else
            printf("*");
    }
}

static double get_axis(XIValuatorState &valuators, int axis) {
    if (axis < 0 || !XIMaskIsSet(valuators.mask, axis)) {
        return 0.0;
    }
    const double *val = valuators.values;
    for (int i = 0; i < axis; i++) {
        if (XIMaskIsSet(valuators.mask, i)) {
            val++;
        }
    }
    return *val;
}

void XState::report_xi2_event(XIDeviceEvent *event, const char *type) {
    printf("%s (XI2): ", type);
    if (event->detail) {
        printf("%d ", event->detail);
    }
    if (std::strcmp(type, "RawMotion") != 0) {
        printf("(%.3f, %.3f) - (", event->root_x, event->root_y);
    }
    print_coordinates(&event->valuators, event->valuators.values);
    printf(") at t = %ld\n", event->time);
}

bool XState::is_synergy_bound(int x, int y) {
    return x >= x_min && x <= x_max && y >= y_min && y <= y_max;
}

bool XState::is_cycling_detected(MouseState currentState) {
    // Detect if the state transitions from EDGE to CENTER (Cycling behavior)
    if (prevState == BOUND && currentState == CENTER) {
        transitionOutCount = 0;
        transitionCount++;

        // If we've seen the transition twice, we can assume it's controlled
        if (transitionCount >= requiredTransitions) {
            if (!controlled) {
                if (verbosity >= 4) printf("Transition in threshold reached!\n");
            }
            transitionCount = 0;
            prevState = currentState;
            return true;
        }
        return false;
    }
    if (prevState == BOUND && currentState == OUTSIDE) {
        transitionCount = 0;
        transitionOutCount++;

        if (transitionOutCount >= requiredOutTransitions) {
            if (controlled) {
                if (verbosity >= 4) printf("Transition out threshold reached!\n");
            }
            transitionOutCount = 0;
            prevState = currentState;
            return true;
        }
        return false;
    }
    if (prevState == BOUND && currentState == BOUND) {
        return false;
    }
    transitionCount = 0;
    transitionOutCount = 0;
    // Update the previous state
    prevState = currentState;
    return false;
}

MouseState XState::get_mouse_state(int x, int y) {
    bool in_bounds = is_synergy_bound(x, y);
    int centerX = (x_min + x_max) / 2;
    int centerY = (y_min + y_max) / 2;
    bool near_center = std::abs(x - centerX) <= cycling_threshold && std::abs(y - centerY) <= cycling_threshold;

    switch (prevState) {
        case NONE:
            if (near_center) {
                if (verbosity >= 4) printf("%s -> Center ( %d, %d )\n", state_name[prevState], x, y);
                prevState = CENTER;
            } else if (in_bounds) {
                if (verbosity >= 4) printf("%s -> Bound ( %d, %d)\n", state_name[prevState], x, y);
                prevState = BOUND;
            } else {
                if (verbosity >= 4) printf("%s -> Outside ( %d, %d)\n", state_name[prevState], x, y);
                prevState = OUTSIDE;
            }

            return prevState;
        case CENTER:
            if (near_center) {
                return CENTER;
            }
            if (!controlled && in_bounds) {
                if (verbosity >= 4) printf("%s -> Bound ( %d, %d )\n", state_name[prevState], x, y);
                return BOUND;
            }
            return OUTSIDE;
        case OUTSIDE:
            if (!controlled && near_center) {
                if (verbosity >= 4) printf("%s -> Center ( %d, %d )\n", state_name[prevState], x, y);
                return CENTER;
            }
            if (controlled && in_bounds) {
                if (verbosity >= 4) printf("%s -> Bound ( %d, %d )\n", state_name[prevState], x, y);
                return BOUND;
            }
            break;
        case BOUND: {
            if (!controlled && near_center) {
                if (verbosity >= 4) printf("%s -> Center ( %d, %d )\n", state_name[prevState], x, y);
                return CENTER;
            }
            if (!in_bounds) {
                if (verbosity >= 4) printf("%s -> Outside ( %d, %d )\n", state_name[prevState], x, y);
                return OUTSIDE;
            }
        }
    }

    return prevState;
}

void XState::handle_xi2_event(XIDeviceEvent *event) {
    switch (event->evtype) {
        case XI_ButtonPress:
            if (verbosity >= 3)
                report_xi2_event(event, "Press");
            if (!xinput_pressed.empty()) {
                if (!current_dev || current_dev->dev != event->deviceid) {
                    break;
                }
            } else {
                current_app_window.set(get_app_window(event->child));
                if (verbosity >= 3) {
                    printf("Active window 0x%lx -> 0x%lx\n", event->child, current_app_window.get());
                }
            }
            current_dev = grabber->get_xi_dev(event->deviceid);
            if (!current_dev) {
                printf("Warning: Spurious device event\n");
                break;
            }
            if (current_dev->master) {
                XISetClientPointer(dpy, None, current_dev->master);
            }
            if (xinput_pressed.empty()) {
                const guint default_mods = grabber->get_default_mods(event->detail);
                if (default_mods == AnyModifier || default_mods == static_cast<guint>(event->mods.base)) {
                    modifiers = AnyModifier;
                } else {
                    modifiers = event->mods.base;
                }
            }
            xinput_pressed.insert(event->detail);
            in_proximity = get_axis(event->valuators, current_dev->proximity_axis);
            H->press(event->detail, create_triple(event->root_x, event->root_y, event->time));
            break;
        case XI_ButtonRelease:
            if (verbosity >= 3) {
                report_xi2_event(event, "Release");
            }
            if (!current_dev || current_dev->dev != event->deviceid) {
                printf("not current_dev, or deviceid is wrong; ignoring event\n");
                break;
            }

            xinput_pressed.erase(event->detail);
            in_proximity = get_axis(event->valuators, current_dev->proximity_axis);

            if (experimental) {
                //if device is disabled, but extra buttons followed, treat last extra button as the default button
                for (auto i = prefs.excluded_devices.ref().begin(); i != prefs.excluded_devices.ref().end(); ++i) {
                    //check if the grabbed device name is in disabled device list
                    if (!i->compare(grabber->get_xi_dev(event->deviceid)->name)) {
                        //check if the button is same as last extra button
                        if (!prefs.extra_buttons.ref().empty() && static_cast<guint>(event->detail) == prefs.
                            extra_buttons.ref().rbegin()->button) {
                            event->detail = 100 + event->detail; //fake the default button
                        }
                        //event->detail = prefs.button.ref().button; //fake the default button
                    }
                }
            }

            H->release(event->detail, create_triple(event->root_x, event->root_y, event->time));
            break;
        case XI_Motion:
            {
                MouseState currentState = get_mouse_state(event->root_x, event->root_y);
                bool cycling_detected = is_cycling_detected(currentState);
                if (cycling_detected || prevState != currentState) {
                    if (prevState != currentState) {
                        if (verbosity >= 4) printf("Mouse state changed from %s to %s ( %0.2f, %0.2f )\n", state_name[prevState], state_name[currentState], event->root_x, event->root_y);
                    }
                    if (cycling_detected) {
                        if (!controlled && prevState == CENTER && currentState == CENTER) {
                            if (verbosity >= 3) printf("Mouse is now controlled by Synergy (Cycling detected)!\n");
                            request_control_state(true, "cycling detection while centered");
                        } else if (prevState == OUTSIDE && currentState == OUTSIDE) {
                            if (verbosity >= 3) printf("Mouse is likely no longer controlled by Synergy\n");
                            request_control_state(false, "cycling detection outside");
                        }
                    }
                }
            }
            if (verbosity >= 5) {
                report_xi2_event(event, "Motion");
            }
            if (!current_dev || current_dev->dev != event->deviceid) {
                break;
            }
            H->motion(create_triple(event->root_x, event->root_y, event->time));
            break;
        case XI_RawMotion:
            if (current_dev && current_dev->dev == event->deviceid) {
                in_proximity = get_axis(reinterpret_cast<XIRawEvent *>(event)->valuators, current_dev->proximity_axis);
            }
            handle_raw_motion(reinterpret_cast<XIRawEvent *>(event));
            break;
        case XI_HierarchyChanged:
            if (grabber->hierarchy_changed(reinterpret_cast<XIHierarchyEvent *>(event))) {
                win->prefs_tab->update_device_list();
            }
        case XI_BarrierHit: {
                const XIBarrierEvent *ev = reinterpret_cast<XIBarrierEvent *>(event);
                if (ev->barrier)
                    XIBarrierReleasePointer(dpy, ev->deviceid, ev->barrier, ev->eventid);
                XFlush(dpy);
                if (ev->barrier == top) {
                    if (verbosity >= 3) printf("Top barrier hit %s (%0.2f, %0.2f)\n", controlled ? "controlled" : "uncontrolled", ev->root_x, ev->root_y);
                } else if (ev->barrier == bottom) {
                    if (verbosity >= 3) printf("Bottom barrier hit %s (%0.2f, %0.2f)\n", controlled ? "controlled" : "uncontrolled", ev->root_x, ev->root_y);
                } else if (ev->barrier == left) {
                    if (verbosity >= 3) printf("Left barrier hit %s (%0.2f, %0.2f)\n", controlled ? "controlled" : "uncontrolled", ev->root_x, ev->root_y);
                } else if (ev->barrier == right) {
                    if (verbosity >= 3) printf("Right barrier hit %s (%0.2f, %0.2f)\n", controlled ? "controlled" : "uncontrolled", ev->root_x, ev->root_y);
                }
            }
            break;
        case XI_BarrierLeave: {
            const XIBarrierEvent *ev = reinterpret_cast<XIBarrierEvent *>(event);
            if (ev->barrier)
                XIBarrierReleasePointer(dpy, ev->deviceid, ev->barrier, ev->eventid);
            XFlush(dpy);
            if (ev->barrier == top) {
                if (ev->root_y > screenTop) {
                    prevState = OUTSIDE;
                    request_control_state(false, "top leave below barrier");
                    if (verbosity >= 3) printf("Top barrier leave (resumed) uncontrolled (%0.2f, %0.2f)\n", ev->root_x, ev->root_y);
                } else if (ev->root_y <= screenTop) {
                    prevState = OUTSIDE;
                    request_control_state(true, "top leave above barrier");
                    if (verbosity >= 3) printf("Top barrier leave (suspended) controlled (%0.2f, %0.2f)\n", ev->root_x, ev->root_y);
                }
            } else if (ev->barrier == bottom) {
                if (ev->root_y >= screenBot) {
                    prevState = OUTSIDE;
                    request_control_state(true, "bottom leave below screen");
                    if (verbosity >= 3) printf("Bottom barrier leave DOWN controlled (%0.2f, %0.2f)\n", ev->root_x, ev->root_y);
                }
            } else if (ev->barrier == left) {
                if (ev->root_x <= screenLeft) {
                    prevState = OUTSIDE;
                    request_control_state(true, "left barrier leave");
                    if (verbosity >= 3) printf("Left barrier leave LEFT controlled (%0.2f, %0.2f)\n", ev->root_x, ev->root_y);
                }
            } else if (ev->barrier == right) {
                if (ev->root_x >= screenRight) {
                    prevState = OUTSIDE;
                    request_control_state(true, "right barrier leave");
                    if (verbosity >= 3) printf("Right barrier leave RIGHT controlled (%0.2f, %0.2f)\n", ev->root_x, ev->root_y);
                }
            }
            if (verbosity >= 3)
                printf("[Barrier Leave] Barrier properties: %d, %d, %d, %d\n", screenLeft, screenTop, screenRight, screenBot);
        }
        break;

    }
}

void XState::handle_raw_motion(XIRawEvent *event) {
    if (!current_dev || current_dev->dev != event->deviceid)
        return;
    double x = 0.0, y = 0.0;
    bool abs_x = current_dev->absolute;
    bool abs_y = current_dev->absolute;
    int i = 0;

    if (XIMaskIsSet(event->valuators.mask, 0))
        x = event->raw_values[i++];
    else
        abs_x = false;

    if (XIMaskIsSet(event->valuators.mask, 1))
        y = event->raw_values[i];
    else
        abs_y = false;

    if (verbosity >= 5) {
        printf("Raw motion (XI2): (");
        print_coordinates(&event->valuators, event->raw_values);
        printf(") at t = %ld\n", event->time);
    }

    H->raw_motion(create_triple(x * current_dev->scale_x, y * current_dev->scale_y, event->time), abs_x, abs_y);
}

#undef H

bool XState::handle(Glib::IOCondition) {
    bool more = drain_pending_events_batch(64);
    if (more && !drain_idle.connected())
        drain_idle = Glib::signal_idle().connect(sigc::mem_fun(*this, &XState::drain_pending_events));
    return true;
}

void XState::update_core_mapping() {
    unsigned char map[MAX_BUTTONS];
    const int n = XGetPointerMapping(dpy, map, MAX_BUTTONS);
    core_inv_map.clear();
    for (int i = n - 1; i; i--) {
        if (map[i] == i + 1) {
            core_inv_map.erase(i + 1);
        } else {
            core_inv_map[map[i]] = i + 1;
        }
    }
}

void XState::fake_core_button(guint b, bool press) {
    if (core_inv_map.count(b)) {
        b = core_inv_map[b];
    }
    XTestFakeButtonEvent(dpy, b, press, CurrentTime);
    XSync(dpy, False);
}

void XState::fake_click(guint b) {
    fake_core_button(b, true);
    fake_core_button(b, false);
}

void Handler::replace_child(Handler *c) {
    delete child;
    child = c;
    if (child)
        child->parent = this;
    if (verbosity >= 2) {
        std::string stack;
        for (Handler *h = child ? child : this; h; h = h->parent) {
            stack = h->name() + " " + stack;
        }
        printf("New event handling stack: %s\n", stack.c_str());
    }
    Handler *new_handler = child ? child : this;
    grabber->grab(new_handler->grab_mode());
    if (child) {
        child->init();
    }
    while (!xstate->queued.empty() && xstate->idle()) {
        (*xstate->queued.begin())();
        xstate->queued.pop_front();
    }
}

class IgnoreHandler : public Handler {
    RModifiers mods;
    bool proximity;

public:
    IgnoreHandler(RModifiers mods_) : mods(mods_), proximity(xstate->in_proximity && prefs.proximity.get()) {
    }

    virtual void press(guint b, RTriple e) {
        if (xstate->current_dev->master) {
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, 0);
            XTestFakeButtonEvent(dpy, b, true, CurrentTime);
        }
    }

    virtual void motion(RTriple e) {
        if (xstate->current_dev->master) {
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, 0);
        }
        if (proximity && !xstate->in_proximity)
            parent->replace_child(nullptr);
    }

    virtual void release(guint b, RTriple e) {
        if (xstate->current_dev->master) {
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, 0);
            XTestFakeButtonEvent(dpy, b, false, CurrentTime);
        }
        if (proximity ? !xstate->in_proximity : xstate->xinput_pressed.empty())
            parent->replace_child(nullptr);
    }

    virtual std::string name() { return "Ignore"; }
    virtual Grabber::State grab_mode() { return Grabber::NONE; }
};

class ButtonHandler : public Handler {
    RModifiers mods;
    guint button, real_button;
    bool proximity;

public:
    ButtonHandler(RModifiers mods_, guint button_) : mods(mods_),
                                                     button(button_),
                                                     real_button(0),
                                                     proximity(xstate->in_proximity && prefs.proximity.get()) {
    }

    virtual void press(guint b, RTriple e) {
        if (xstate->current_dev->master) {
            if (!real_button)
                real_button = b;
            if (real_button == b)
                b = button;
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, 0);
            XTestFakeButtonEvent(dpy, b, true, CurrentTime);
        }
    }

    virtual void motion(RTriple e) {
        if (xstate->current_dev->master)
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, 0);
        if (proximity && !xstate->in_proximity)
            parent->replace_child(nullptr);
    }

    virtual void release(guint b, RTriple e) {
        if (xstate->current_dev->master) {
            if (real_button == b)
                b = button;
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, 0);
            XTestFakeButtonEvent(dpy, b, false, CurrentTime);
        }
        if (proximity ? !xstate->in_proximity : xstate->xinput_pressed.empty())
            parent->replace_child(nullptr);
    }

    virtual std::string name() { return "Button"; }
    virtual Grabber::State grab_mode() { return Grabber::NONE; }
};

void XState::bail_out() {
    handler->replace_child(nullptr);
    xinput_pressed.clear();
    XFlush(dpy);
}

int XState::xErrorHandler(Display *dpy2, XErrorEvent *e) {
    if (dpy != dpy2)
        return xstate->oldHandler(dpy2, e);
    if (verbosity <= 5 && e->error_code == BadWindow) {
        switch (e->request_code) {
            case X_ChangeWindowAttributes:
            case X_GetProperty:
            case X_QueryTree:
                return 0;
        }
    }
    char text[64];
    XGetErrorText(dpy, e->error_code, text, sizeof text);
    char msg[16];
    snprintf(msg, sizeof msg, "%d", e->request_code);
    char def[128];
    if (e->request_code < 128)
        snprintf(def, sizeof def, "request_code=%d, minor_code=%d", e->request_code, e->minor_code);
    else
        snprintf(def, sizeof def, "extension=%s, request_code=%d", xstate->opcodes[e->request_code].c_str(),
                 e->minor_code);
    char dbtext[128];
    XGetErrorDatabaseText(dpy, "XRequest", msg, def, dbtext, sizeof dbtext);
    printf("XError: %s: %s\n", text, dbtext);

    return 0;
}

int XState::xIOErrorHandler(Display *dpy2) {
    if (dpy != dpy2)
        return xstate->oldIOHandler(dpy2);
    printf("Fatal Error: Connection to X server lost\n");
    quit();
    return 0;
}

void XState::ping() {
    XClientMessageEvent ev;
    ev.type = ClientMessage;
    ev.window = ping_window;
    ev.message_type = *EASYSTROKE_PING;
    ev.format = 32;
    XSendEvent(dpy, ping_window, False, 0, reinterpret_cast<XEvent *>(&ev));
    XFlush(dpy);
}

void XState::remove_device(int deviceid) {
    if (current_dev && current_dev->dev == deviceid)
        current_dev = nullptr;
}

void XState::ungrab(int deviceid) {
    if (current_dev && current_dev->dev == deviceid)
        xinput_pressed.clear();
}

class WaitForPongHandler : public Handler, protected Timeout {
public:
    WaitForPongHandler() { set_timeout(100); }

    virtual void timeout() {
        printf("Warning: %s timed out\n", "WaitForPongHandler");
        xstate->bail_out();
    }

    virtual void pong() { parent->replace_child(nullptr); }
    virtual std::string name() { return "WaitForPong"; }
    virtual Grabber::State grab_mode() { return parent->grab_mode(); }
};

class AbstractScrollHandler : public Handler {
    bool have_x, have_y;
    float last_x, last_y;
    Time last_t;
    float offset_x, offset_y;
    Glib::ustring str;
    int orig_x, orig_y;

protected:
    AbstractScrollHandler() : have_x(false), have_y(false), last_x(0.0), last_y(0.0), last_t(0), offset_x(0.0),
                              offset_y(0.0) {
        if (!prefs.move_back.get() || (xstate->current_dev && xstate->current_dev->absolute))
            return;
        Window dummy1, dummy2;
        int dummy3, dummy4;
        unsigned int dummy5;
        XQueryPointer(dpy, ROOT, &dummy1, &dummy2, &orig_x, &orig_y, &dummy3, &dummy4, &dummy5);
    }

    virtual void fake_wheel(int b1, int n1, int b2, int n2) {
        for (int i = 0; i < n1; i++)
            xstate->fake_click(b1);
        for (int i = 0; i < n2; i++)
            xstate->fake_click(b2);
    }

    static float curve(float v) {
        return v * exp(log(abs(v)) / 3);
    }

protected:
    void move_back() {
        if (!prefs.move_back.get())
            return;
        XTestFakeMotionEvent(dpy, DefaultScreen(dpy), orig_x, orig_y, 0);
    }

public:
    virtual void raw_motion(RTriple e, bool abs_x, bool abs_y) {
        const float dx = abs_x ? (have_x ? e->x - last_x : 0) : e->x;
        const float dy = abs_y ? (have_y ? e->y - last_y : 0) : e->y;

        if (abs_x) {
            last_x = e->x;
            have_x = true;
        }

        if (abs_y) {
            last_y = e->y;
            have_y = true;
        }

        if (!last_t) {
            last_t = e->t;
            return;
        }

        if (e->t == last_t)
            return;

        int dt = e->t - last_t;
        last_t = e->t;

        double factor = (prefs.scroll_invert.get() ? 1.0 : -1.0) * prefs.scroll_speed.get();
        offset_x += factor * curve(dx / dt) * dt / 20.0;
        offset_y += factor * curve(dy / dt) * dt / 10.0;
        int b1 = 0, n1 = 0, b2 = 0, n2 = 0;
        if (abs(offset_x) > 1.0) {
            n1 = static_cast<int>(std::floor(abs(offset_x)));
            if (offset_x > 0) {
                b1 = 7;
                offset_x -= n1;
            } else {
                b1 = 6;
                offset_x += n1;
            }
        }
        if (abs(offset_y) > 1.0) {
            if (abs(offset_y) < 1.0)
                return;
            n2 = static_cast<int>(std::floor(abs(offset_y)));
            if (offset_y > 0) {
                b2 = 5;
                offset_y -= n2;
            } else {
                b2 = 4;
                offset_y += n2;
            }
        }
        if (n1 || n2)
            fake_wheel(b1, n1, b2, n2);
    }
};

class ScrollHandler : public AbstractScrollHandler {
    RModifiers mods;
    bool proximity;

public:
    ScrollHandler(RModifiers mods_) : mods(mods_) {
        proximity = xstate->in_proximity && prefs.proximity.get();
    }

    virtual void raw_motion(RTriple e, bool abs_x, bool abs_y) {
        if (proximity && !xstate->in_proximity) {
            parent->replace_child(nullptr);
            move_back();
        }
        if (!xstate->xinput_pressed.empty())
            AbstractScrollHandler::raw_motion(e, abs_x, abs_y);
    }

    virtual void press_master(guint b, Time t) {
        xstate->fake_core_button(b, false);
    }

    virtual void release(guint b, RTriple e) {
        if ((proximity && xstate->in_proximity) || !xstate->xinput_pressed.empty())
            return;
        parent->replace_child(nullptr);
        move_back();
    }

    virtual std::string name() { return "Scroll"; }
    virtual Grabber::State grab_mode() { return Grabber::RAW; }
};

class ScrollAdvancedHandler : public AbstractScrollHandler {
    RModifiers m;
    guint &rb;

public:
    ScrollAdvancedHandler(RModifiers m_, guint &rb_) : m(m_), rb(rb_) {
    }

    virtual void fake_wheel(int b1, int n1, int b2, int n2) {
        AbstractScrollHandler::fake_wheel(b1, n1, b2, n2);
        rb = 0;
    }

    virtual void release(guint b, RTriple e) {
        Handler *p = parent;
        p->replace_child(nullptr);
        p->release(b, e);
        move_back();
    }

    virtual void press(guint b, RTriple e) {
        Handler *p = parent;
        p->replace_child(nullptr);
        p->press(b, e);
        move_back();
    }

    virtual std::string name() { return "ScrollAdvanced"; }
    virtual Grabber::State grab_mode() { return Grabber::RAW; }
};

class AdvancedStrokeActionHandler : public Handler {
    RStroke s;

public:
    AdvancedStrokeActionHandler(RStroke s_, RTriple e) : s(s_) {
    }

    virtual void press(guint b, RTriple e) {
        if (stroke_action) {
            s->button = b;
            (*stroke_action)(s);
        }
    }

    virtual void release(guint b, RTriple e) {
        if (stroke_action)
            (*stroke_action)(s);
        if (xstate->xinput_pressed.empty())
            parent->replace_child(nullptr);
    }

    virtual std::string name() { return "InstantStrokeAction"; }
    virtual Grabber::State grab_mode() { return Grabber::NONE; }
};

class AdvancedHandler : public Handler {
    RTriple e;
    guint remap_from, remap_to;
    Time click_time;
    guint replay_button;
    RTriple replay_orig;
    std::map<guint, RAction> as;
    std::map<guint, RRanking> rs;
    std::map<guint, RModifiers> mods;
    RModifiers sticky_mods;
    guint button1, button2;
    RPreStroke replay;
    bool isRockerLeft, isRockerRight;

    void show_ranking(guint b, RTriple e) {
        if (!rs.count(b))
            return;
        Ranking::queue_show(rs[b], e);
        rs.erase(b);
    }

    AdvancedHandler(RStroke s, RTriple e_, guint b1, guint b2, RPreStroke replay_) : e(e_), remap_from(0), remap_to(0),
        click_time(0), replay_button(0),
        button1(b1), button2(b2), replay(replay_), isRockerLeft(false), isRockerRight(false) {
        if (s) {
            isRockerLeft = s->isRockerLeft;
            isRockerRight = s->isRockerRight;
            actions.get_action_list(grabber->current_class->get())->handle_advanced(s, as, rs, b1, b2);
        }
    }

public:
    static Handler *create(RStroke s, RTriple e, guint b1, guint b2, RPreStroke replay) {
        if (stroke_action && s) {
            return new AdvancedStrokeActionHandler(s, e);
        }
        return new AdvancedHandler(s, e, b1, b2, replay);
    }

    virtual void init() {
        grabber->suspend();
        if (replay && !replay->empty()) {
            const bool replay_first = !as.count(button2);
            auto i = replay->begin();
            if (replay_first) {
                press(button2 ? button2 : button1, *i);
            }
            while (i != replay->end()) {
                motion(*i++);
            }
            if (!replay_first) {
                press(button2 ? button2 : button1, e);
            }
        } else {
            press(button2 ? button2 : button1, e);
        }
        grabber->resume();
        replay.reset();
    }

    virtual void press(guint b, RTriple e) {
        if (isRockerLeft) {
            const RAction act = as[button1];
            mods[button1] = act->prepare();
            if (IS_KEY(act)) {
                if (mods_equal(sticky_mods, mods[button1]))
                    mods[button1] = sticky_mods;
                else
                    sticky_mods = mods[button1];
            } else
                sticky_mods.reset();
            act->run();
            return;
        }
        if (isRockerRight) {
            const RAction act = as[button1];
            mods[button1] = act->prepare();
            if (IS_KEY(act)) {
                if (mods_equal(sticky_mods, mods[button1]))
                    mods[button1] = sticky_mods;
                else
                    sticky_mods = mods[button1];
            } else
                sticky_mods.reset();
            act->run();
            return;
        }
        if (xstate->current_dev->master) {
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, 0);
        }
        click_time = 0;
        if (remap_to) {
            xstate->fake_core_button(remap_to, false);
        }
        remap_from = 0;
        remap_to = 0;
        replay_button = 0;
        const guint bb = b == button1 ? button2 : b;
        show_ranking(bb, e);
        if (!as.count(bb)) {
            sticky_mods.reset();
            if (xstate->current_dev->master) {
                if (b > 0) {
                    XTestFakeButtonEvent(dpy, b, true, CurrentTime);
                }
            }
            return;
        }
        const RAction act = as[bb];
        if (IS_SCROLL(act)) {
            click_time = e->t;
            replay_button = b;
            replay_orig = e;
            RModifiers m = act->prepare();
            sticky_mods.reset();
            return replace_child(new ScrollAdvancedHandler(m, replay_button));
        }
        if (IS_IGNORE(act)) {
            click_time = e->t;
            replay_button = b;
            replay_orig = e;
        }
        IF_BUTTON(act, b2) {
            // This is kind of a hack:  Store modifiers in
            // sticky_mods, so that they are automatically released
            // on the next press
            sticky_mods = act->prepare();
            remap_from = b;
            remap_to = b2;
            xstate->fake_core_button(b2, true);
            return;
        }
        mods[b] = act->prepare();
        if (IS_KEY(act)) {
            if (mods_equal(sticky_mods, mods[b]))
                mods[b] = sticky_mods;
            else
                sticky_mods = mods[b];
        } else
            sticky_mods.reset();
        act->run();
    }

    virtual void motion(RTriple e) {
        if (replay_button && std::hypot(replay_orig->x - e->x, replay_orig->y - e->y) > 16) {
            replay_button = 0;
        }
        if (xstate->current_dev->master) {
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, CurrentTime);
        }
    }

    virtual void release(guint b, RTriple e) {
        if (isRockerLeft || isRockerRight) {
            sticky_mods.reset();
            mods.clear();
            return parent->replace_child(nullptr);
        }
        if (xstate->current_dev->master) {
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, CurrentTime);
        }
        if (remap_to) {
            xstate->fake_core_button(remap_to, false);
        }
        const guint bb = (b == button1) ? button2 : b;
        if (!as.count(bb)) {
            sticky_mods.reset();
            if (xstate->current_dev->master) {
                XTestFakeButtonEvent(dpy, b, false, CurrentTime);
            }
        }
        if (xstate->xinput_pressed.empty()) {
            if (e->t < click_time + 250 && b == replay_button) {
                sticky_mods.reset();
                mods.clear();
                xstate->fake_click(b);
            }
            return parent->replace_child(nullptr);
        }
        replay_button = 0;
        mods.erase((b == button1) ? button2 : b);
        if (remap_from) {
            sticky_mods.reset();
        }
        remap_from = 0;
    }

    virtual std::string name() { return "Advanced"; }
    virtual Grabber::State grab_mode() { return Grabber::NONE; }
};

static void get_timeouts(TimeoutType type, int *init, int *final) {
    switch (type) {
        case TimeoutOff:
            *init = 0;
            *final = 0;
            break;
        case TimeoutConservative:
            *init = 750;
            *final = 750;
            break;
        case TimeoutDefault:
            *init = 250;
            *final = 250;
            break;
        case TimeoutMedium:
            *init = 100;
            *final = 100;
            break;
        case TimeoutAggressive:
            *init = 50;
            *final = 75;
            break;
        case TimeoutFlick:
            *init = 30;
            *final = 50;
            break;
        default: ;
    }
}

class StrokeHandler : public Handler, public sigc::trackable {
    guint button;
    guint trigger;
    RPreStroke cur;
    bool is_gesture;
    bool drawing;
    RTriple last, orig;
    bool use_timeout;
    int init_timeout, final_timeout, radius, main_radius;
    bool isRockerLeft, isRockerRight;

    struct Connection {
        sigc::connection c;
        double dist;

        Connection(StrokeHandler *parent, double dist_, int to) : dist(dist_) {
            c = Glib::signal_timeout().connect(mem_fun(*parent, &StrokeHandler::timeout), to);
        }

        ~Connection() { c.disconnect(); }
    };

    typedef boost::shared_ptr<Connection> RConnection;
    sigc::connection init_connection;
    std::vector<RConnection> connections;

    RStroke finish(guint b) {
        if (b != 0 && (!is_gesture || grabber->is_instant(button)) && ((b == Button1 && button != Button1) || (b == Button3 && button != Button3))) {
            isRockerLeft = b == Button1;
            isRockerRight = b == Button3;
            trace->end();
            XFlush(dpy);
            RPreStroke c = cur;
            c.reset(new PreStroke);
            auto s = Stroke::create(*c, b, b, xstate->modifiers, true, isRockerLeft, isRockerRight);
            return s;
        }

        trace->end();
        XFlush(dpy);
        RPreStroke c = cur;
        if (!is_gesture || grabber->is_instant(button)) {
            c.reset(new PreStroke);
        }
        return Stroke::create(*c, trigger, b, xstate->modifiers, false, false, false);
    }

    bool timeout() {
        if (verbosity >= 2) {
            printf("Aborting stroke...\n");
        }
        trace->end();
        RPreStroke c = cur;
        if (!is_gesture) {
            c.reset(new PreStroke);
        }
        RStroke s;
        if (prefs.timeout_gestures.get() || grabber->is_click_hold(button)) {
            s = Stroke::create(*c, trigger, 0, xstate->modifiers, true, false, false);
        }
        parent->replace_child(AdvancedHandler::create(s, last, button, 0, cur));
        XFlush(dpy);
        return false;
    }

    void do_instant() {
        PreStroke ps;
        RStroke s = Stroke::create(ps, trigger, button, xstate->modifiers, false, false, false);
        parent->replace_child(AdvancedHandler::create(s, orig, button, button, cur));
    }

    bool expired(RConnection c, double dist) {
        c->dist -= dist;
        return c->dist < 0;
    }

protected:
    void abort_stroke() {
        parent->replace_child(AdvancedHandler::create(RStroke(), last, button, 0, cur));
    }

    virtual void motion(RTriple e) {
        cur->add(e);
        const float dist = std::hypot(e->x - orig->x, e->y - orig->y);
        if ((!is_gesture && dist > 16) || button == Button1) {
            if ((use_timeout && !final_timeout) || button == Button1) {
                return abort_stroke();
            }
            init_connection.disconnect();
            is_gesture = true;
        }
        if (!drawing && button != Button1 && dist > 4 && (!use_timeout || final_timeout)) {
            drawing = true;
            bool first = true;
            for (const auto &i: *cur) {
                Trace::Point p{};
                p.x = i->x;
                p.y = i->y;
                if (first) {
                    trace->start(p);
                    first = false;
                } else {
                    trace->draw(p);
                }
            }
        } else if (drawing) {
            Trace::Point p{};
            p.x = e->x;
            p.y = e->y;
            trace->draw(p);
        }
        if (use_timeout && is_gesture) {
            int *rad = button == Button1 ? &main_radius : &radius;
            connections.erase(remove_if(connections.begin(), connections.end(),
                                        bind(mem_fun(*this, &StrokeHandler::expired),
                                                   std::hypot(e->x - last->x, e->y - last->y))), connections.end());
            connections.push_back(boost::make_shared<Connection>(this, *rad, final_timeout));
        }
        last = e;
    }

    virtual void press(guint b, RTriple e) {
        const RStroke s = finish(b);
        parent->replace_child(AdvancedHandler::create(s, e, button, b, cur));
    }

    virtual void release(guint b, RTriple e) {
        const RStroke s = finish(0);

        if (experimental) {
            //button 1 should be treated as trigger 0, other trigger buttons copied from the event button b itself
            if (b > 100) {
                b = b - 100;
                s->trigger = prefs.button.ref().button - 1;
            }
        }

        if (prefs.move_back.get())
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), orig->x, orig->y, CurrentTime);
        else
            XTestFakeMotionEvent(dpy, DefaultScreen(dpy), e->x, e->y, CurrentTime);

        if (stroke_action) {
            (*stroke_action)(s);
            return parent->replace_child(nullptr);
        }
        RRanking ranking;
        RAction act = actions.get_action_list(grabber->current_class->get())->handle(s, ranking);
        if (!IS_CLICK(act))
            Ranking::queue_show(ranking, e);
        if (!act) {
            XkbBell(dpy, None, 0, None);
            return parent->replace_child(nullptr);
        }

        const RModifiers mods = act->prepare();
        if (IS_CLICK(act)) {
            act = Button::create(static_cast<Gdk::ModifierType>(0), b);
        } else
            IF_BUTTON(act, bb) {
                return parent->replace_child(new ButtonHandler(mods, bb));
            }
        if (IS_IGNORE(act)) {
            return parent->replace_child(new IgnoreHandler(mods));
        }
        if (IS_SCROLL(act)) {
            return parent->replace_child(new ScrollHandler(mods));
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", static_cast<int>(orig->x));
        setenv("EASYSTROKE_X1", buf, 1);
        snprintf(buf, sizeof(buf), "%d", static_cast<int>(orig->y));
        setenv("EASYSTROKE_Y1", buf, 1);
        snprintf(buf, sizeof(buf), "%d", static_cast<int>(e->x));
        setenv("EASYSTROKE_X2", buf, 1);
        snprintf(buf, sizeof(buf), "%d", static_cast<int>(e->y));
        setenv("EASYSTROKE_Y2", buf, 1);
        act->run();
        unsetenv("EASYSTROKE_X1");
        unsetenv("EASYSTROKE_Y1");
        unsetenv("EASYSTROKE_X2");
        unsetenv("EASYSTROKE_Y2");
        parent->replace_child(nullptr);
    }

public:
    StrokeHandler(guint b, RTriple e) : button(b),
                                        trigger(grabber->get_default_button() == static_cast<int>(b) ? 0 : b),
                                        is_gesture(false),
                                        drawing(false),
                                        last(e),
                                        orig(e),
                                        init_timeout(prefs.init_timeout.get()),
                                        final_timeout(prefs.final_timeout.get()),
                                        radius(16),
                                        main_radius(8),
                                        isRockerLeft(false),
                                        isRockerRight(false) {
        const std::map<std::string, TimeoutType> &dt = prefs.device_timeout.ref();
        const auto j = dt.find(xstate->current_dev->name);
        if (j != dt.end())
            get_timeouts(j->second, &init_timeout, &final_timeout);
        else
            get_timeouts(prefs.timeout_profile.get(), &init_timeout, &final_timeout);
        use_timeout = init_timeout;
    }

    virtual void init() {
        int *rad = button == Button1 ? &main_radius : &radius;
        init_timeout = button == Button1 ? init_timeout / 2 : init_timeout;
        if (grabber->is_instant(button)) {
            return do_instant();
        }
        if (grabber->is_click_hold(button)) {
            use_timeout = true;
            init_timeout = 500;
            final_timeout = 0;
        }
        cur = PreStroke::create();
        cur->add(orig);
        if (!use_timeout) {
            return;
        }
        if (final_timeout && final_timeout < 32 && *rad < 16 * 32 / final_timeout) {
            *rad = 16 * 32 / final_timeout;
            final_timeout = final_timeout * *rad / 16;
        }
        printf("Resolved rad %d vs %d vs %d\n", *rad, init_timeout, final_timeout);
        init_connection = Glib::signal_timeout().connect(mem_fun(*this, &StrokeHandler::timeout), init_timeout);
    }

    ~StrokeHandler() { trace->end(); }
    virtual std::string name() { return "Stroke"; }
    virtual Grabber::State grab_mode() { return Grabber::NONE; }
};

class IdleHandler : public Handler {
protected:
    virtual void init() {
        xstate->update_core_mapping();
    }

    virtual void press(guint b, RTriple e) {
        if (current_app_window.get()) {
            printf("Activating current window\n");
            XState::activate_window(current_app_window.get(), e->t);
        }
        replace_child(new StrokeHandler(b, e));
    }

public:
    IdleHandler(XState *xstate_) {
        xstate = xstate_;
    }

    virtual ~IdleHandler() {
        XUngrabKey(dpy, XKeysymToKeycode(dpy,XK_Escape), AnyModifier, ROOT);
    }

    virtual std::string name() { return "Idle"; }
    virtual Grabber::State grab_mode() { return Grabber::BUTTON; }
};

class SelectHandler : public Handler {
    virtual void press_master(guint b, Time t) {
        parent->replace_child(new WaitForPongHandler);
        xstate->ping();
        xstate->queue(sigc::ptr_fun(&gtk_main_quit));
    }

public:
    virtual std::string name() { return "Select"; }
    virtual Grabber::State grab_mode() { return Grabber::SELECT; }
};

void XState::select() {
    win->get_window().get_window()->lower();
    handler->top()->replace_child(new SelectHandler);
}

std::string XState::select_window() {
    queue(sigc::mem_fun(this, &XState::select));
    gtk_main();
    win->get_window().raise();
    return grabber->current_class->get();
}

void XState::init_randr_tracking() {
    Window root = DefaultRootWindow(dpy);
    if (!XRRQueryExtension(dpy, &randr_event_base, &randr_error_base)) {
        if (verbosity >= 2)
            printf("RandR extension unavailable; pointer barriers will not auto-refresh\n");
        randr_event_base = -1;
        return;
    }

    XRRSelectInput(dpy, root, RROutputChangeNotifyMask | RRCrtcChangeNotifyMask | RRScreenChangeNotifyMask);
}

void XState::update_screen_metrics() {
    screenWidth = DisplayWidth(dpy, 0);
    screenHeight = DisplayHeight(dpy, 0);

    int center_x = 0, center_y = 0;
    if (!get_primary_monitor_center(&center_x, &center_y)) {
        if (verbosity >= 2)
            printf("Falling back to derived center because primary monitor lookup failed\n");
        center_x = 2 * (screenWidth / 3) + (screenWidth / 3) / 2;
        center_y = screenHeight / 2;
    }

    int boxSize = 250; // Synergy-controlled bounding box size

    x_min = center_x - (boxSize / 2);
    x_max = center_x + (boxSize / 2);
    y_min = center_y - (boxSize / 2);
    y_max = center_y + (boxSize / 2);
    if (verbosity >= 3)
        printf("Screen size: %d x %d\nCenter: %d x %d\nDims: x_min=%d x_max=%d y_min=%d y_max=%d\n", screenWidth, screenHeight, center_x, center_y, x_min, x_max, y_min, y_max);
    constexpr int offset = 2;
    screenTop = 32;
    screenBot = screenHeight - offset;
    screenLeft = offset;
    screenRight = screenWidth - offset;

    // Reset state tracking so new geometry doesn't keep stale assumptions.
    prevState = NONE;
    transitionCount = 0;
    transitionOutCount = 0;
}

void XState::destroy_pointer_barriers() {
    if (right)
        XFixesDestroyPointerBarrier(dpy, right);
    if (left)
        XFixesDestroyPointerBarrier(dpy, left);
    if (top)
        XFixesDestroyPointerBarrier(dpy, top);
    if (bottom)
        XFixesDestroyPointerBarrier(dpy, bottom);

    right = left = top = bottom = 0;
}

void XState::rebuild_pointer_barriers() {
    destroy_pointer_barriers();

    const Window root = DefaultRootWindow(dpy);
    right = XFixesCreatePointerBarrier(dpy, root, screenRight, screenTop, screenRight, screenBot, 0, 0, nullptr);
    left = XFixesCreatePointerBarrier(dpy, root, screenLeft, screenTop, screenLeft, screenBot, 0, 0, nullptr);
    top = XFixesCreatePointerBarrier(dpy, root, screenLeft, screenTop, screenRight, screenTop, 0, 0, nullptr);
    bottom = XFixesCreatePointerBarrier(dpy, root, screenLeft, screenBot, screenRight, screenBot, 0, 0, nullptr);

    if (!right || !left || !top || !bottom) {
        printf("One or more pointer barriers failed to create; Synergy detection may be degraded (l=%lu r=%lu t=%lu b=%lu)\n",
               static_cast<unsigned long>(left), static_cast<unsigned long>(right), static_cast<unsigned long>(top),
               static_cast<unsigned long>(bottom));
    }

    XFlush(dpy);
}

void XState::handle_randr_event(XEvent &ev) {
    if (randr_event_base < 0)
        return;

    const int subtype = ev.type - randr_event_base;
    if (subtype == RRScreenChangeNotify) {
        XRRUpdateConfiguration(&ev);
        update_screen_metrics();
        rebuild_pointer_barriers();
        return;
    }

    if (subtype == RRNotify) {
        auto *rr_ev = reinterpret_cast<XRRNotifyEvent *>(&ev);
        switch (rr_ev->subtype) {
            case RRNotify_OutputChange:
            case RRNotify_CrtcChange:
                update_screen_metrics();
                rebuild_pointer_barriers();
                break;
        }
    }
}

XState::XState() : current_dev(nullptr),
                   in_proximity(false),
                   accepted(true),
                   modifiers(0),
                   randr_event_base(-1),
                   randr_error_base(0) {
    int n, opcode, event, error;
    char **ext = XListExtensions(dpy, &n);
    for (int i = 0; i < n; i++)
        if (XQueryExtension(dpy, ext[i], &opcode, &event, &error))
            opcodes[opcode] = ext[i];
    XFreeExtensionList(ext);
    oldHandler = XSetErrorHandler(xErrorHandler);
    oldIOHandler = XSetIOErrorHandler(xIOErrorHandler);
    ping_window = XCreateSimpleWindow(dpy, ROOT, 0, 0, 1, 1, 0, 0, 0);
    handler = new IdleHandler(this);
    handler->init();
    top = bottom = left = right = 0;
    init_randr_tracking();
    update_screen_metrics();
    rebuild_pointer_barriers();
}

void XState::request_control_state(bool wants_controlled, const char *reason) {
    target_controlled = wants_controlled;
    if (reason)
        pending_control_reason = reason;

    if (control_timeout.connected())
        return;

    // Debounce grab transitions away from the XI handler.
    control_timeout = Glib::signal_timeout().connect(sigc::mem_fun(*this, &XState::apply_control_state), 75);
}

bool XState::apply_control_state() {
    if (control_timeout.connected())
        control_timeout.disconnect();

    if (controlled == target_controlled)
        return false;

    controlled = target_controlled;
    if (verbosity >= 3) {
        printf("Applying %s control (%s)\n", controlled ? "Synergy" : "local",
               pending_control_reason.empty() ? "no reason provided" : pending_control_reason.c_str());
    }

    if (controlled)
        grabber->suspend();
    else
        grabber->resume();
    return false;
}

bool XState::drain_pending_events_batch(int max_events) {
    int processed = 0;
    while (processed < max_events && XPending(dpy)) {
        try {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (!grabber->handle(ev)) {
                handle_event(ev);
            }
        } catch (GrabFailedException &e) {
            printf(_("Error: %s\n"), e.what());
            bail_out();
        }
        processed++;
    }

    if (processed == max_events && XPending(dpy) && verbosity >= 2)
        printf("X queue congested; scheduling idle drain after processing %d events\n", processed);

    return XPending(dpy);
}

bool XState::drain_pending_events() {
    bool more = drain_pending_events_batch(64);
    if (!more && drain_idle.connected())
        drain_idle.disconnect();
    return more;
}

const char *XState::state_name[4] = { "None", "Outside", "Bound", "Center"};

void XState::run_action(RAction act) {
    printf("run_action\n");
    const RModifiers mods = act->prepare();
    IF_BUTTON(act, b)
        return handler->replace_child(new ButtonHandler(mods, b));
    if (IS_IGNORE(act))
        return handler->replace_child(new IgnoreHandler(mods));
    if (IS_SCROLL(act))
        return handler->replace_child(new ScrollHandler(mods));
    act->run();
}

bool XState::get_primary_monitor_center(int *center_x, int *center_y) {
    if (!center_x || !center_y) return false; // Ensure valid pointers

    // Get the root window
    Window root = DefaultRootWindow(dpy);

    // Get screen resources
    XRRScreenResources *screenRes = XRRGetScreenResourcesCurrent(dpy, root);
    if (!screenRes) {
        printf("Failed to get screen resources\n");
        return false;
    }

    // Get primary monitor output
    RROutput primary_output = XRRGetOutputPrimary(dpy, root);
    if (primary_output == None) {
        printf("No primary monitor set, using first available monitor\n");
        primary_output = screenRes->outputs[0]; // Fallback to the first output
    }

    // Get output info
    XRROutputInfo *outputInfo = XRRGetOutputInfo(dpy, screenRes, primary_output);
    if (!outputInfo || outputInfo->connection == RR_Disconnected) {
        printf("Primary monitor not connected\n");
        XRRFreeScreenResources(screenRes);
        return false;
    }

    // Get CRTC Info
    XRRCrtcInfo *crtcInfo = XRRGetCrtcInfo(dpy, screenRes, outputInfo->crtc);
    if (!crtcInfo) {
        printf("Failed to get CRTC info\n");
        XRRFreeOutputInfo(outputInfo);
        XRRFreeScreenResources(screenRes);
        return false;
    }

    // Calculate monitor center
    *center_x = crtcInfo->x + crtcInfo->width / 2;
    *center_y = crtcInfo->y + crtcInfo->height / 2;

    // Cleanup
    XRRFreeCrtcInfo(crtcInfo);
    XRRFreeOutputInfo(outputInfo);
    XRRFreeScreenResources(screenRes);

    return true; // Success
}

