/*
 * MacSurf — macsurf_js_dom.c
 *
 * Duktape/C bindings that bridge JS calls to libdom.  Priority order
 * from the plan:
 *   1.  document.getElementById
 *   2.  element.innerHTML setter/getter
 *   3.  element.style setter  (TODO stage 10+)
 *   4.  element.addEventListener  (wired via js_dom_event_add_listener)
 *   5.  document.createElement
 *   6.  element.appendChild
 *   7.  window.location.href  (TODO)
 *   8.  XMLHttpRequest  (macsurf_js_xhr.c)
 *   9.  document.querySelector  (basic: #id / tag only for now)
 *  10.  console.log
 *
 * Every dom_* call must null-check its return.  A malformed DOM is a
 * normal runtime condition, not a bug.
 *
 * The JS representation of a DOM element is a plain JS object with
 * these internal properties:
 *   __el        : pointer to dom_element (via duk_push_pointer)
 *   __listeners : { "click": [fn, fn, ...], ... }
 * Plus accessor properties: tagName, innerHTML, ... registered via
 * duk_def_prop.
 *
 * A Duktape finalizer on the wrapper object calls dom_node_unref when
 * the JS object is garbage collected, matching libdom's refcount model.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "duktape.h"
#include "macsurf_js.h"
#include "macsurf_debug.h"

/*
 * libdom is in the source tree but not all the implementation .c files
 * are in the CW8 project file list, and the public header relies on
 * static-inline helpers that CW8 doesn't always emit out-of-line bodies
 * for.  Until the full libdom binding lands (deferred — see
 * dom_parser.c port note in the moonshot plan), we provide local
 * forward decls + no-op stubs in this TU so the JS DOM bindings
 * compile, link, and gracefully return null when no document is set.
 */

#include <dom/core/exceptions.h>

struct dom_document;
struct dom_element;
struct dom_node;
struct dom_string;
typedef struct dom_element  dom_element;
typedef struct dom_document dom_document;
typedef struct dom_node     dom_node;
typedef struct dom_string   dom_string;

/* Real out-of-line functions from libdom src/core/string.c.
 * Signatures match libdom/include/dom/core/string.h. */
extern dom_exception dom_string_create(const uint8_t *ptr, size_t len,
		dom_string **str);
extern const char   *dom_string_data(const dom_string *str);
extern uint32_t      dom_string_length(dom_string *str);

/* macsurf_dom_dispatch.c — wrappers for libdom static-inline functions.
 * Static inlines have no out-of-line symbol; the dispatch .c creates one. */
extern void          macsurf_dom_node_ref(dom_node *node);
extern void          macsurf_dom_node_unref(dom_node *node);
extern void          macsurf_dom_string_unref(dom_string *str);
extern dom_exception macsurf_dom_document_get_element_by_id(dom_document *doc,
		dom_string *id, dom_element **element);
extern dom_exception macsurf_dom_document_create_element(dom_document *doc,
		dom_string *tag_name, dom_element **element);
extern dom_exception macsurf_dom_element_get_tag_name(dom_element *el,
		dom_string **name);
extern dom_exception macsurf_dom_element_get_attribute(dom_element *el,
		dom_string *name, dom_string **value);
extern dom_exception macsurf_dom_element_set_attribute(dom_element *el,
		dom_string *name, dom_string *value);
extern dom_exception macsurf_dom_node_append_child(dom_node *parent,
		dom_node *new_child, dom_node **result);
/* fixes382 (M1) — JS->DOM->render: real document.body/documentElement/head. */
extern dom_exception macsurf_dom_document_get_document_element(
		dom_document *doc, dom_element **result);
extern dom_exception macsurf_dom_node_get_first_child(dom_node *node,
		dom_node **result);
extern dom_exception macsurf_dom_node_get_next_sibling(dom_node *node,
		dom_node **result);
/* fixes385 (M4) — extend the mutation API: real createTextNode / insertBefore
 * / removeChild (the React reconciler needs these beyond appendChild). */
typedef struct dom_text dom_text;
extern dom_exception macsurf_dom_document_create_text_node(dom_document *doc,
		dom_string *data, dom_text **text);
extern dom_exception macsurf_dom_node_remove_child(dom_node *parent,
		dom_node *old_child, dom_node **result);
extern dom_exception macsurf_dom_node_insert_before(dom_node *parent,
		dom_node *new_child, dom_node *ref_child, dom_node **result);

/* fixes342 — safe JS-init eval (defined in macsurf_js.c). Used for
 * per-element wrapper polyfill installs so a parse error in one
 * shim doesn't break ALL element wrappers. */
extern void macsurf_js__safe_eval(duk_context *ctx, const char *src);

/* fixes349 — install_per_element runs a JS function expression of the
 * form  (function(el){ ... })  with the wrapper currently on top of the
 * value stack passed as the `el` argument. Replaces the old pattern
 *
 *     macsurf_js__safe_eval(duk, "(function(el){...})");
 *     duk_dup(duk, -2);
 *     duk_call(duk, 1);
 *     duk_pop(duk);
 *
 * which was broken by fixes342: safe_eval uses duk_peval_string_noresult
 * which DISCARDS the function result, leaving the stack as [wrapper]
 * instead of [wrapper, fn]. duk_dup(-2) then grabbed whatever was below
 * the wrapper (in element_finalizer/getElementById callers, the wrapper
 * itself or a string arg) and duk_call(1) tried to call THAT — which
 * produced the visible "TypeError: [object Object] not callable" that
 * killed the entire per-element wrapper install. The whole element gets
 * left without classList/style/matches/closest/getBoundingClientRect/etc.
 *
 * This helper does the eval-then-pcall sequence with proper stack
 * management and pcall-based error trapping so one bad shim can't kill
 * the rest. Stack on entry: [..., wrapper]. Stack on exit: same.
 */
static void macsurf_js__install_per_element(duk_context *duk,
		const char *src)
{
	if (duk_peval_string(duk, src) != 0) {
		const char *err = duk_safe_to_string(duk, -1);
		macsurf_debug_log_writef(
			"js per-elem eval failed: %s",
			err ? err : "(null)");
		duk_pop(duk);
		return;
	}
	/* stack: [..., wrapper, fn] */
	duk_dup(duk, -2);                       /* dup wrapper as arg */
	if (duk_pcall(duk, 1) != 0) {
		const char *err = duk_safe_to_string(duk, -1);
		macsurf_debug_log_writef(
			"js per-elem call failed: %s",
			err ? err : "(null)");
	}
	duk_pop(duk);                           /* pop result or error */
}

/* ----------------------------------------------------------------- */
/* Current document — set by the html handler on page load.          */
/* ----------------------------------------------------------------- */

static dom_document *macsurf_js_current_document = NULL;

void
macsurf_js_set_document(dom_document *doc)
{
	macsurf_js_current_document = doc;
}

/* fixes383 (M2) — current html content, for the JS->DOM->render re-convert
 * trigger. Set alongside the document in js_newthread (doc_priv IS the
 * html_content). Stored as an opaque struct content* (the html_content base)
 * so this TU needs no html internals. */
struct content;
static struct content *macsurf_js_current_content = NULL;
/* fixes384 (M3) — debounced re-convert (macos9_reconvert.c). Replaces the
 * fixes383 immediate html_reconvert_content call: a mutation burst now
 * coalesces to ONE re-convert via the scheduler. */
extern void macos9_js_mark_dom_dirty(struct content *c);

void
macsurf_js_set_content(struct content *c)
{
	macsurf_js_current_content = c;
}

/* Stage 1 death-row hook (Seam 1, the one exception): macsurf_js_current_content
 * is a raw content pointer independent of content_list and not reachable via
 * any scheduled continuation, so the death-row pinned-check cannot see it. The
 * drain calls this just before it frees a content; NULL our cached pointer if
 * it is the one going away, so the DOM bindings never deref a freed content. */
void
macsurf_js_notify_content_freed(struct content *c)
{
	if (macsurf_js_current_content == c) {
		macsurf_js_current_content = NULL;
	}
}

/* ----------------------------------------------------------------- */
/* Element wrapper push/finalizer                                     */
/* ----------------------------------------------------------------- */

static duk_ret_t
element_finalizer(duk_context *duk)
{
	dom_element *el;

	duk_get_prop_string(duk, 0, "__el");
	el = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop(duk);

	if (el != NULL) {
		macsurf_dom_node_unref((dom_node *)el);
	}
	return 0;
}

/* Forward decls for helpers defined later in this TU. */
static duk_ret_t macsurf_getTagName(duk_context *duk);
static duk_ret_t macsurf_getInnerHTML(duk_context *duk);
static duk_ret_t macsurf_setInnerHTML(duk_context *duk);
static duk_ret_t macsurf_getTextContent(duk_context *duk);
static duk_ret_t macsurf_setTextContent(duk_context *duk);
extern dom_exception macsurf_dom_node_get_text_content(dom_node *node,
		dom_string **result);
extern dom_exception macsurf_dom_node_set_text_content(dom_node *node,
		dom_string *content);
static duk_ret_t macsurf_getAttribute(duk_context *duk);
static duk_ret_t macsurf_setAttribute(duk_context *duk);
static duk_ret_t macsurf_appendChild(duk_context *duk);
static duk_ret_t macsurf_insertBefore(duk_context *duk);   /* fixes385 (M4) */
static duk_ret_t macsurf_removeChild(duk_context *duk);    /* fixes385 (M4) */
static duk_ret_t macsurf_addEventListener(duk_context *duk);
static void wrapper_register(duk_context *duk, dom_element *el,
		duk_idx_t wrapper_idx);

void
macsurf_push_element(duk_context *duk, dom_element *el)
{
	if (el == NULL) {
		duk_push_null(duk);
		return;
	}

	/* Take an extra ref — the finalizer will drop it. */
	macsurf_dom_node_ref((dom_node *)el);

	duk_push_object(duk);
	duk_push_pointer(duk, el);
	duk_put_prop_string(duk, -2, "__el");

	duk_push_object(duk);
	duk_put_prop_string(duk, -2, "__listeners");

	/* DOM methods on the element wrapper. */
	duk_push_c_function(duk, macsurf_getTagName, 0);
	duk_put_prop_string(duk, -2, "tagName");
	duk_push_c_function(duk, macsurf_getAttribute, 1);
	duk_put_prop_string(duk, -2, "getAttribute");
	duk_push_c_function(duk, macsurf_setAttribute, 2);
	duk_put_prop_string(duk, -2, "setAttribute");
	duk_push_c_function(duk, macsurf_appendChild, 1);
	duk_put_prop_string(duk, -2, "appendChild");
	duk_push_c_function(duk, macsurf_insertBefore, 2);   /* fixes385 (M4) */
	duk_put_prop_string(duk, -2, "insertBefore");
	duk_push_c_function(duk, macsurf_removeChild, 1);    /* fixes385 (M4) */
	duk_put_prop_string(duk, -2, "removeChild");
	duk_push_c_function(duk, macsurf_addEventListener, 3);
	duk_put_prop_string(duk, -2, "addEventListener");

	/* innerHTML accessor. */
	duk_push_string(duk, "innerHTML");
	duk_push_c_function(duk, macsurf_getInnerHTML, 0);
	duk_push_c_function(duk, macsurf_setInnerHTML, 1);
	duk_def_prop(duk, -4,
			DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	/* fixes319d — textContent accessor. Writes through to the real
	 * DOM via dom_node_set_text_content so el.textContent='foo' in JS
	 * actually replaces the element's children with a text node. */
	duk_push_string(duk, "textContent");
	duk_push_c_function(duk, macsurf_getTextContent, 0);
	duk_push_c_function(duk, macsurf_setTextContent, 1);
	duk_def_prop(duk, -4,
			DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	/* fixes337 — extra element methods commonly feature-detected:
	 * getBoundingClientRect, scrollIntoView, focus, blur, click,
	 * dispatchEvent, dataset. All V1 stubs returning sensible values.
	 * Real impls queued for future rounds when the visual path
	 * matters per-method. */
	macsurf_js__install_per_element(duk,
		"(function(el){"
		"  el.getBoundingClientRect=function(){"
		"    return {top:0,left:0,right:0,bottom:0,width:0,height:0,x:0,y:0};"
		"  };"
		"  el.scrollIntoView=function(){};"
		"  el.focus=function(){};"
		"  el.blur=function(){};"
		"  el.click=function(){"
		"    var L=this.__listeners;if(L&&L.click)L.click.forEach(function(f){"
		"      try{f.call(this);}catch(e){}"
		"    });"
		"  };"
		"  el.dispatchEvent=function(ev){"
		"    var t=ev&&ev.type||'';"
		"    var L=this.__listeners;"
		"    if(L&&L[t])L[t].forEach(function(f){try{f.call(this,ev);}catch(e){}});"
		"    return true;"
		"  };"
		"  el.removeEventListener=function(t,fn){"
		"    if(!this.__listeners||!this.__listeners[t])return;"
		"    var arr=this.__listeners[t];"
		"    for(var i=0;i<arr.length;i++)if(arr[i]===fn){arr.splice(i,1);return;}"
		"  };"
		"  el.dataset=el.dataset||{};"
		"})");

	/* fixes382 (M1) — layout-ish metrics on every wrapper so the now-REAL
	 * document.body / documentElement answer FB's viewport sizing (preserves
	 * the fixes379 stub values). Plain values for now; layout-derived dims
	 * are a later refinement. */
	macsurf_js__install_per_element(duk,
		"(function(el){"
		"  var vw=(typeof window!=='undefined'&&window.innerWidth)||980;"
		"  var vh=(typeof window!=='undefined'&&window.innerHeight)||600;"
		"  if(el.clientWidth===undefined)el.clientWidth=vw;"
		"  if(el.clientHeight===undefined)el.clientHeight=vh;"
		"  if(el.offsetWidth===undefined)el.offsetWidth=vw;"
		"  if(el.offsetHeight===undefined)el.offsetHeight=vh;"
		"  if(el.scrollWidth===undefined)el.scrollWidth=vw;"
		"  if(el.scrollHeight===undefined)el.scrollHeight=vh;"
		"  if(el.scrollTop===undefined)el.scrollTop=0;"
		"  if(el.scrollLeft===undefined)el.scrollLeft=0;"
		"  if(el.offsetTop===undefined)el.offsetTop=0;"
		"  if(el.offsetLeft===undefined)el.offsetLeft=0;"
		"  if(el.nodeType===undefined)el.nodeType=1;"
		"})");

	/* fixes336 — common element methods (matches, closest, children,
	 * firstChild / lastChild / parentNode / nextSibling / cloneNode).
	 * Currently all stubs except matches (uses getAttribute on the
	 * wrapper to mock-check id / class / tag patterns). Stubs return
	 * sensible defaults so feature-detection succeeds. */
	macsurf_js__install_per_element(duk,
		"(function(el){"
		"  el.matches=function(sel){"
		"    if(!sel)return false;"
		"    sel=String(sel).trim();"
		"    if(sel.charAt(0)=='#'){"
		"      return el.getAttribute('id')==sel.substr(1);"
		"    }else if(sel.charAt(0)=='.'){"
		"      var c=el.getAttribute('class')||'';"
		"      var want=sel.substr(1);"
		"      var ks=c.split(/\\s+/);"
		"      for(var i=0;i<ks.length;i++)if(ks[i]==want)return true;"
		"      return false;"
		"    }else{"
		"      var tn=el.tagName?el.tagName():'';"
		"      return tn.toLowerCase()==sel.toLowerCase();"
		"    }"
		"  };"
		"  el.closest=function(sel){"
		"    var cur=el;"
		"    while(cur){"
		"      if(cur.matches&&cur.matches(sel))return cur;"
		"      cur=cur.parentNode||null;"
		"    }"
		"    return null;"
		"  };"
		"  el.children=el.children||[];"
		"  el.parentNode=el.parentNode||null;"
		"  el.parentElement=el.parentElement||null;"
		"  el.firstChild=el.firstChild||null;"
		"  el.lastChild=el.lastChild||null;"
		"  el.nextSibling=el.nextSibling||null;"
		"  el.previousSibling=el.previousSibling||null;"
		"  el.cloneNode=function(deep){return el;};"
		"  el.contains=function(other){return other===el;};"
		"  el.hasAttribute=function(name){return el.getAttribute(name)!==null;};"
		"  el.removeAttribute=function(name){el.setAttribute(name,'');};"
		"})");

	/* fixes326 (#30) — classList helper object. .add / .remove /
	 * .toggle / .contains operate on the element's `class` attribute
	 * via getAttribute / setAttribute. Implemented in JS so it's
	 * simpler and the underlying attribute calls already DOM-write. */
	macsurf_js__install_per_element(duk,
		"(function(el){"
		"  Object.defineProperty(el,'classList',{"
		"    get:function(){"
		"      var self=this;"
		"      function _list(){"
		"        var c=self.getAttribute('class')||'';"
		"        return c.split(/\\s+/).filter(function(x){return x.length;});"
		"      }"
		"      function _write(arr){"
		"        self.setAttribute('class',arr.join(' '));"
		"      }"
		"      return {"
		"        add:function(){"
		"          var a=_list();"
		"          for(var i=0;i<arguments.length;i++){"
		"            if(a.indexOf(arguments[i])<0)a.push(arguments[i]);"
		"          }_write(a);"
		"        },"
		"        remove:function(){"
		"          var a=_list();"
		"          for(var i=0;i<arguments.length;i++){"
		"            var x=a.indexOf(arguments[i]);"
		"            if(x>=0)a.splice(x,1);"
		"          }_write(a);"
		"        },"
		"        toggle:function(t,force){"
		"          var a=_list();var i=a.indexOf(t);"
		"          if(i>=0&&force!==true){a.splice(i,1);_write(a);return false;}"
		"          if(i<0&&force!==false){a.push(t);_write(a);return true;}"
		"          return i>=0;"
		"        },"
		"        contains:function(t){return _list().indexOf(t)>=0;},"
		"        get length(){return _list().length;}"
		"      };"
		"    },configurable:true"
		"  });"
		"})");

	/* fixes327 (#32) — element.style with setProperty / cssText.
	 * Duktape is ES5 (no Proxy), so we install Object.defineProperty
	 * accessors for the most common camelCase property names. Anything
	 * else can use the standard CSS Object Model methods
	 * setProperty / getPropertyValue / cssText. */
	macsurf_js__install_per_element(duk,
		"(function(el){"
		"  function _kebab(p){return p.replace(/([A-Z])/g,'-$1').toLowerCase();}"
		"  function _set(p,v){"
		"    var s=el.getAttribute('style')||'';"
		"    var k=_kebab(p);"
		"    var re=new RegExp('(^|;)\\\\s*'+k+'\\\\s*:[^;]*;?');"
		"    s=s.replace(re,'');"
		"    if(v!==''&&v!==null&&v!==undefined)s+=k+':'+v+';';"
		"    el.setAttribute('style',s);"
		"  }"
		"  function _get(p){"
		"    var s=el.getAttribute('style')||'';"
		"    var k=_kebab(p);"
		"    var re=new RegExp('(?:^|;)\\\\s*'+k+'\\\\s*:\\\\s*([^;]+)');"
		"    var m=s.match(re);return m?m[1].trim():'';"
		"  }"
		"  var style={"
		"    setProperty:function(p,v){_set(p,v);},"
		"    getPropertyValue:function(p){return _get(p);},"
		"    removeProperty:function(p){var v=_get(p);_set(p,'');return v;}"
		"  };"
		"  Object.defineProperty(style,'cssText',{"
		"    get:function(){return el.getAttribute('style')||'';},"
		"    set:function(v){el.setAttribute('style',v||'');}"
		"  });"
		"  var props=['color','background','backgroundColor','backgroundImage',"
		"    'backgroundPosition','backgroundRepeat','backgroundSize',"
		"    'display','visibility','opacity','width','height',"
		"    'minWidth','minHeight','maxWidth','maxHeight',"
		"    'margin','marginTop','marginLeft','marginRight','marginBottom',"
		"    'padding','paddingTop','paddingLeft','paddingRight','paddingBottom',"
		"    'border','borderTop','borderLeft','borderRight','borderBottom',"
		"    'borderWidth','borderColor','borderStyle','borderRadius',"
		"    'fontSize','fontWeight','fontFamily','fontStyle',"
		"    'lineHeight','letterSpacing','wordSpacing','textAlign',"
		"    'textDecoration','textTransform','whiteSpace',"
		"    'top','left','right','bottom','position','zIndex',"
		"    'cursor','overflow','overflowX','overflowY','float','clear',"
		"    'transform','transition','animation','boxShadow','outline'];"
		"  props.forEach(function(p){"
		"    Object.defineProperty(style,p,{"
		"      get:function(){return _get(p);},"
		"      set:function(v){_set(p,v);},"
		"      enumerable:true,configurable:true"
		"    });"
		"  });"
		"  Object.defineProperty(el,'style',{value:style,configurable:true});"
		"})");

	duk_push_c_function(duk, element_finalizer, 1);
	duk_set_finalizer(duk, -2);

	/* Register the wrapper in the pointer-keyed stash so
	 * dispatch_event can find it from a raw dom_element*. */
	wrapper_register(duk, el, duk_get_top_index(duk));
}

static dom_element *
element_from_this(duk_context *duk)
{
	dom_element *el;
	duk_push_this(duk);
	duk_get_prop_string(duk, -1, "__el");
	el = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop_2(duk);
	return el;
}

/* ----------------------------------------------------------------- */
/* document.getElementById                                            */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getElementById(duk_context *duk)
{
	const char *id;
	dom_string *id_str = NULL;
	dom_element *el = NULL;

	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}

	id = duk_require_string(duk, 0);
	if (id == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (dom_string_create((const unsigned char *)id, strlen(id), &id_str)
			!= DOM_NO_ERR || id_str == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (macsurf_dom_document_get_element_by_id(macsurf_js_current_document,
			id_str, &el) != DOM_NO_ERR) {
		el = NULL;
	}
	macsurf_dom_string_unref(id_str);

	macsurf_debug_log_writef(
		"getElementById('%s') -> %s", id, el ? "FOUND" : "null");
	macsurf_push_element(duk, el);
	return 1;
}

/* ----------------------------------------------------------------- */
/* element.tagName                                                    */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getTagName(duk_context *duk)
{
	dom_element *el;
	dom_string *tag = NULL;

	el = element_from_this(duk);
	if (el == NULL) {
		duk_push_string(duk, "");
		return 1;
	}

	if (macsurf_dom_element_get_tag_name(el, &tag) != DOM_NO_ERR || tag == NULL) {
		duk_push_string(duk, "");
		return 1;
	}

	duk_push_lstring(duk, dom_string_data(tag), dom_string_length(tag));
	macsurf_dom_string_unref(tag);
	return 1;
}

/* ----------------------------------------------------------------- */
/* element.innerHTML — getter / setter                                */
/*                                                                    */
/* Full HTML serialisation needs libdom's HTML serialiser which is    */
/* not yet wired up.  Stage 4 returns empty string for the getter     */
/* and stores the raw assignment on the wrapper object so subsequent  */
/* reads see the last-written value.  True DOM mutation is TODO.      */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getInnerHTML(duk_context *duk)
{
	duk_push_this(duk);
	if (duk_get_prop_string(duk, -1, "__innerHTML") == 0) {
		duk_pop(duk);
		duk_push_string(duk, "");
	}
	/* leave value on stack */
	return 1;
}

static duk_ret_t
macsurf_setInnerHTML(duk_context *duk)
{
	const char *html = duk_safe_to_string(duk, 0);
	duk_push_this(duk);
	duk_push_string(duk, html);
	duk_put_prop_string(duk, -2, "__innerHTML");
	duk_pop(duk);
	/* TODO stage 10+: actually parse html and replace element children. */
	return 0;
}

/* fixes319d — textContent getter/setter. Routes through the libdom
 * dom_node_set_text_content vtable call so writes propagate to the
 * actual DOM tree (and so subsequent reads from libcss / layout see
 * the updated text). */
static duk_ret_t
macsurf_getTextContent(duk_context *duk)
{
	dom_element *el = element_from_this(duk);
	dom_string *out = NULL;
	if (el == NULL ||
	    macsurf_dom_node_get_text_content((dom_node *)el, &out) !=
			DOM_NO_ERR || out == NULL) {
		duk_push_string(duk, "");
		return 1;
	}
	duk_push_lstring(duk, dom_string_data(out), dom_string_length(out));
	macsurf_dom_string_unref(out);
	return 1;
}

static duk_ret_t
macsurf_setTextContent(duk_context *duk)
{
	dom_element *el = element_from_this(duk);
	const char *txt;
	dom_string *str = NULL;
	dom_exception ex;
	if (el == NULL) {
		MS_LOG("setTextContent: el is NULL");
		return 0;
	}
	txt = duk_safe_to_string(duk, 0);
	if (txt == NULL) {
		MS_LOG("setTextContent: txt is NULL");
		return 0;
	}
	if (dom_string_create((const unsigned char *)txt, strlen(txt),
			&str) != DOM_NO_ERR || str == NULL) {
		MS_LOG("setTextContent: dom_string_create failed");
		return 0;
	}
	ex = macsurf_dom_node_set_text_content((dom_node *)el, str);
	macsurf_debug_log_writef(
		"setTextContent: ex=%d len=%d", (int)ex, (int)strlen(txt));
	macsurf_dom_string_unref(str);
#ifdef __MACOS9__
	/* fixes383 (M2) — supersedes fixes320j. textContent='...' replaced the
	 * element's children in the real DOM; a re-LAYOUT of the stale box tree
	 * (the old path) would not show it. Re-convert rebuilds the box tree from
	 * the mutated DOM and repaints, so the new text actually appears. */
	if (macsurf_js_current_content != NULL)
		macos9_js_mark_dom_dirty(macsurf_js_current_content);
#endif
	return 0;
}

/* ----------------------------------------------------------------- */
/* element.getAttribute / setAttribute                                */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getAttribute(duk_context *duk)
{
	dom_element *el;
	dom_string *name_str = NULL;
	dom_string *val_str = NULL;
	const char *name;

	el = element_from_this(duk);
	if (el == NULL) {
		duk_push_null(duk);
		return 1;
	}

	name = duk_require_string(duk, 0);
	if (dom_string_create((const unsigned char *)name, strlen(name),
			&name_str) != DOM_NO_ERR || name_str == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (macsurf_dom_element_get_attribute(el, name_str, &val_str) != DOM_NO_ERR
			|| val_str == NULL) {
		macsurf_dom_string_unref(name_str);
		duk_push_null(duk);
		return 1;
	}

	duk_push_lstring(duk, dom_string_data(val_str),
			dom_string_length(val_str));
	macsurf_dom_string_unref(val_str);
	macsurf_dom_string_unref(name_str);
	return 1;
}

static duk_ret_t
macsurf_setAttribute(duk_context *duk)
{
	dom_element *el;
	dom_string *name_str = NULL;
	dom_string *val_str = NULL;
	const char *name;
	const char *val;

	el = element_from_this(duk);
	if (el == NULL) {
		return 0;
	}

	name = duk_require_string(duk, 0);
	val  = duk_safe_to_string(duk, 1);

	if (dom_string_create((const unsigned char *)name, strlen(name),
			&name_str) != DOM_NO_ERR || name_str == NULL) {
		return 0;
	}
	if (dom_string_create((const unsigned char *)val, strlen(val),
			&val_str) != DOM_NO_ERR || val_str == NULL) {
		macsurf_dom_string_unref(name_str);
		return 0;
	}

	macsurf_dom_element_set_attribute(el, name_str, val_str);
	macsurf_dom_string_unref(val_str);
	macsurf_dom_string_unref(name_str);
	/* fixes384 (M3) — attribute changes (class/style/hidden) affect render;
	 * debounced re-convert coalesces the frequent ones. */
	if (macsurf_js_current_content != NULL)
		macos9_js_mark_dom_dirty(macsurf_js_current_content);
	return 0;
}

/* ----------------------------------------------------------------- */
/* element.appendChild                                                */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_appendChild(duk_context *duk)
{
	dom_element *parent;
	dom_element *child = NULL;
	dom_node *result = NULL;

	parent = element_from_this(duk);
	if (parent == NULL) {
		duk_push_null(duk);
		return 1;
	}

	duk_get_prop_string(duk, 0, "__el");
	child = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop(duk);

	if (child == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (macsurf_dom_node_append_child((dom_node *)parent, (dom_node *)child,
			&result) != DOM_NO_ERR) {
		duk_push_null(duk);
		return 1;
	}

	/* fixes383 (M2) — structural DOM mutation reached the real tree;
	 * re-convert (rebuild box tree) + repaint so JS-built content shows. */
	if (macsurf_js_current_content != NULL)
		macos9_js_mark_dom_dirty(macsurf_js_current_content);

	/* Return the appended child (same as input by spec). */
	duk_dup(duk, 0);
	return 1;
}

/* ----------------------------------------------------------------- */
/* fixes385 (M4) — element.insertBefore(newNode, refNode)            */
/* refNode may be null (then == appendChild). Real libdom insert.    */
/* ----------------------------------------------------------------- */
static duk_ret_t
macsurf_insertBefore(duk_context *duk)
{
	dom_element *parent;
	dom_element *newc = NULL;
	dom_element *refc = NULL;
	dom_node *result = NULL;

	parent = element_from_this(duk);
	if (parent == NULL) {
		duk_push_null(duk);
		return 1;
	}
	duk_get_prop_string(duk, 0, "__el");
	newc = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop(duk);
	if (newc == NULL) {
		duk_push_null(duk);
		return 1;
	}
	/* refNode is optional; null => append at end. */
	if (duk_is_object(duk, 1)) {
		duk_get_prop_string(duk, 1, "__el");
		refc = (dom_element *)duk_get_pointer(duk, -1);
		duk_pop(duk);
	}

	if (macsurf_dom_node_insert_before((dom_node *)parent, (dom_node *)newc,
			(dom_node *)refc, &result) != DOM_NO_ERR) {
		duk_push_null(duk);
		return 1;
	}

	if (macsurf_js_current_content != NULL)
		macos9_js_mark_dom_dirty(macsurf_js_current_content);

	duk_dup(duk, 0);                 /* return the inserted node */
	return 1;
}

/* ----------------------------------------------------------------- */
/* fixes385 (M4) — element.removeChild(node). Real libdom remove.    */
/* ----------------------------------------------------------------- */
static duk_ret_t
macsurf_removeChild(duk_context *duk)
{
	dom_element *parent;
	dom_element *child = NULL;
	dom_node *result = NULL;

	parent = element_from_this(duk);
	if (parent == NULL) {
		duk_push_null(duk);
		return 1;
	}
	duk_get_prop_string(duk, 0, "__el");
	child = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop(duk);
	if (child == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (macsurf_dom_node_remove_child((dom_node *)parent, (dom_node *)child,
			&result) != DOM_NO_ERR) {
		duk_push_null(duk);
		return 1;
	}

	if (macsurf_js_current_content != NULL)
		macos9_js_mark_dom_dirty(macsurf_js_current_content);

	duk_dup(duk, 0);                 /* return the removed node */
	return 1;
}

/* ----------------------------------------------------------------- */
/* document.createElement                                             */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_createElement(duk_context *duk)
{
	const char *tag;
	dom_string *tag_str = NULL;
	dom_element *el = NULL;

	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}

	tag = duk_require_string(duk, 0);
	if (dom_string_create((const unsigned char *)tag, strlen(tag),
			&tag_str) != DOM_NO_ERR || tag_str == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (macsurf_dom_document_create_element(macsurf_js_current_document,
			tag_str, &el) != DOM_NO_ERR) {
		el = NULL;
	}
	macsurf_dom_string_unref(tag_str);

	macsurf_push_element(duk, el);
	return 1;
}

/* ----------------------------------------------------------------- */
/* fixes385 (M4) — document.createTextNode(text). Real dom_text;     */
/* wrapped via macsurf_push_element so appendChild(textNode) splices */
/* a real text node into the tree (the __el is used as a dom_node*). */
/* ----------------------------------------------------------------- */
static duk_ret_t
macsurf_createTextNode(duk_context *duk)
{
	const char *txt;
	dom_string *str = NULL;
	dom_text *node = NULL;

	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}
	txt = duk_safe_to_string(duk, 0);
	if (txt == NULL) {
		duk_push_null(duk);
		return 1;
	}
	if (dom_string_create((const unsigned char *)txt, strlen(txt), &str)
			!= DOM_NO_ERR || str == NULL) {
		duk_push_null(duk);
		return 1;
	}
	if (macsurf_dom_document_create_text_node(macsurf_js_current_document,
			str, &node) != DOM_NO_ERR) {
		node = NULL;
	}
	macsurf_dom_string_unref(str);

	/* Mirror createElement's ref handling: push_element refs; the create
	 * ref is the ownership ref that transfers to the DOM on append. */
	macsurf_push_element(duk, (dom_element *) node);
	return 1;
}

/* ----------------------------------------------------------------- */
/* fixes382 (M1) — document.documentElement / body / head             */
/* Real wrapped elements (was null/fake stub). The DOM nodes persist   */
/* across box-tree rebuilds, so these wrappers stay valid; JS appends   */
/* into the real <body>, the re-convert path (M2/M3) repaints it.       */
/* ----------------------------------------------------------------- */

/* Find a direct child element of `parent` whose tag == `want` (case-
 * insensitive). Returns the found element holding ONE outstanding ref
 * (the caller hands it to macsurf_push_element then unrefs), or NULL.
 * Balances refs on every skipped sibling. */
static dom_element *
macsurf_find_child_by_tag(dom_element *parent, const char *want)
{
	dom_node *child = NULL;
	dom_node *sib = NULL;
	dom_element *found = NULL;
	size_t wl;

	if (parent == NULL)
		return NULL;
	wl = strlen(want);
	if (macsurf_dom_node_get_first_child((dom_node *)parent, &child)
			!= DOM_NO_ERR)
		return NULL;
	while (child != NULL) {
		dom_string *tag = NULL;
		if (macsurf_dom_element_get_tag_name((dom_element *)child, &tag)
				== DOM_NO_ERR && tag != NULL) {
			if ((size_t)dom_string_length(tag) == wl &&
			    strncasecmp(dom_string_data(tag), want, wl) == 0) {
				found = (dom_element *)child;  /* keep this ref */
			}
			macsurf_dom_string_unref(tag);
		}
		if (found != NULL)
			break;                 /* leave child's ref on `found` */
		if (macsurf_dom_node_get_next_sibling(child, &sib) != DOM_NO_ERR)
			sib = NULL;
		macsurf_dom_node_unref(child); /* drop the skipped child's ref */
		child = sib;
		sib = NULL;
	}
	return found;
}

static duk_ret_t
macsurf_document_get_documentElement(duk_context *duk)
{
	dom_element *root = NULL;
	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}
	if (macsurf_dom_document_get_document_element(
			macsurf_js_current_document, &root) != DOM_NO_ERR)
		root = NULL;
	macsurf_push_element(duk, root);        /* push takes its own ref */
	if (root != NULL)
		macsurf_dom_node_unref((dom_node *)root);  /* drop get's ref */
	return 1;
}

static duk_ret_t
macsurf_document_get_body(duk_context *duk)
{
	dom_element *root = NULL;
	dom_element *body = NULL;
	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}
	if (macsurf_dom_document_get_document_element(
			macsurf_js_current_document, &root) != DOM_NO_ERR)
		root = NULL;
	if (root != NULL) {
		body = macsurf_find_child_by_tag(root, "body");
		macsurf_dom_node_unref((dom_node *)root);
	}
	macsurf_push_element(duk, body);        /* push takes its own ref */
	if (body != NULL)
		macsurf_dom_node_unref((dom_node *)body);  /* drop walk ref */
	return 1;
}

static duk_ret_t
macsurf_document_get_head(duk_context *duk)
{
	dom_element *root = NULL;
	dom_element *head = NULL;
	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}
	if (macsurf_dom_document_get_document_element(
			macsurf_js_current_document, &root) != DOM_NO_ERR)
		root = NULL;
	if (root != NULL) {
		head = macsurf_find_child_by_tag(root, "head");
		macsurf_dom_node_unref((dom_node *)root);
	}
	macsurf_push_element(duk, head);
	if (head != NULL)
		macsurf_dom_node_unref((dom_node *)head);
	return 1;
}

/* ----------------------------------------------------------------- */
/* querySelectorAll helper: walk the subtree rooted at `node`,        */
/* push matching element wrappers onto the Duk array at stack[-1].   */
/*                                                                    */
/* sel_tag  : tag name to match (e.g. "textarea") or NULL = any tag  */
/* sel_attr : attribute name that must be present  or NULL = ignored  */
/* sel_val  : if non-NULL, attribute value must CONTAIN this string   */
/* ----------------------------------------------------------------- */
static void
qsa_walk(duk_context *duk, dom_node *node,
         const char *sel_tag,
         const char *sel_attr, const char *sel_val)
{
    dom_node   *child = NULL;
    dom_node   *sib   = NULL;
    dom_string *tag_s = NULL;
    int         tag_match = 0;

    if (node == NULL)
        return;

    /* Check if this node is an element (get_tag_name succeeds). */
    if (macsurf_dom_element_get_tag_name((dom_element *)node, &tag_s)
            == DOM_NO_ERR && tag_s != NULL) {
        /* Tag filter */
        if (sel_tag == NULL) {
            tag_match = 1;
        } else {
            size_t tl = strlen(sel_tag);
            tag_match = ((size_t)dom_string_length(tag_s) == tl &&
                strncasecmp(dom_string_data(tag_s), sel_tag, tl) == 0);
        }
        macsurf_dom_string_unref(tag_s);

        if (tag_match) {
            /* Attribute filter */
            int attr_match = 1;
            if (sel_attr != NULL) {
                dom_string *attr_name_s = NULL;
                dom_string *attr_val_s  = NULL;
                attr_match = 0;
                dom_string_create((const uint8_t *)sel_attr,
                        strlen(sel_attr), &attr_name_s);
                if (attr_name_s != NULL) {
                    if (macsurf_dom_element_get_attribute(
                            (dom_element *)node,
                            attr_name_s, &attr_val_s) == DOM_NO_ERR
                            && attr_val_s != NULL) {
                        if (sel_val == NULL) {
                            attr_match = 1; /* attribute present */
                        } else {
                            /* substring match */
                            const char *av = dom_string_data(attr_val_s);
                            attr_match = (av &&
                                strstr(av, sel_val) != NULL) ? 1 : 0;
                        }
                        macsurf_dom_string_unref(attr_val_s);
                    }
                    macsurf_dom_string_unref(attr_name_s);
                }
            }
            if (attr_match) {
                /* Push this element into the results array (at stack -1). */
                /* Stack before: [..., array]
                   push element: [..., array, element]
                   get_length on array (index -2), put_prop_index pops element
                   Stack after:  [..., array] */
                {
                    duk_uarridx_t len;
                    macsurf_push_element(duk, (dom_element *)node);
                    len = (duk_uarridx_t)duk_get_length(duk, -2);
                    duk_put_prop_index(duk, -2, len);
                }
            }
        }
    }

    /* Recurse into children. */
    if (macsurf_dom_node_get_first_child(node, &child) != DOM_NO_ERR)
        return;
    while (child != NULL) {
        qsa_walk(duk, child, sel_tag, sel_attr, sel_val);
        if (macsurf_dom_node_get_next_sibling(child, &sib) != DOM_NO_ERR)
            sib = NULL;
        macsurf_dom_node_unref(child);
        child = sib;
    }
}

/* ----------------------------------------------------------------- */
/* document.querySelectorAll — supports:                              */
/*   tag            e.g. "textarea"                                   */
/*   [attr]         e.g. "[data-xf-init]"                            */
/*   tag[attr]      e.g. "textarea[name]"                            */
/*   tag[attr*=v]   substring match, e.g. "textarea[name*=msg]"      */
/* Returns a JS Array (not a NodeList, but iterable the same way).   */
/* ----------------------------------------------------------------- */
static duk_ret_t
macsurf_querySelectorAll(duk_context *duk)
{
    const char *sel = duk_safe_to_string(duk, 0);
    char sel_tag[32];
    char sel_attr[64];
    char sel_val[128];
    const char *p;
    dom_element *root = NULL;
    size_t i;
    size_t j;

    /* Prepare result array. */
    duk_push_array(duk);

    if (sel == NULL || sel[0] == '\0' ||
            macsurf_js_current_document == NULL)
        return 1;

    /* Parse selector into tag / attr / val components. */
    sel_tag[0]  = '\0';
    sel_attr[0] = '\0';
    sel_val[0]  = '\0';

    p = sel;
    /* Optional tag name: letters before '[' or end. */
    if (*p != '[' && *p != '.' && *p != '#') {
        i = 0;
        while (*p && *p != '[' && i < sizeof(sel_tag) - 1)
            sel_tag[i++] = *p++;
        sel_tag[i] = '\0';
    }
    /* Optional [attr] or [attr*=val]. */
    if (*p == '[') {
        p++;
        i = 0;
        while (*p && *p != ']' && *p != '=' && *p != '*' &&
               i < sizeof(sel_attr) - 1)
            sel_attr[i++] = *p++;
        sel_attr[i] = '\0';
        if (*p == '*' && *(p+1) == '=') {
            p += 2;
            if (*p == '"' || *p == '\'') p++;
            j = 0;
            while (*p && *p != '"' && *p != '\'' && *p != ']' &&
                   j < sizeof(sel_val) - 1)
                sel_val[j++] = *p++;
            sel_val[j] = '\0';
        }
    }

    /* Walk from documentElement. */
    if (macsurf_dom_document_get_document_element(
            macsurf_js_current_document, &root) == DOM_NO_ERR
            && root != NULL) {
        qsa_walk(duk, (dom_node *)root,
                 sel_tag[0]  ? sel_tag  : NULL,
                 sel_attr[0] ? sel_attr : NULL,
                 sel_val[0]  ? sel_val  : NULL);
        macsurf_dom_node_unref((dom_node *)root);
    }

    return 1;
}

/* ----------------------------------------------------------------- */
/* document.querySelector — ID-only and tag-only support.             */
/* Supports "#foo" (getElementById) and bare tag names.  Compound     */
/* selectors fall through to null for now.                            */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_querySelector(duk_context *duk)
{
	const char *sel = duk_require_string(duk, 0);

	if (sel == NULL || sel[0] == '\0') {
		duk_push_null(duk);
		return 1;
	}

	if (sel[0] == '#') {
		/* Delegate to getElementById with the rest of the string. */
		duk_push_string(duk, sel + 1);
		duk_replace(duk, 0);
		return macsurf_getElementById(duk);
	}

	/* TODO stage 10: tag-name lookup via dom_document_get_elements_by_tag_name. */
	duk_push_null(duk);
	return 1;
}

/* ----------------------------------------------------------------- */
/* document.title — getter/setter backed by an internal string prop.  */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_document_get_title(duk_context *duk)
{
	duk_push_this(duk);
	if (duk_get_prop_string(duk, -1, "__title") == 0) {
		duk_pop(duk);
		duk_push_string(duk, "");
	}
	return 1;
}

static duk_ret_t
macsurf_document_set_title(duk_context *duk)
{
	const char *title = duk_safe_to_string(duk, 0);
	macsurf_debug_log_writef("set_title fired: '%s'",
			title ? title : "(null)");
	duk_push_this(duk);
	duk_push_string(duk, title);
	duk_put_prop_string(duk, -2, "__title");
	duk_pop(duk);
#ifdef __MACOS9__
	/* fixes320h — go through macos9_gw_set_title_from_js which sets
	 * the JS title lock so the subsequent NetSurf core reformat-time
	 * vtable call doesn't reset the title back to the HTML <title>. */
	{
		extern struct gui_window *macos9_window_list_head(void);
		extern void macos9_gw_set_title_from_js(struct gui_window *g,
				const char *t);
		struct gui_window *win = macos9_window_list_head();
		if (win == NULL) {
			MS_LOG("set_title: window_list head is NULL");
		} else if (title == NULL) {
			MS_LOG("set_title: title is NULL");
		} else {
			macsurf_debug_log_writef(
				"set_title: calling set_title_from_js win=%p",
				win);
			macos9_gw_set_title_from_js(win, title);
		}
	}
#endif
	return 0;
}

/* ----------------------------------------------------------------- */
/* console.log — routes to NSLOG.                                     */
/* ----------------------------------------------------------------- */

extern void macsurf_js_console_append(const char *line);

static duk_ret_t
macsurf_console_log(duk_context *duk)
{
	duk_idx_t n = duk_get_top(duk);
	duk_idx_t i;
	/* Concatenate args with spaces — closest to console.log semantics. */
	if (n == 0) {
		macsurf_js_console_append("");
		return 0;
	}
	duk_push_string(duk, " ");
	duk_insert(duk, 0);
	duk_join(duk, n);
	{
		const char *line = duk_safe_to_string(duk, -1);
		macsurf_js_console_append(line);
	}
	duk_pop(duk);
	return 0;
}

/* ----------------------------------------------------------------- */
/* alert(msg) — stage 8.  Shows a Mac OS 9 dialog; no-op on Linux.    */
/* ----------------------------------------------------------------- */

/*
 * alert(msg) — logs to debug channel only.  We avoid pulling Dialogs.h into
 * this TU (it cascades into MacWindows.h and AliasHandle compile
 * errors on CW8's Universal Interfaces; the Files.h + Aliases.h
 * workaround used in macos9.h is fragile here).  A real Carbon
 * NoteAlert wrapper will land in a separate file once the rest of
 * the JS path is stable.
 */
static duk_ret_t
macsurf_alert(duk_context *duk)
{
	const char *msg = duk_safe_to_string(duk, 0);
	(void)msg;
	return 0;
}

/* ----------------------------------------------------------------- */
/* setup_globals — register document / window / console / alert /    */
/* setTimeout / setInterval / clearTimeout / XMLHttpRequest on the    */
/* Duktape global object.                                             */
/* ----------------------------------------------------------------- */

/* From macsurf_js_timers.c */
extern duk_ret_t macsurf_js_settimeout(duk_context *duk);
extern duk_ret_t macsurf_js_setinterval(duk_context *duk);
extern duk_ret_t macsurf_js_cleartimeout(duk_context *duk);

/* From macsurf_js_xhr.c */
extern duk_ret_t macsurf_js_xhr_constructor(duk_context *duk);

void
macsurf_js_setup_globals(duk_context *duk)
{
	if (duk == NULL) return;

	/* window === the Duktape global object itself (alias). */
	duk_push_global_object(duk);
	duk_push_string(duk, "window");
	duk_push_global_object(duk);
	duk_def_prop(duk, -3,
			DUK_DEFPROP_HAVE_VALUE |
			DUK_DEFPROP_HAVE_WRITABLE |   DUK_DEFPROP_WRITABLE |
			DUK_DEFPROP_HAVE_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	/* fixes496 — the other standard self-references to the global object.
	 * `self` is the keystone: Froala (editor-compiled.js) wraps itself in
	 * `(function(f,b){...})(this,function(){...})` and inside resolves the
	 * global as `self`, throwing ReferenceError: identifier 'self' undefined
	 * the moment it ran (it now parses+runs after the fixes495 transpiler
	 * work). `globalThis` (ES2020), `top`, `parent`, `frames` are the same
	 * window-is-its-own-global aliases real pages assume; define them all so
	 * library bootstrap code that probes any of them succeeds. */
	{
		static const char *const g_aliases[] = {
			"self", "globalThis", "top", "parent", "frames"
		};
		int ai;
		for (ai = 0; ai < (int)(sizeof(g_aliases)/sizeof(g_aliases[0])); ai++) {
			duk_push_string(duk, g_aliases[ai]);
			duk_push_global_object(duk);
			duk_def_prop(duk, -3,
					DUK_DEFPROP_HAVE_VALUE |
					DUK_DEFPROP_HAVE_WRITABLE |   DUK_DEFPROP_WRITABLE |
					DUK_DEFPROP_HAVE_ENUMERABLE |
					DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);
		}
	}

	/* ---- document ---- */
	duk_push_object(duk);                             /* [global, document] */

	duk_push_c_function(duk, macsurf_getElementById, 1);
	duk_put_prop_string(duk, -2, "getElementById");

	duk_push_c_function(duk, macsurf_createElement, 1);
	duk_put_prop_string(duk, -2, "createElement");

	/* fixes385 (M4) — real createTextNode (overrides the macsurf_js.c JS
	 * stub via the ||-preserve order). */
	duk_push_c_function(duk, macsurf_createTextNode, 1);
	duk_put_prop_string(duk, -2, "createTextNode");

	duk_push_c_function(duk, macsurf_querySelector, 1);
	duk_put_prop_string(duk, -2, "querySelector");

	duk_push_c_function(duk, macsurf_querySelectorAll, 1);
	duk_put_prop_string(duk, -2, "querySelectorAll");

	/* title accessor */
	duk_push_string(duk, "title");
	duk_push_c_function(duk, macsurf_document_get_title, 0);
	duk_push_c_function(duk, macsurf_document_set_title, 1);
	duk_def_prop(duk, -4,
			DUK_DEFPROP_HAVE_GETTER |
			DUK_DEFPROP_HAVE_SETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	/* fixes382 (M1) — documentElement / body / head as getters returning
	 * the REAL wrapped elements (replaces the fixes379 fake stubs). */
	duk_push_string(duk, "documentElement");
	duk_push_c_function(duk, macsurf_document_get_documentElement, 0);
	duk_def_prop(duk, -3, DUK_DEFPROP_HAVE_GETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);
	duk_push_string(duk, "body");
	duk_push_c_function(duk, macsurf_document_get_body, 0);
	duk_def_prop(duk, -3, DUK_DEFPROP_HAVE_GETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);
	duk_push_string(duk, "head");
	duk_push_c_function(duk, macsurf_document_get_head, 0);
	duk_def_prop(duk, -3, DUK_DEFPROP_HAVE_GETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	duk_put_prop_string(duk, -2, "document");

	/* ---- console ---- */
	duk_push_object(duk);
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "log");
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "warn");
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "error");
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "info");
	duk_put_prop_string(duk, -2, "console");

	/* ---- alert ---- */
	duk_push_c_function(duk, macsurf_alert, 1);
	duk_put_prop_string(duk, -2, "alert");

	/* ---- timers ---- */
	duk_push_c_function(duk, macsurf_js_settimeout, 2);
	duk_put_prop_string(duk, -2, "setTimeout");
	duk_push_c_function(duk, macsurf_js_setinterval, 2);
	duk_put_prop_string(duk, -2, "setInterval");
	duk_push_c_function(duk, macsurf_js_cleartimeout, 1);
	duk_put_prop_string(duk, -2, "clearTimeout");
	duk_push_c_function(duk, macsurf_js_cleartimeout, 1);
	duk_put_prop_string(duk, -2, "clearInterval");

	/* ---- XMLHttpRequest constructor ---- */
	duk_push_c_function(duk, macsurf_js_xhr_constructor, 1);
	duk_put_prop_string(duk, -2, "XMLHttpRequest");

	duk_pop(duk); /* global object */
}

/* ----------------------------------------------------------------- */
/* Event dispatch                                                     */
/*                                                                    */
/* Stage 9 implementation:                                            */
/*   1. Inline on<event> handlers:                                    */
/*      Read the element's `on<type>` attribute via libdom and        */
/*      duk_peval_lstring it.  Backwards-compatible with classic      */
/*      "<button onclick='alert(1)'>" markup, which is exactly what   */
/*      pre-2010 sites use heavily.                                   */
/*   2. addEventListener-registered handlers:                         */
/*      Stored in __listeners[type] = [fn, fn, ...] on the wrapper.   */
/*      We pcall each in order.  Errors per-handler are isolated.     */
/*                                                                    */
/* Returns true iff at least one handler was invoked successfully.    */
/* ----------------------------------------------------------------- */

#define MACSURF_JS_WRAPPER_STASH "macsurf_el_wrappers"

static void
wrapper_stash_key(char *buf, size_t buflen, void *p)
{
	/* Use the pointer's hex form as a stash key.  Stable for the
	 * pointer's lifetime; dom_element addresses don't move. */
	unsigned long v = (unsigned long)p;
	if (buflen >= 11) {
		buf[0] = '0'; buf[1] = 'x';
		{
			int i;
			for (i = 9; i >= 2; i--) {
				int nyb = (int)(v & 0xF);
				buf[i] = (char)(nyb < 10 ? '0' + nyb : 'a' + nyb - 10);
				v >>= 4;
			}
		}
		buf[10] = '\0';
	} else if (buflen > 0) {
		buf[0] = '\0';
	}
}

static void
wrapper_register(duk_context *duk, dom_element *el, duk_idx_t wrapper_idx)
{
	char key[12];
	wrapper_stash_key(key, sizeof(key), el);

	duk_push_global_stash(duk);
	if (duk_get_prop_string(duk, -1, MACSURF_JS_WRAPPER_STASH) == 0) {
		duk_pop(duk);
		duk_push_object(duk);
		duk_dup_top(duk);
		duk_put_prop_string(duk, -3, MACSURF_JS_WRAPPER_STASH);
	}
	duk_dup(duk, wrapper_idx);
	duk_put_prop_string(duk, -2, key);
	duk_pop_2(duk);
}

static bool
wrapper_lookup_push(duk_context *duk, dom_element *el)
{
	char key[12];
	wrapper_stash_key(key, sizeof(key), el);

	duk_push_global_stash(duk);
	if (duk_get_prop_string(duk, -1, MACSURF_JS_WRAPPER_STASH) == 0) {
		duk_pop_2(duk);
		return false;
	}
	if (duk_get_prop_string(duk, -1, key) == 0) {
		duk_pop_3(duk);
		return false;
	}
	duk_remove(duk, -2); /* the wrappers stash */
	duk_remove(duk, -2); /* the global stash */
	return true;
}

bool
macsurf_js_dispatch_event(struct jscontext *ctx,
		struct dom_element *el, const char *event_type)
{
	dom_string *attr_name = NULL;
	dom_string *attr_val  = NULL;
	char attr_buf[32];
	size_t et_len;
	bool fired = false;

	if (ctx == NULL || ctx->duk == NULL || el == NULL ||
	    event_type == NULL) {
		return false;
	}

	/* --- 1. Inline on<type> handler from attribute. --- */
	et_len = strlen(event_type);
	if (et_len + 2 + 1 <= sizeof(attr_buf)) {
		attr_buf[0] = 'o'; attr_buf[1] = 'n';
		memcpy(attr_buf + 2, event_type, et_len);
		attr_buf[2 + et_len] = '\0';

		if (dom_string_create((const unsigned char *)attr_buf,
				et_len + 2, &attr_name) == DOM_NO_ERR &&
		    attr_name != NULL) {
			if (macsurf_dom_element_get_attribute(el, attr_name,
					&attr_val) == DOM_NO_ERR &&
			    attr_val != NULL) {
				int rc = duk_peval_lstring(ctx->duk,
						dom_string_data(attr_val),
						dom_string_length(attr_val));
				if (rc == 0) {
					fired = true;
				} else {
					/* fixes320i — log Duktape's error
					 * and the attr bytes so we can see
					 * why peval fails. */
					const char *em = duk_safe_to_string(
							ctx->duk, -1);
					int blen = (int)dom_string_length(
							attr_val);
					if (blen > 200) blen = 200;
					macsurf_debug_log_writef(
						"dispatch peval rc=%d err='%s'",
						rc, em ? em : "(null)");
					macsurf_debug_log_writef(
						"dispatch attr[%d]: %s",
						blen, dom_string_data(attr_val));
				}
				duk_pop(ctx->duk);
				macsurf_dom_string_unref(attr_val);
			}
			macsurf_dom_string_unref(attr_name);
		}
	}

	/* --- 2. addEventListener handlers from wrapper.__listeners. --- */
	if (wrapper_lookup_push(ctx->duk, el)) {
		if (duk_get_prop_string(ctx->duk, -1, "__listeners") &&
		    duk_get_prop_string(ctx->duk, -1, event_type) &&
		    duk_is_array(ctx->duk, -1)) {

			duk_size_t len = duk_get_length(ctx->duk, -1);
			duk_size_t i;
			for (i = 0; i < len; i++) {
				duk_get_prop_index(ctx->duk, -1, (duk_uarridx_t)i);
				if (duk_is_callable(ctx->duk, -1)) {
					duk_dup(ctx->duk, -4); /* this = wrapper */
					if (duk_pcall_method(ctx->duk, 0) == 0) {
						fired = true;
					}
				}
				duk_pop(ctx->duk);
			}
			duk_pop(ctx->duk);  /* listeners[type] */
			duk_pop(ctx->duk);  /* __listeners */
		} else {
			/* unwind whatever we pushed */
			duk_pop(ctx->duk);
			duk_pop(ctx->duk);
		}
		duk_pop(ctx->duk);          /* wrapper */
	}

	return fired;
}

/* ----------------------------------------------------------------- */
/* element.addEventListener                                           */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_addEventListener(duk_context *duk)
{
	const char *type;

	if (!duk_is_string(duk, 0) || !duk_is_function(duk, 1)) {
		return 0;
	}
	type = duk_get_string(duk, 0);

	duk_push_this(duk);
	if (duk_get_prop_string(duk, -1, "__listeners") == 0) {
		duk_pop(duk);
		duk_push_object(duk);
		duk_dup_top(duk);
		duk_put_prop_string(duk, -3, "__listeners");
	}
	/* stack: ... this listeners */
	if (duk_get_prop_string(duk, -1, type) == 0 ||
	    !duk_is_array(duk, -1)) {
		duk_pop(duk);
		duk_push_array(duk);
		duk_dup_top(duk);
		duk_put_prop_string(duk, -3, type);
	}
	/* stack: ... this listeners arr */
	{
		duk_size_t len = duk_get_length(duk, -1);
		duk_dup(duk, 1);
		duk_put_prop_index(duk, -2, (duk_uarridx_t)len);
	}
	duk_pop_3(duk);
	return 0;
}

/* Re-register macsurf_push_element to also store the wrapper in the
 * pointer-keyed stash so dispatch_event can find it later.  This is a
 * thin wrapper around the stage-4 helper; we forward-declared above. */
void
macsurf_register_element_wrapper(duk_context *duk, dom_element *el,
		duk_idx_t wrapper_idx)
{
	if (duk == NULL || el == NULL) return;
	wrapper_register(duk, el, wrapper_idx);
}
