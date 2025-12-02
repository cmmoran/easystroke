/*
 * Copyright (c) 2012, Thomas Jaeger <ThJaeger@gmail.com>
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
#ifndef __HANDLER_H__
#define __HANDLER_H__
#include "gesture.h"
#include "grabber.h"
#include "actiondb.h"

enum MouseState {
	NONE,
	OUTSIDE,
	BOUND,
	CENTER,
};

class Handler;

class XState {
	friend class Handler;
public:
	static const char *state_name[];

	XState();

	bool handle(Glib::IOCondition);
	void handle_enter_leave(XEvent &ev);
	void handle_event(XEvent &ev);
	void handle_xi2_event(XIDeviceEvent *event);
	void handle_raw_motion(XIRawEvent *event);
	void report_xi2_event(XIDeviceEvent *event, const char *type);

	void fake_core_button(guint b, bool press);
	void fake_click(guint b);
	void update_core_mapping();
	bool is_synergy_bound(int mouse_x, int mouse_y);
	bool is_cycling_detected(MouseState mouse_state);
	MouseState get_mouse_state(int x, int y);

	void remove_device(int deviceid);
	void ungrab(int deviceid);

	bool idle();
	void ping();
	void bail_out();
	void select();
        void run_action(RAction act);
        void queue(sigc::slot<void> f);
        std::string select_window();

        static bool get_primary_monitor_center(int *center_x, int *center_y);
        void request_control_state(bool wants_controlled, const char *reason);
        bool apply_control_state();
        bool drain_pending_events();
        bool drain_pending_events_batch(int max_events);
        static void activate_window(Window w, Time t);
        static Window get_window(Window w, Atom prop);
        static Atom get_atom(Window w, Atom prop);
        static bool has_atom(Window w, Atom prop, Atom value);
        static void icccm_client_message(Window w, Atom a, Time t);

	Grabber::XiDevice *current_dev;
	bool in_proximity;
	bool accepted;
	std::set<guint> xinput_pressed;
	guint modifiers;
	std::map<guint, guint> core_inv_map;
	int x_min, x_max, y_min, y_max; // Define Synergy controlled region
	bool controlled = false;
	int cycling_threshold = 2; // Threshold for cycling behavior near the edges or center of the bounding box
        int screenWidth, screenHeight, screenTop, screenBot, screenLeft, screenRight;
        int transitionCount = 0, requiredTransitions = 2;         // Track the number of EDGE → CENTER transitions
        int transitionOutCount = 0, requiredOutTransitions = 3;
        MouseState prevState = NONE; // Initial state is outside the controlled area
        PointerBarrier top, left, bottom, right;
        bool target_controlled = false;
        sigc::connection control_timeout;
        sigc::connection drain_idle;
        std::string pending_control_reason;
        int randr_event_base;
        int randr_error_base;
private:
        Window ping_window;
        Handler *handler;

        void init_randr_tracking();
        void handle_randr_event(XEvent &ev);
        void update_screen_metrics();
        void destroy_pointer_barriers();
        void rebuild_pointer_barriers();

        static int xErrorHandler(Display *dpy2, XErrorEvent *e);
        static int xIOErrorHandler(Display *dpy2);
        int (*oldHandler)(Display *, XErrorEvent *);
        int (*oldIOHandler)(Display *);
	std::list<sigc::slot<void> > queued;
	std::map<int, std::string> opcodes;
};

class Handler {
public:
	Handler *child;
	Handler *parent;
	Handler() : child(nullptr), parent(nullptr) {}
	Handler *top() {
		if (child)
			return child->top();
		return this;
	}

	virtual void motion(RTriple e) {}
	virtual void raw_motion(RTriple e, bool, bool) {}
	virtual void press(guint b, RTriple e) {}
	virtual void release(guint b, RTriple e) {}
	virtual void press_master(guint b, Time t) {}
	virtual void pong() {}
	void replace_child(Handler *c);
	virtual void init() {}
	virtual ~Handler() {
		if (child)
			delete child;
	}
	virtual std::string name() = 0;
	virtual Grabber::State grab_mode() = 0;
};

extern XState *xstate;

#endif
