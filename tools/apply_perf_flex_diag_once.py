from pathlib import Path

path = Path('browser/netsurf/content/handlers/html/layout_flex.c')
s = path.read_text()

blocks = [r'''	/* fixes1306 (#167, C0) - the FLEXITEM diagnostic (fixes1302/1304)
	 * shows a nested flex item's base_size (captured right after THIS
	 * function returns, per fixes1303) disagreeing with that SAME
	 * box's memoized flex_layout_height moments later inside the SAME
	 * parent's layout_flex_inner call -- e.g. base=23286 but
	 * cache_h=1441, both supposedly read from this one box after this
	 * one function. That should be impossible if layout_flex_item runs
	 * (for this box, this pass) exactly once before the read. Log every
	 * call for nested flex/inline-flex items specifically (not every
	 * box on the page -- bounded to the exact shape of the bug) so the
	 * next hardware run shows definitively whether it's called more
	 * than once per pass, memo-hit or not, and what height each call
	 * sees. */
	if (b->type == BOX_FLEX || b->type == BOX_INLINE_FLEX) {
		macsurf_debug_log_writef(
			"LIFE FLEXITEMCALL box=%p avail_w=%d b_w=%d b_h=%d "
			"gen=%d cache_gen=%d cache_w=%d cache_h=%d "
			"memo_hit=%d",
			(void *)b, available_width, (int)b->width,
			(int)b->height, (int)macsurf_layout_pass_gen,
			(int)b->flex_layout_gen, (int)b->flex_layout_width,
			(int)b->flex_layout_height,
			(macsurf_flex_layout_cache_enabled &&
				b->flex_layout_gen == macsurf_layout_pass_gen &&
				b->flex_layout_width == available_width &&
				b->flex_layout_width == b->width &&
				b->flex_layout_height == b->height) ? 1 : 0);
	}

''', r'''	/* fixes161e - per-call FLEX marker capped at first 200 calls per
	 * redraw. Counter resets when macsurf_layout_seq changes
	 * (incremented in layout_document). Child count walks flex->children
	 * once; capped at 999 as a safety. Prime suspect for both apple and
	 * huffpost; per-call granularity will pin the exact box. */
	{
		extern long macsurf_layout_seq;
		static long macsurf_flex_calls = 0;
		static long macsurf_flex_seq = -1;
		int macsurf_flex_children = 0;
		struct box *macsurf_flex_c;
		if (macsurf_flex_seq != macsurf_layout_seq) {
			macsurf_flex_calls = 0;
			macsurf_flex_seq = macsurf_layout_seq;
		}
		macsurf_flex_calls++;
		if (macsurf_flex_calls <= 200) {
			for (macsurf_flex_c = flex->children;
			     macsurf_flex_c != NULL;
			     macsurf_flex_c = macsurf_flex_c->next) {
				macsurf_flex_children++;
				if (macsurf_flex_children > 999)
					break;
			}
			macsurf_debug_log_writef(
				"LAYOUTPHASE flex #%ld box=%p type=%d w=%d h=%d children=%d",
				macsurf_flex_calls, (void *)flex,
				(int)flex->type,
				(int)flex->width, (int)flex->height,
				macsurf_flex_children);
		}
	}

''']

for block in blocks:
    if s.count(block) != 1:
        raise SystemExit('expected one diagnostic block, got %d' % s.count(block))
    s = s.replace(block, '', 1)

path.write_text(s)
