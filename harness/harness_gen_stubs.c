/* AUTO-GENERATED harness link stubs (S0). Frontend/desktop functions the
 * reconvert UAF repro does not exercise -> no-op. Refine any that turn out
 * to be on the repro path. */
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include "utils/ns_errors.h"

struct browser_window;
struct gui_window;
struct hlcache_handle;
struct selection;
struct textarea;
struct textarea_setup;
struct textarea_msg;
typedef unsigned int textarea_flags;
typedef void(*textarea_client_callback)(void *data, struct textarea_msg *msg);

/* real allocator (harness uses libc malloc so ASan tracks) */
void *macsurf_safe_alloc(size_t n){return malloc(n);}
void *macsurf_safe_calloc(size_t a,size_t b){return calloc(a,b);}
void *macsurf_safe_realloc(void*p,size_t n){return realloc(p,n);}
int macsurf_ptr_is_heap(const void*p){(void)p;return 1;}
/* fixes1191 companion - real macos9 code uses macsurf_ptr_is_valid for
 * static/PEF pointers (content_handler vtables); same no-op-accept stub
 * as macsurf_ptr_is_heap above, the harness doesn't model partition
 * bounds at all. */
int macsurf_ptr_is_valid(const void*p){(void)p;return 1;}
double macsurf_monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
unsigned long macsurf_get_ticks(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long)(t.tv_sec*60+t.tv_nsec/16666666);}
/* fixes1070 — this stub returned `unsigned long`. The REAL macos9_micros
 * (main.c:122) returns DOUBLE, and every caller declares it that way with a
 * local `extern double macos9_micros(void);` (macsurf_qjs.c, html.c,
 * cssh_select.c, macos9_tls_fetcher.c). An integer return comes back in rax
 * while the caller reads xmm0, so every timing this harness has ever taken
 * was uninitialised-register garbage rather than a measurement.
 *
 * Nothing asserted on those numbers, so it stayed invisible — the exact
 * harness false-signal class already documented for N_ELEMENTS and
 * foreground_images. It matters now: the perf brackets added this round are
 * meant to be developed and sanity-checked here before hardware sees them,
 * and garbage in equals confident nonsense out. Match the real signature. */
double macos9_micros(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000.0+t.tv_nsec/1000.0;}
/* guit + nsoptions[] are now REAL, properly-typed globals in driver.c
 * (driver.c needs a working guit->misc->schedule to pump box construction,
 * and box_get_style reads nsoptions[NSOPTION_author_level_css] at runtime —
 * a bogus 0-arg/void stub here would be a live landmine, not a harmless nop). */
/* macos9_schedule is likewise real now (harness_stubs.c) -- the JS mutation
 * bindings call it unconditionally after every DOM mutation. */
void browser_window_drop_file_at_point(void){}
void browser_window_find_target(void){}
void browser_window_frame_resize_start(void){}
struct hlcache_handle *browser_window_get_content(struct browser_window *bw){(void)bw;return NULL;}
void browser_window_get_drag_type(void){}
void browser_window_get_features(void){}
void browser_window_get_position(void){}
float browser_window_get_scale(struct browser_window *bw){(void)bw;return 1.0f;}
void browser_window_history_back(void){}
void browser_window_history_forward(void){}
void browser_window_mouse_click(void){}
void browser_window_mouse_track(void){}
void browser_window_navigate(void){}
void browser_window_page_drag_start(void){}
void browser_window_redraw(void){}
void browser_window_reformat(void){}
void browser_window_scroll_at_point(void){}
void browser_window_set_dimensions(void){}
void browser_window_set_drag_type(void){}
void browser_window_set_position(void){}
void browser_window_set_status(void){}
void cookie_manager_add(void){}
void cookie_manager_remove(void){}
void fetch_about_register(void){}
void fetch_data_register(void){}
void fetch_file_register(void){}
void fetch_javascript_register(void){}
void fetch_resource_register(void){}
void html_redraw_printing(void){}
void html_redraw_printing_border(void){}
void html_redraw_printing_top_cropped(void){}
void knockout_plot_end(void){}
void knockout_plot_start(void){}
void macos9_animation_now_ticks(void){}
void macos9_animation_register(void){}
void macos9_animation_register_rect(void){}
void macos9_font_measure_calls(void){}
void macos9_font_measure_chars(void){}
void macos9_grad_linear_unpack_count(void){}
void macos9_grad_radial_unpack_count(void){}
void macos9_grad_set_count(void){}
struct browser_window *macos9_gw_bw(struct gui_window *g){(void)g;return NULL;}
void macos9_heap_free_bytes(void){}
void macos9_heap_max_block(void){}
void macos9_hittest_scroll_x(void){}
void macos9_hittest_scroll_y(void){}
struct gui_window *macos9_window_list_head(void){return NULL;}
void macsurf__decoded_img_bytes_current(void){}
void macsurf_profile_accum_cascade(void){}
void macsurf_profile_accum_js(void){}
void macsurf_profile_accum_layout(void){}
void macsurf_profile_accum_parse(void){}
long macsurf_profile_get_js_us(void){return 0;}
void macsurf_profile_note_reflow(void){}
void macsurf_profile_stamp(void){}
void macsurf__site_blocker(void){}
void macsurf__site_box_blk(void){}
void macsurf__site_box_inline(void){}
void macsurf__site_box_inlinec(void){}
void macsurf__site_box_other(void){}
void macsurf__site_box_text(void){}
void macsurf__site_box_total(void){}
void macsurf__site_css_ok(void){}
void macsurf__site_css_skip(void){}
void macsurf__site_css_total_bytes(void){}
void macsurf__site_decoded_img_bytes_peak(void){}
void macsurf__site_decoded_img_skip_budget(void){}
void macsurf__site_fetch_active_peak(void){}
void macsurf__site_fetch_slot_fail(void){}
void macsurf__site_heavy(void){}
void macsurf__site_img_fail(void){}
void macsurf__site_img_ok(void){}
void macsurf__site_rgov_skip_css(void){}
void macsurf__site_rgov_skip_doc(void){}
void macsurf__site_rgov_skip_font(void){}
void macsurf__site_rgov_skip_img(void){}
void macsurf__site_rgov_skip_other(void){}
void macsurf__site_rgov_skip_script(void){}
/* fixes1027 -- N_ELEMENTS is a MACRO, not a function. The stub generator saw
 * an undefined symbol (libcss's parse/language.c uses N_ELEMENTS without
 * including libcss's own utils.h, and the harness force-includes no prefix,
 * so it compiled as an implicit call under -w) and dutifully stubbed it to a
 * no-op. Result: in parseSelectorSpecific,
 *     for (lut_idx = 0; lut_idx < N_ELEMENTS(pseudo_lut); lut_idx++)
 *     if (lut_idx == N_ELEMENTS(pseudo_lut)) return CSS_INVALID;
 * never entered the loop and always took the invalid branch, so EVERY
 * pseudo-class and pseudo-element selector -- :hover, :link, :visited,
 * :first-child, ::before, ::after -- was rejected at parse time in every
 * harness run there has ever been. The real macro now comes from
 * include/macsurf_safe_alloc_decls.h, mirroring macsurf_prefix.h:325. */
void netsurf_mkdir_all(void){}
void netsurf_mkpath(void){}
void netsurf_recursive_rm(void){}
void netsurf_version_major(void){}
void netsurf_version_minor(void){}
void nslog_log(void){}
void ns_system_colour(void){}
void ns_system_colour_char(void){}
void nsu_base64_decode_alloc_url(void){}
void plot_fstyle_broken_object(void){}
void plot_style_broken_object(void){}
void plot_style_content_edge(void){}
void plot_style_fill_darkwbasec(void){}
void plot_style_fill_lightwbasec(void){}
void plot_style_fill_wbasec(void){}
void plot_style_fill_wblobc(void){}
void plot_style_fill_white(void){}
void plot_style_margin_edge(void){}
void plot_style_padding_edge(void){}
void plot_style_stroke_darkwbasec(void){}
void plot_style_stroke_lightwbasec(void){}
void plot_style_stroke_wblobc(void){}
void save_text_solve_whitespace(void){}
void selection_active(void){}
void selection_clear(void){}
void selection_click(void){}
void selection_copy_to_clipboard(void){}
void selection_dragging(void){}
void selection_dragging_start(void){}
void selection_get_copy(void){}
void selection_highlighted(void){}
void selection_init(struct selection *s){(void)s;}
void selection_reinit(void){}
void selection_select_all(void){}
void selection_set_position(void){}
void selection_string_append(void){}
void selection_track(void){}
void textarea_clear_selection(void){}
struct textarea *textarea_create(const textarea_flags flags,
		const struct textarea_setup *setup,
		textarea_client_callback callback, void *data)
{(void)flags;(void)setup;(void)callback;(void)data;return NULL;}
void textarea_destroy(void){}
void textarea_drop_text(void){}
void textarea_get_selection(void){}
void textarea_keypress(void){}
void textarea_mouse_action(void){}
void textarea_redraw(void){}
void textarea_scroll(void){}
void textarea_set_caret(void){}
void textarea_set_layout(void){}
void textarea_set_text(void){}
unsigned long TickCount(void){return macsurf_get_ticks();}
void UNUSED(void){}
