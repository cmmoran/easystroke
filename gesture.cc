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
#include "gesture.h"

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/export.hpp>

BOOST_CLASS_EXPORT(Stroke)

void update_triple(const RTriple& e, float x, float y, Time t) {
	e->x = x;
	e->y = y;
	e->t = t;
}

RTriple create_triple(float x, float y, Time t) {
	RTriple e(new Triple);
	update_triple(e, x, y, t);
	return e;
}

Stroke::Stroke(const PreStroke &ps, const int trigger_, const int button_, unsigned int modifiers_, bool timeout_, bool isRockerLeft_, bool isRockerRight_) : trigger(trigger_), button(button_), modifiers(modifiers_), timeout(timeout_), isRockerLeft(isRockerLeft_), isRockerRight(isRockerRight_) {
	if (ps.valid()) {
		stroke_t *s = stroke_alloc(static_cast<int>(ps.size()));
		for (const auto & p : ps)
			stroke_add_point(s, p->x, p->y);
		stroke_finish(s);
		stroke.reset(s, &stroke_free);
	}
}

int Stroke::compare(const RStroke& a, const RStroke &b, double &score) {
	score = 0.0;
	if (!a || !b)
		return -1;
	if (!a->timeout != !b->timeout)
		return -1;
	if (a->button != b->button)
		return -1;
	if (a->trigger != b->trigger)
		return -1;
	if (a->modifiers != b->modifiers)
		return -1;
	if (!a->stroke || !b->stroke) {
		if (!a->stroke && !b->stroke) {
			score = 1.0;
			return 1;
		}
		return -1;
	}
	double cost = stroke_compare(a->stroke.get(), b->stroke.get(), nullptr, nullptr);
	if (cost >= stroke_infinity)
		return -1;
	score = MAX(1.0 - 2.5*cost, 0.0);
	if (a->timeout)
		return score > 0.85;
	return score > 0.7;
}

Glib::RefPtr<Gdk::Pixbuf> Stroke::draw(int size, double width, bool inv) const {
	if (size != STROKE_SIZE || (width != 2.0 && width != 4.0) || inv)
		return draw_(size, width, inv);
	int i = width == 2.0;
	if (pb[i])
		return pb[i];
	pb[i] = draw_(size, width);
	return pb[i];
}

Glib::RefPtr<Gdk::Pixbuf> Stroke::pbEmpty;

Glib::RefPtr<Gdk::Pixbuf> Stroke::drawEmpty(int size) {
	if (size != STROKE_SIZE)
		return drawEmpty_(size);
	if (pbEmpty)
		return pbEmpty;
	pbEmpty = drawEmpty_(size);
	return pbEmpty;
}


RStroke Stroke::trefoil() {
	PreStroke s;
	constexpr int n = 40;
	for (int i = 0; i<=n; i++) {
		const double phi = M_PI*(-4.0*i/n)-2.7;
		const double r = exp(1.0 + sin(6.0*M_PI*i/n)) + 2.0;
		s.add(create_triple(static_cast<float>(r*cos(phi)), static_cast<float>(r*sin(phi)), i));
	}
	return create(s, 0, 0, AnyModifier, false, false, false);
}
