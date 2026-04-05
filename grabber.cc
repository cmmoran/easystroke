/*
 * Copyright (c) 2008-2009, Thomas Jaeger <ThJaeger@gmail.com>
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
#include "grabber.h"
#include "main.h"
#include "prefs.h"
#include <X11/extensions/XTest.h>
#include <xorg/xserver-properties.h>
#include <X11/cursorfont.h>
#include <X11/Xutil.h>
#include <glibmm/i18n.h>

extern Source<bool> disabled;
extern Source<Window> current_app_window;
extern Source<bool> recording;

Grabber *grabber = nullptr;

static unsigned int ignore_mods[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
static XIEventMask device_mask;
static XIEventMask raw_mask;

template<class X1, class X2>
class BiMap {
    std::map<X1, X2> map1;
    std::map<X2, X1> map2;

public:
    void erase1(X1 x1) {
        auto i1 = map1.find(x1);
        if (i1 == map1.end())
            return;
        map2.erase(i1->second);
        map1.erase(i1->first);
    }

    void erase2(X2 x2) {
        auto i2 = map2.find(x2);
        if (i2 == map2.end())
            return;
        map1.erase(i2->second);
        map2.erase(i2->first);
    }

    void pop(X1 &x1, X2 &x2) {
        typename std::map<X1, X2>::reverse_iterator i1 = map1.rbegin();
        x1 = i1->first;
        x2 = i1->second;
        map2.erase(i1->second);
        map1.erase(i1->first);
    }

    void add(X1 x1, X2 x2) {
        erase1(x1);
        erase2(x2);
        map1[x1] = x2;
        map2[x2] = x1;
    }

    bool empty() { return map1.empty(); }
    bool contains1(X1 x1) { return map1.find(x1) != map1.end(); }
    bool contains2(X2 x2) { return map2.find(x2) != map2.end(); }
    X2 find1(X1 x1) { return map1.find(x1)->second; }
    X1 find2(X2 x2) { return map2.find(x2)->second; }
};

Atom XAtom::operator*() {
    if (!atom)
        atom = XInternAtom(dpy, name, False);
    return atom;
}

BiMap<Window, Window> frame_win;
BiMap<Window, Window> frame_child;
XAtom _NET_FRAME_WINDOW("_NET_FRAME_WINDOW");
XAtom _NET_WM_STATE("_NET_WM_STATE");
XAtom _NET_WM_STATE_HIDDEN("_NET_WM_STATE_HIDDEN");
XAtom _NET_ACTIVE_WINDOW("_NET_ACTIVE_WINDOW");

std::list<Window> minimized;
unsigned int minimized_n = 0;

void get_frame(Window w) {
    Window frame = xstate->get_window(w, *_NET_FRAME_WINDOW);
    if (!frame) {
        return;
    }
    frame_win.add(frame, w);
}

Children::Children(Window w) : parent(w) {
    XSelectInput(dpy, parent, SubstructureNotifyMask);
    unsigned int n;
    Window dummyw1, dummyw2, *ch;
    XQueryTree(dpy, parent, &dummyw1, &dummyw2, &ch, &n);
    for (unsigned int i = 0; i < n; i++)
        add(ch[i]);
    XFree(ch);
}

bool Children::handle(XEvent &ev) {
    switch (ev.type) {
        case CreateNotify:
            if (ev.xcreatewindow.parent != parent) {
                return false;
            }
            add(ev.xcreatewindow.window);
            return true;
        case DestroyNotify:
            frame_child.erase1(ev.xdestroywindow.window);
            frame_child.erase2(ev.xdestroywindow.window);
            minimized.remove(ev.xdestroywindow.window);
            destroy(ev.xdestroywindow.window);
            return true;
        case ReparentNotify:
            if (ev.xreparent.event != parent) {
                return false;
            }
            if (ev.xreparent.window == parent) {
                return false;
            }
            if (ev.xreparent.parent == parent) {
                add(ev.xreparent.window);
            } else {
                remove(ev.xreparent.window);
            }
            return true;
        case PropertyNotify:
            if (ev.xproperty.atom == *_NET_FRAME_WINDOW) {
                if (ev.xproperty.state == PropertyDelete) {
                    frame_win.erase1(ev.xproperty.window);
                }
                if (ev.xproperty.state == PropertyNewValue) {
                    get_frame(ev.xproperty.window);
                }
                return true;
            }
            if (ev.xproperty.atom == *_NET_WM_STATE) {
                if (ev.xproperty.state == PropertyDelete) {
                    minimized.remove(ev.xproperty.window);
                    return true;
                }
                const bool was_hidden = std::find(minimized.begin(), minimized.end(), ev.xproperty.window) != minimized.end();
                const bool is_hidden = XState::has_atom(ev.xproperty.window, *_NET_WM_STATE, *_NET_WM_STATE_HIDDEN);
                if (was_hidden && !is_hidden) {
                    minimized.remove(ev.xproperty.window);
                }
                if (is_hidden && !was_hidden) {
                    minimized.push_back(ev.xproperty.window);
                }
                return true;
            }
            return false;
        default:
            return false;
    }
}

void Children::add(const Window w) {
    if (!w)
        return;

    XSelectInput(dpy, w, EnterWindowMask | PropertyChangeMask);
    get_frame(w);
}

void Children::remove(Window w) {
    XSelectInput(dpy, w, 0);
    destroy(w);
}

void Children::destroy(Window w) {
    frame_win.erase1(w);
    frame_win.erase2(w);
}

static void activate(Window w, Time t) {
    XClientMessageEvent ev;
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = *_NET_ACTIVE_WINDOW;
    ev.format = 32;
    ev.data.l[0] = 0; // 1 app, 2 pager
    ev.data.l[1] = t;
    ev.data.l[2] = 0;
    ev.data.l[3] = 0;
    ev.data.l[4] = 0;
    XSendEvent(dpy, ROOT, False, SubstructureNotifyMask | SubstructureRedirectMask, reinterpret_cast<XEvent *>(&ev));
}

static XIEventMask initialize_xi_mask(const int mask_event[], const int mask_event_count) {
    XIEventMask mask;
    mask.deviceid = XIAllDevices;
    mask.mask_len = XIMaskLen(XI_LASTEVENT);
    mask.mask = new unsigned char[mask.mask_len];
    memset(mask.mask, 0, mask.mask_len);

    for (int i = 0; i < mask_event_count; ++i) {
        XISetMask(mask.mask, mask_event[i]);
    }

    return mask;
}

std::string get_wm_class(Window w) {
    if (!w) {
        return "";
    }
    XClassHint ch;
    if (!XGetClassHint(dpy, w, &ch)) {
        return "";
    }
    const char *res_name = ch.res_name;
    const char *res_class = ch.res_class;
    std::string ans = res_name ? res_name : "";
    if (res_name) {
        XFree(ch.res_name);
    }
    if (res_class) {
        XFree(ch.res_class);
    }
    return ans;
}

class IdleNotifier : public Base {
    sigc::slot<void> f;
    void run() {
        if (!f.empty()) {
            f();
        }
    }

public:
    IdleNotifier(sigc::slot<void> f_) : f(f_) {
    }

    virtual void notify() { xstate->queue(sigc::mem_fun(*this, &IdleNotifier::run)); }
};

void Grabber::unminimize() {
    if (minimized.empty())
        return;
    Window w = minimized.back();
    minimized.pop_back();
    activate(w, CurrentTime);
}

const char *Grabber::state_name[4] = {"None", "Button", "Select", "Raw"};

Grabber::Grabber() : children(ROOT) {
    suspend();
    cursor_select = XCreateFontCursor(dpy, XC_crosshair);
    init_xi();
    prefs.excluded_devices.connect(new IdleNotifier(sigc::mem_fun(*this, &Grabber::update)));
    prefs.button.connect(new IdleNotifier(sigc::mem_fun(*this, &Grabber::update)));
    current_class = fun(&get_wm_class, current_app_window);
    current_class->connect(new IdleNotifier(sigc::mem_fun(*this, &Grabber::update)));
    recording.connect(new IdleNotifier(sigc::mem_fun(*this, &Grabber::update)));
    disabled.connect(new IdleNotifier(sigc::mem_fun(*this, &Grabber::set)));
    update();
    resume();
}

Grabber::~Grabber() {
    XFreeCursor(dpy, cursor_select);
}

bool Grabber::init_xi() {
    /* XInput Extension available? With PointerBarrier we need 2.3 */
    int major = 2, minor = 3;
    if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error) ||
        XIQueryVersion(dpy, &major, &minor) == BadRequest ||
        major < 2) {
        printf("Error: This version of easystroke needs an XInput 2.3-aware X server.\n"
            "Please downgrade to easystroke 0.4.x or upgrade your X server to 1.7.\n");
        exit(EXIT_FAILURE);
    }

    int n;
    XIDeviceInfo *info = XIQueryDevice(dpy, XIAllDevices, &n);
    if (!info) {
        printf("Warning: No XInput devices available\n");
        return false;
    }

    for (int i = 0; i < n; i++)
        new_device(info + i);
    XIFreeDeviceInfo(info);
    prefs.excluded_devices.connect(new IdleNotifier(sigc::mem_fun(*this, &Grabber::update_excluded)));
    update_excluded();
    xi_grabbed = false;
    set();

    if (xi_devs.empty()) {
        printf("Error: No suitable XInput devices found\n");
        exit(EXIT_FAILURE);
    }

    constexpr int device_mask_events[] = {XI_ButtonPress, XI_ButtonRelease, XI_Motion};
    device_mask = initialize_xi_mask(device_mask_events, 3);

    constexpr int raw_mask_events[] = {XI_ButtonPress, XI_ButtonRelease, XI_RawMotion};
    raw_mask = initialize_xi_mask(raw_mask_events, 3);

    const int global_mask_events[] = {XI_HierarchyChanged, XI_Motion, XI_RawMotion, XI_BarrierHit, XI_BarrierLeave};
    XIEventMask global_mask = initialize_xi_mask(global_mask_events, 5);

    XISelectEvents(dpy, ROOT, &global_mask, 1);

    return true;
}

bool Grabber::hierarchy_changed(XIHierarchyEvent *event) {
    bool changed = false;
    for (int i = 0; i < event->num_info; i++) {
        const XIHierarchyInfo *info = event->info + i;
        if (info->flags & XISlaveAdded) {
            int n;
            XIDeviceInfo *dev_info = XIQueryDevice(dpy, info->deviceid, &n);
            if (!dev_info) {
                continue;
            }
            new_device(dev_info);
            XIFreeDeviceInfo(dev_info);
            update_excluded();
            changed = true;
        } else if (info->flags & XISlaveRemoved) {
            if (verbosity >= 1) {
                printf("Device %d removed.\n", info->deviceid);
            }
            xstate->remove_device(info->deviceid);
            xi_devs.erase(info->deviceid);
            changed = true;
        } else if (info->flags & (XISlaveAttached | XISlaveDetached)) {
            auto j = xi_devs.find(info->deviceid);
            if (j != xi_devs.end()) {
                j->second->master = info->attachment;
            }
        }
    }
    return changed;
}

void Grabber::update_excluded() {
    suspend();
    for (const auto & xi_dev : xi_devs)
        xi_dev.second->active = !prefs.excluded_devices.ref().count(xi_dev.second->name);
    resume();
}

bool is_xtest_device(int dev) {
    static XAtom XTEST(XI_PROP_XTEST_DEVICE);
    Atom type;
    int format;
    unsigned long num_items, bytes_after;
    unsigned char *data;
    if (Success != XIGetProperty(dpy, dev, *XTEST, 0, 1, False, XA_INTEGER,
                                 &type, &format, &num_items, &bytes_after, &data))
        return false;
    const bool ret = num_items && format == 8 && *reinterpret_cast<int8_t *>(data);
    XFree(data);
    return ret;
}

void Grabber::new_device(XIDeviceInfo *info) {
    if (info->use == XIMasterKeyboard)
        return;

    if (is_xtest_device(info->deviceid))
        return;

    for (int j = 0; j < info->num_classes; j++)
        if (info->classes[j]->type == ButtonClass) {
            const auto xi_dev = new XiDevice(info);
            xi_devs[info->deviceid].reset(xi_dev);
            return;
        }
}

Grabber::XiDevice::XiDevice(XIDeviceInfo *info) : absolute(false), active(true), proximity_axis(-1),
                                                                   scale_x(1.0), scale_y(1.0), num_buttons(0) {
    static XAtom PROXIMITY(AXIS_LABEL_PROP_ABS_DISTANCE);
    dev = info->deviceid;
    name = info->name;
    master = info->attachment;
    for (int j = 0; j < info->num_classes; j++) {
        XIAnyClassInfo *dev_class = info->classes[j];
        if (dev_class->type == ButtonClass) {
            const auto *b = reinterpret_cast<XIButtonClassInfo *>(dev_class);
            num_buttons = b->num_buttons;
        } else if (dev_class->type == ValuatorClass) {
            const auto *v = reinterpret_cast<XIValuatorClassInfo *>(dev_class);
            if ((v->number == 0 || v->number == 1) && v->mode != XIModeRelative) {
                absolute = true;
                if (v->number == 0)
                    scale_x = static_cast<double>(DisplayWidth(dpy, DefaultScreen(dpy))) / (v->max - v->min);
                else
                    scale_y = static_cast<double>(DisplayHeight(dpy, DefaultScreen(dpy))) / (v->max - v->min);
            }
            if (v->label == *PROXIMITY)
                proximity_axis = v->number;
        }
    }

    if (verbosity >= 1)
        printf("Opened Device %d ('%s'%s).\n", dev, info->name, absolute ? ": absolute" : "");
}

Grabber::XiDevice *Grabber::get_xi_dev(const int id) {
    const auto i = xi_devs.find(id);
    return i == xi_devs.end() ? nullptr : i->second.get();
}

void Grabber::XiDevice::grab_button(ButtonInfo &bi, bool grab) {
    XIGrabModifiers modifiers[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    int nmods = 0;
    if (bi.button == AnyModifier) {
        nmods = 1;
        modifiers[0].modifiers = XIAnyModifier;
    } else {
        nmods = 4;
        for (int i = 0; i < 4; i++) {
            modifiers[i].modifiers = bi.state ^ ignore_mods[i];
        }
    }
    if (grab) {
        const int status_base = XIGrabButton(dpy, dev, 1, ROOT, None, GrabModeAsync, GrabModeAsync, False, &device_mask, nmods, modifiers);
        if (status_base != Success && verbosity >= 1)
            printf("Warning: XIGrabButton failed for device %d button 1: %d\n", dev, status_base);
        const int status_button = XIGrabButton(dpy, dev, bi.button, ROOT, None, GrabModeAsync, GrabModeAsync, False, &device_mask, nmods, modifiers);
        if (status_button != Success && verbosity >= 1)
            printf("Warning: XIGrabButton failed for device %d button %u: %d\n", dev, bi.button, status_button);
    } else {
        XIUngrabButton(dpy, dev, 1, ROOT, nmods, modifiers);
        XIUngrabButton(dpy, dev, bi.button, ROOT, nmods, modifiers);
        xstate->ungrab(dev);
    }
}

void Grabber::grab_xi(bool grab) {
    if (!xi_grabbed == !grab)
        return;
    xi_grabbed = grab;
    if (!experimental) {
        //standard behaviour; only grab enabled devices
        for (const auto &xi_dev: xi_devs) {
            if (xi_dev.second->active) {
                for (auto &button: buttons) {
                    xi_dev.second->grab_button(button, grab);
                }
            }
        }
    } else {
        //modified behaviour; also grab additional buttons of disabled devices
        for (const auto &xi_dev: xi_devs) {
            for (auto j = buttons.begin(); j != buttons.end(); ++j) {
                if (xi_dev.second->active || j != buttons.begin()) {
                    xi_dev.second->grab_button(*j, grab);
                }
            }
        }
    }
}

void Grabber::XiDevice::grab_device(GrabState grab) {
    if (grab == GrabNo) {
        XIUngrabDevice(dpy, dev, CurrentTime);
        xstate->ungrab(dev);
        return;
    }
    const int status = XIGrabDevice(dpy, dev, ROOT, CurrentTime, None, GrabModeAsync, GrabModeAsync, False,
                                    grab == GrabYes ? &device_mask : &raw_mask);
    if (status != Success && verbosity >= 1)
        printf("Warning: XIGrabDevice failed for device %d mode %d: %d\n", dev, grab, status);
}

void Grabber::grab_xi_devs(GrabState grab) {
    if (xi_devs_grabbed == grab)
        return;
    xi_devs_grabbed = grab;
    for (const auto &xi_dev: xi_devs)
        xi_dev.second->grab_device(grab);
}

void Grabber::set() {
    const bool act = !suspended && ((active && !disabled.get()) || (current != NONE && current != BUTTON));
    grab_xi(act && current != SELECT);
    if (!act) {
        grab_xi_devs(GrabNo);
    } else if (current == NONE) {
        grab_xi_devs(GrabYes);
    } else if (current == RAW) {
        grab_xi_devs(GrabRaw);
    } else {
        grab_xi_devs(GrabNo);
    }
    const State old = grabbed;
    grabbed = act ? current : NONE;
    if (old == grabbed)
        return;
    if (verbosity >= 2)
        printf("grabbing: %s\n", state_name[grabbed]);

    if (old == SELECT) {
        XUngrabPointer(dpy, CurrentTime);
    }

    if (grabbed == SELECT) {
        const int code = XGrabPointer(dpy, ROOT, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, ROOT, cursor_select, CurrentTime);
        if (code != GrabSuccess)
            throw GrabFailedException(code);
    }
}

void Grabber::queue_suspend() {
    xstate->queue(sigc::mem_fun(*this, &Grabber::suspend));
}

void Grabber::queue_resume() {
    xstate->queue(sigc::mem_fun(*this, &Grabber::resume));
}

std::string Grabber::select_window() {
    return xstate->select_window();
}

bool Grabber::is_grabbed(guint b) {
    return std::any_of(buttons.begin(), buttons.end(), [b](const ButtonInfo &bi) { return bi.button == b; });
}

bool Grabber::is_instant(guint b) {
    return std::any_of(buttons.begin(), buttons.end(),
                       [b](const ButtonInfo &bi) { return bi.button == b && bi.instant; });
}

bool Grabber::is_click_hold(guint b) {
    return std::any_of(buttons.begin(), buttons.end(),
                       [b](const ButtonInfo &bi) { return bi.button == b && bi.click_hold; });
}

guint Grabber::get_default_mods(const guint button) {
    for (const auto i: buttons) {
        if (i.button == button) {
            return i.state;
        }
    }
    return AnyModifier;
}

void Grabber::update() {
    ButtonInfo bi = prefs.button.ref();
    active = true;
    if (!recording.get()) {
        const auto i = prefs.exceptions.ref().find(current_class->get());
        if (i != prefs.exceptions.ref().end()) {
            if (i->second)
                bi = *i->second;
            else
                active = false;
        }

        if (prefs.whitelist.get() && !actions.apps.count(current_class->get()))
            active = false;
    }
    const std::vector<ButtonInfo> &extra = prefs.extra_buttons.ref();
    if (grabbed_button == bi && buttons.size() == extra.size() + 1 &&
        std::equal(extra.begin(), extra.end(), ++buttons.begin())) {
        set();
        return;
    }
    suspend();
    grabbed_button = bi;
    buttons.clear();
    buttons.reserve(extra.size() + 1);
    buttons.push_back(bi);
    for (auto i: extra)
        if (!i.overlap(bi))
            buttons.push_back(i);
    resume();
}

// Fuck Xlib
static bool has_wm_state(Window w) {
    if (!w) {
        return false;
    }
    static XAtom WM_STATE("WM_STATE");
    Atom actual_type_return;
    int actual_format_return;
    unsigned long nitems_return;
    unsigned long bytes_after_return;
    unsigned char *prop_return = nullptr;
    if (Success != XGetWindowProperty(dpy, w, *WM_STATE, 0, 2, False, AnyPropertyType, &actual_type_return,
                                      &actual_format_return, &nitems_return, &bytes_after_return, &prop_return))
        return false;
    if (prop_return) {
        XFree(prop_return);
    }
    return actual_format_return == 32 && nitems_return > 0;
}

static bool is_stable_app_window(const Window w) {
    if (!w || w == ROOT) {
        return false;
    }

    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr)) {
        return false;
    }

    if (attr.c_class == InputOnly || attr.override_redirect) {
        return false;
    }

    return true;
}

auto find_wm_state(const Window w) -> Window {
    if (!w)
        return w;
    if (has_wm_state(w))
        return w;
    Window found = None;
    unsigned int n;
    Window dummyw1, dummyw2, *ch;
    if (!XQueryTree(dpy, w, &dummyw1, &dummyw2, &ch, &n))
        return None;
    for (unsigned int i = 0; i != n; i++)
        if (has_wm_state(ch[i]))
            found = ch[i];
    if (!found)
        for (unsigned int i = 0; i != n; i++) {
            found = find_wm_state(ch[i]);
            if (found)
                break;
        }
    XFree(ch);
    return found;
}

Window get_app_window(Window w) {
    if (!w) {
        return None;
    }

    if (frame_win.contains1(w)) {
        const Window app = frame_win.find1(w);
        if (is_stable_app_window(app) && has_wm_state(app)) {
            return app;
        }
        frame_win.erase1(w);
    }

    if (frame_child.contains1(w)) {
        const Window app = frame_child.find1(w);
        if (is_stable_app_window(app) && has_wm_state(app)) {
            return app;
        }
        frame_child.erase1(w);
    }

    Window w2 = find_wm_state(w);
    if (w2 && is_stable_app_window(w2)) {
        frame_child.add(w, w2);
        if (w2 != w) {
            XSelectInput(dpy, w2, StructureNotifyMask | PropertyChangeMask);
        }
        return w2;
    }
    if (verbosity >= 1) {
        printf("Window 0x%lx does not have a stable associated top-level window\n", w);
    }
    return None;
}
