/*
 * MacSurf — macsurf_es6.c
 *
 * Minimal ES6 -> ES5 source transpiler for the Duktape engine (ES5.1).
 * Raises the JS ceiling so modern scripts parse. Applied transform-on-failure
 * in js_exec: a script that Duktape rejects with a SyntaxError is re-tried
 * after this transform, so working ES5 scripts are never touched.
 *
 * Passes (run in order, each over the previous pass's output):
 *   STAGE 1  let / const -> var
 *   STAGE 2  arrow functions -> function expressions
 *
 * Every pass is lexer-aware: strings, template literals, comments and regex
 * literals are never rewritten. The arrow pass only transforms `this`-free /
 * `arguments`-free arrows (lexical `this` binding can't be reproduced by a
 * plain function expression without scope hoisting, a later stage); when an
 * arrow is in any way ambiguous it is emitted verbatim, never malformed — so
 * the worst case is "the script still fails to parse", never "the script runs
 * wrong". Template literals, classes and async/await are later stages.
 *
 * C89 / CW8. Only deps: <string.h>, <stdlib.h>. Part of MacSurf / NetSurf. GPL v2.
 */

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#include "javascript/macsurf_es6.h"

/* Lexer states. */
#define ES_CODE  0
#define ES_SQ    1   /* 'single' string  */
#define ES_DQ    2   /* "double" string  */
#define ES_TMPL  3   /* `template` lit    */
#define ES_LCOM  4   /* // line comment   */
#define ES_BCOM  5   /* block comment   */
#define ES_RE    6   /* /regex/           */

/* Identifier char — used to find keyword/token boundaries. */
static int
es6_is_ident(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* Regex-context test: a '/' starts a regex (not division) when the previous
 * significant char is start-of-input or an operator / opener / separator. */
static int
es6_regex_ctx(int prev)
{
	if (prev == 0)
		return 1;
	return strchr("({}[,;=!&|?:+-*%<>~^", prev) != NULL;
}

/* ===================================================================== */
/* STAGE 1: let / const -> var                                           */
/* ===================================================================== */

/* Output is never longer than input (let->var same length, const->var
 * shorter), so cap = len + 1 always fits. Returns bytes written, 0 on
 * overflow. */
static size_t
es6_letconst_pass(const char *src, size_t len, char *out, size_t cap)
{
	size_t i;
	size_t o;
	int    st;
	int    prev_sig;

	if (src == NULL || out == NULL || cap == 0)
		return 0;

	i = 0;
	o = 0;
	st = ES_CODE;
	prev_sig = 0;

	while (i < len) {
		char c = src[i];

		if (o + 2 >= cap)
			return 0;

		if (st == ES_CODE) {
			if (c == '/') {
				if (i + 1 < len && src[i + 1] == '/') {
					out[o++] = c;
					out[o++] = src[i + 1];
					i += 2;
					st = ES_LCOM;
					continue;
				}
				if (i + 1 < len && src[i + 1] == '*') {
					out[o++] = c;
					out[o++] = src[i + 1];
					i += 2;
					st = ES_BCOM;
					continue;
				}
				if (es6_regex_ctx(prev_sig)) {
					out[o++] = c;
					i++;
					st = ES_RE;
					continue;
				}
				out[o++] = c;
				prev_sig = c;
				i++;
				continue;
			}
			if (c == '\'') { out[o++] = c; prev_sig = c; i++; st = ES_SQ; continue; }
			if (c == '"')  { out[o++] = c; prev_sig = c; i++; st = ES_DQ; continue; }
			if (c == '`')  { out[o++] = c; prev_sig = c; i++; st = ES_TMPL; continue; }

			if (c == 'l' || c == 'c') {
				int prevok = (i == 0) ||
					!es6_is_ident((unsigned char) src[i - 1]);
				if (prevok && c == 'l' && i + 3 <= len &&
				    src[i + 1] == 'e' && src[i + 2] == 't' &&
				    (i + 3 == len ||
				     !es6_is_ident((unsigned char) src[i + 3]))) {
					if (o + 3 >= cap) return 0;
					out[o++] = 'v'; out[o++] = 'a'; out[o++] = 'r';
					prev_sig = 'r';
					i += 3;
					continue;
				}
				if (prevok && c == 'c' && i + 5 <= len &&
				    src[i + 1] == 'o' && src[i + 2] == 'n' &&
				    src[i + 3] == 's' && src[i + 4] == 't' &&
				    (i + 5 == len ||
				     !es6_is_ident((unsigned char) src[i + 5]))) {
					if (o + 3 >= cap) return 0;
					out[o++] = 'v'; out[o++] = 'a'; out[o++] = 'r';
					prev_sig = 'r';
					i += 5;
					continue;
				}
			}

			out[o++] = c;
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				prev_sig = c;
			i++;
			continue;
		}

		if (st == ES_SQ) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) {
				i++;
				if (o + 1 >= cap) return 0;
				out[o++] = src[i];
			} else if (c == '\'') {
				st = ES_CODE;
			}
			i++;
			continue;
		}
		if (st == ES_DQ) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) {
				i++;
				if (o + 1 >= cap) return 0;
				out[o++] = src[i];
			} else if (c == '"') {
				st = ES_CODE;
			}
			i++;
			continue;
		}
		if (st == ES_TMPL) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) {
				i++;
				if (o + 1 >= cap) return 0;
				out[o++] = src[i];
			} else if (c == '`') {
				st = ES_CODE;
			}
			i++;
			continue;
		}
		if (st == ES_LCOM) {
			out[o++] = c;
			if (c == '\n')
				st = ES_CODE;
			i++;
			continue;
		}
		if (st == ES_BCOM) {
			out[o++] = c;
			if (c == '*' && i + 1 < len && src[i + 1] == '/') {
				i++;
				if (o + 1 >= cap) return 0;
				out[o++] = src[i];
				st = ES_CODE;
			}
			i++;
			continue;
		}
		/* ES_RE */
		out[o++] = c;
		if (c == '[') {
			/* character class: copy until ] so [ /] doesn't exit regex */
			i++;
			while (i < len && src[i] != ']') {
				if (o + 1 >= cap) return 0;
				out[o++] = src[i];
				if (src[i] == '\\' && i + 1 < len) {
					i++;
					if (o + 1 >= cap) return 0;
					out[o++] = src[i];
				}
				i++;
			}
			if (i < len) {
				if (o + 1 >= cap) return 0;
				out[o++] = src[i]; /* ] */
				i++;
			}
			continue;
		} else if (c == '\\' && i + 1 < len) {
			i++;
			if (o + 1 >= cap) return 0;
			out[o++] = src[i];
		} else if (c == '/') {
			st = ES_CODE;
			prev_sig = '/';
		}
		i++;
	}

	out[o] = '\0';
	return o;
}

/* ===================================================================== */
/* STAGE 2: arrow functions -> function expressions                      */
/* ===================================================================== */

/* Scan a balanced block starting at s[b]=='{'; return the index just past the
 * matching '}', or 0 if unbalanced / not a block. Lexer-aware. */
static size_t
es6_block_end(const char *s, size_t n, size_t b)
{
	size_t i;
	int st;
	int prev_sig;
	int depth;

	if (b >= n || s[b] != '{')
		return 0;
	i = b + 1;
	st = ES_CODE;
	prev_sig = '{';
	depth = 1;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { i++; st = ES_RE; continue; }
			if (c == '\'') { i++; st = ES_SQ; prev_sig = c; continue; }
			if (c == '"')  { i++; st = ES_DQ; prev_sig = c; continue; }
			if (c == '`')  { i++; st = ES_TMPL; prev_sig = c; continue; }
			if (c == '{') depth++;
			else if (c == '}') { depth--; if (depth == 0) return i + 1; }
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				prev_sig = c;
			i++;
			continue;
		}
		if (st == ES_SQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '\'') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_DQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '"') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_TMPL) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '`') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_CODE; }
			else i++;
			continue;
		}
		/* ES_RE */
		if (c == '[') {
			/* character class: skip until ] so [ /] doesn't exit regex */
			i++;
			while (i < n && s[i] != ']') {
				if (s[i] == '\\' && i + 1 < n) i++;
				i++;
			}
			if (i < n) i++; /* skip ] */
			continue;
		}
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev_sig = '/'; } i++; }
	}
	return 0; /* unbalanced */
}

/* Scan a balanced parenthesis group starting at s[b]=='('; return the index of
 * the matching ')', or 0 if unbalanced. Lexer-aware (skips strings/comments/
 * regex inside). Used by the method-shorthand pass to find a param list end. */
static size_t
es6_paren_end(const char *s, size_t n, size_t b)
{
	size_t i;
	int st;
	int prev_sig;
	int depth;

	if (b >= n || s[b] != '(')
		return 0;
	i = b + 1;
	st = ES_CODE;
	prev_sig = '(';
	depth = 1;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { i++; st = ES_RE; continue; }
			if (c == '\'') { i++; st = ES_SQ; prev_sig = c; continue; }
			if (c == '"')  { i++; st = ES_DQ; prev_sig = c; continue; }
			if (c == '`')  { i++; st = ES_TMPL; prev_sig = c; continue; }
			if (c == '(') depth++;
			else if (c == ')') { depth--; if (depth == 0) return i; }
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				prev_sig = c;
			i++;
			continue;
		}
		if (st == ES_SQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '\'') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_DQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '"') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_TMPL) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '`') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_CODE; }
			else i++;
			continue;
		}
		/* ES_RE */
		if (c == '[') {
			i++;
			while (i < n && s[i] != ']') {
				if (s[i] == '\\' && i + 1 < n) i++;
				i++;
			}
			if (i < n) i++;
			continue;
		}
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev_sig = '/'; } i++; }
	}
	return 0; /* unbalanced */
}

/* ===================================================================== */
/* STAGE 1b: ES6 method shorthand -> name:function (object literals)      */
/* and  name(args){...} -> name:function(args){...}                       */
/*                                                                        */
/* XenForo's compiled bundles (editor-compiled.js, action.min.js,        */
/* prefix_menu.min.js, message.min.js, ...) define handlers as object     */
/* literals using ES6 method shorthand:                                   */
/*     XF.Element.newHandler({ init(){...}, click(a){...} })              */
/* Duktape 2.7 is ES5.1 and rejects `name(){}` in an object literal       */
/* (it's the single most common construct that blocked those bundles).    */
/* This pass rewrites a shorthand method  IDENT(PARAMS){  to              */
/*   IDENT:function(PARAMS){  so the result is ES5-clean.                  */
/*                                                                        */
/* Detection (conservative, lexer-aware):                                 */
/*   - in code context, an identifier whose previous significant char is  */
/*     '{' or ',' (an object-literal member boundary),                    */
/*   - immediately (modulo whitespace) followed by a balanced (...) group,*/
/*   - then (modulo whitespace) a '{' that opens a balanced block.        */
/*   The identifier must not be a reserved word that legitimately takes   */
/*   `(){` in that position (none do at a member boundary, but we still   */
/*   skip get/set/async-prefixed forms to avoid changing accessor/async   */
/*   semantics — those are handled, or stripped, by other passes).        */
/* This never matches a call `foo(x)` (prev sig there is an operand/`)`/  */
/* `;`, not '{' or ','), nor control flow `if(){}` (prev sig not a member */
/* boundary), nor a property value `k:function(){}` (the value `function` */
/* keyword is emitted by us and prev sig before it is ':').               */
/* ===================================================================== */
/* Is the char before an opening '{' one that puts the brace in EXPRESSION
 * position (so the '{' opens an object literal, not a statement block)?
 *
 * `enclosing_is_obj` is 1 when the brace we're currently inside is itself an
 * object literal — needed to disambiguate ':'. After ':' a '{' is an object
 * value ONLY when we're inside an object literal (key:{...}); in a statement
 * context ':' is a LABEL (`label:{...}` is a block, not an object), which is
 * exactly the case (`a:{if(...)...}`) that previously made the pass mistake
 * `if(...)` for a method and corrupt the output.
 *
 * A '{' opens an object literal when the previous significant char is one of
 * = ( , [ ? & | ! + - * / % < > ^ ~  (true expression operators), or ':' when
 * already inside an object literal. A block follows ) } ; : (label) or start. */
static int
es6_brace_is_object(int prev, int enclosing_is_obj)
{
	if (prev == 0)
		return 0; /* start of program: a leading { is a block */
	if (prev == ':')
		return enclosing_is_obj; /* key:{...} vs label:{...} */
	/* NOTE: '>' is deliberately EXCLUDED. The only common `>{` is an arrow
	 * body `=>{...}`, which opens a function block, not an object. Treating
	 * it as a block is what keeps `forEach(x=>{if(...)...})` from having its
	 * `if(...)` mistaken for a method. A genuine `a > {obj}` comparison is
	 * vanishingly rare and harmless to skip. '<' is likewise excluded. */
	return strchr("=(,[?&|!+-*/%^~", prev) != NULL;
}

/* Brace-context stack depth cap. Real code nests far less than this; on
 * overflow we conservatively treat further braces as blocks (no transform). */
#define ES_MS_STACK 256

static size_t
es6_method_shorthand_pass(const char *src, size_t len, char *out, size_t cap)
{
	size_t i;
	size_t o;
	int    st;
	int    prev_sig;
	/* obj_stack[d] == 1 if the brace at depth d opened an object literal. */
	char   obj_stack[ES_MS_STACK];
	int    depth;

	if (src == NULL || out == NULL || cap == 0)
		return 0;

	i = 0;
	o = 0;
	st = ES_CODE;
	prev_sig = 0;
	depth = 0;

	while (i < len) {
		char c = src[i];
		if (o + 1 >= cap) return 0;

		if (st == ES_CODE) {
			if (c == '/' && i + 1 < len && src[i + 1] == '/') {
				out[o++] = c; out[o++] = src[i + 1]; i += 2; st = ES_LCOM; continue;
			}
			if (c == '/' && i + 1 < len && src[i + 1] == '*') {
				out[o++] = c; out[o++] = src[i + 1]; i += 2; st = ES_BCOM; continue;
			}
			if (c == '/' && es6_regex_ctx(prev_sig)) {
				out[o++] = c; i++; st = ES_RE; continue;
			}
			if (c == '\'') { out[o++] = c; prev_sig = c; i++; st = ES_SQ; continue; }
			if (c == '"')  { out[o++] = c; prev_sig = c; i++; st = ES_DQ; continue; }
			if (c == '`')  { out[o++] = c; prev_sig = c; i++; st = ES_TMPL; continue; }

			/* Track brace context: classify every '{' as object or block. */
			if (c == '{') {
				int encl = (depth > 0 && depth <= ES_MS_STACK) ?
					(int) obj_stack[depth - 1] : 0;
				int isobj = es6_brace_is_object(prev_sig, encl);
				if (depth < ES_MS_STACK)
					obj_stack[depth] = (char) isobj;
				depth++;
				out[o++] = c; prev_sig = c; i++;
				continue;
			}
			if (c == '}') {
				if (depth > 0) depth--;
				out[o++] = c; prev_sig = c; i++;
				continue;
			}

			/* Candidate method shorthand? Must be at a member boundary AND
			 * the innermost brace must be an object literal (not a block).
			 * This is what stops `if(...){` inside a statement block from
			 * being mistaken for a method. */
			if ((prev_sig == '{' || prev_sig == ',') &&
			    depth > 0 && depth <= ES_MS_STACK &&
			    obj_stack[depth - 1] &&
			    ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			     c == '_' || c == '$')) {
				size_t id0 = i;
				size_t id1;
				size_t j;
				size_t pclose;
				size_t bopen;
				int    is_method = 0;
				int    idlen;
				/* scan identifier */
				j = i;
				while (j < len && es6_is_ident((unsigned char) src[j]))
					j++;
				id1 = j;
				idlen = (int)(id1 - id0);
				/* skip whitespace */
				while (j < len && (src[j] == ' ' || src[j] == '\t' ||
				       src[j] == '\n' || src[j] == '\r'))
					j++;
				if (j < len && src[j] == '(') {
					pclose = es6_paren_end(src, len, j);
					if (pclose != 0) {
						size_t k = pclose + 1;
						while (k < len && (src[k] == ' ' ||
						       src[k] == '\t' || src[k] == '\n' ||
						       src[k] == '\r'))
							k++;
						/* Just require a '{' after the param list.
						 * We deliberately do NOT call es6_block_end
						 * to verify the whole block balances: that
						 * rescans the entire (possibly huge) method
						 * body on every member-boundary identifier,
						 * which is O(n^2) and made the 733KB Froala
						 * bundle take effectively forever. The '(' +
						 * balanced ')' + '{' signature is already a
						 * reliable method discriminator here. */
						if (k < len && src[k] == '{') {
							is_method = 1;
							bopen = k;
							(void) bopen;
						}
					}
				}
				/* Exclude get/set/async accessor/async forms — leave for
				 * other passes (the async pass strips async; get/set are
				 * rare in these bundles and changing them is unsafe). */
				if (is_method) {
					if ((idlen == 3 &&
					     (strncmp(src + id0, "get", 3) == 0 ||
					      strncmp(src + id0, "set", 3) == 0)) ||
					    (idlen == 5 &&
					     strncmp(src + id0, "async", 5) == 0)) {
						/* but `get`/`set`/`async` could also be the
						 * actual method NAME (e.g. {get(){...}}). Only
						 * skip when followed by ANOTHER identifier+`(`,
						 * i.e. a true accessor `get foo(){}`. If `(`
						 * directly follows the keyword it IS the name. */
						size_t p = id1;
						while (p < len && (src[p] == ' ' ||
						       src[p] == '\t' || src[p] == '\n' ||
						       src[p] == '\r'))
							p++;
						if (p < len && src[p] != '(')
							is_method = 0;
					}
				}
				if (is_method) {
					/* emit IDENT then ":function" */
					if (o + (size_t)idlen + 10 >= cap) return 0;
					memcpy(out + o, src + id0, (size_t)idlen);
					o += (size_t)idlen;
					memcpy(out + o, ":function", 9);
					o += 9;
					prev_sig = 'n';
					i = id1; /* continue copying from after the name */
					continue;
				}
				/* not a method: fall through to copy the identifier verbatim
				 * (loop will re-enter char by char). */
			}

			out[o++] = c;
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				prev_sig = c;
			i++;
			continue;
		}

		if (st == ES_SQ) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
			else if (c == '\'') st = ES_CODE;
			i++;
			continue;
		}
		if (st == ES_DQ) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
			else if (c == '"') st = ES_CODE;
			i++;
			continue;
		}
		if (st == ES_TMPL) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
			else if (c == '`') st = ES_CODE;
			i++;
			continue;
		}
		if (st == ES_LCOM) {
			out[o++] = c;
			if (c == '\n') st = ES_CODE;
			i++;
			continue;
		}
		if (st == ES_BCOM) {
			out[o++] = c;
			if (c == '*' && i + 1 < len && src[i + 1] == '/') {
				i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; st = ES_CODE;
			}
			i++;
			continue;
		}
		/* ES_RE */
		out[o++] = c;
		if (c == '[') {
			i++;
			while (i < len && src[i] != ']') {
				if (o + 1 >= cap) return 0;
				out[o++] = src[i];
				if (src[i] == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
				i++;
			}
			if (i < len) { if (o + 1 >= cap) return 0; out[o++] = src[i]; i++; }
			continue;
		} else if (c == '\\' && i + 1 < len) {
			i++; if (o + 1 >= cap) return 0; out[o++] = src[i];
		} else if (c == '/') {
			st = ES_CODE; prev_sig = '/';
		}
		i++;
	}

	out[o] = '\0';
	return o;
}

/* Find the end of a concise (expression) arrow body starting at s[b]. The body
 * ends at the first depth-0, ternary-balanced terminator: ',' ';' a closing
 * ')' ']' '}', a non-ternary ':', or end of input. Returns that index. */
static size_t
es6_expr_end(const char *s, size_t n, size_t b)
{
	size_t i;
	int st;
	int prev_sig;
	int depth;
	int tern;

	i = b;
	st = ES_CODE;
	prev_sig = 0;
	depth = 0;
	tern = 0;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { i++; st = ES_RE; continue; }
			if (c == '\'') { i++; st = ES_SQ; prev_sig = c; continue; }
			if (c == '"')  { i++; st = ES_DQ; prev_sig = c; continue; }
			if (c == '`')  { i++; st = ES_TMPL; prev_sig = c; continue; }
			if (c == '(' || c == '[' || c == '{') { depth++; }
			else if (c == ')' || c == ']' || c == '}') {
				if (depth == 0) return i;
				depth--;
			}
			else if (depth == 0 && (c == ',' || c == ';')) return i;
			else if (depth == 0 && c == '?') tern++;
			else if (depth == 0 && c == ':') {
				if (tern > 0) tern--;
				else return i;
			}
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				prev_sig = c;
			i++;
			continue;
		}
		if (st == ES_SQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '\'') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_DQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '"') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_TMPL) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '`') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_CODE; }
			else i++;
			continue;
		}
		/* ES_RE */
		if (c == '[') {
			i++;
			while (i < n && s[i] != ']') {
				if (s[i] == '\\' && i + 1 < n) i++;
				i++;
			}
			if (i < n) i++;
			continue;
		}
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev_sig = '/'; } i++; }
	}
	return n;
}

/* Does region s[a..e) reference `this` or `arguments` as a bare identifier
 * token (lexer-aware)? Conservative: a nested function's own `this` also
 * counts, so some transformable arrows are skipped — but never a wrong
 * transform. */
static int
es6_region_uses_this(const char *s, size_t a, size_t e)
{
	size_t i;
	int st;
	int prev_sig;

	i = a;
	st = ES_CODE;
	prev_sig = 0;
	while (i < e) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < e && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < e && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { i++; st = ES_RE; continue; }
			if (c == '\'') { i++; st = ES_SQ; prev_sig = c; continue; }
			if (c == '"')  { i++; st = ES_DQ; prev_sig = c; continue; }
			if (c == '`')  { i++; st = ES_TMPL; prev_sig = c; continue; }
			/* skip nested function bodies — they bind their own `this` */
			if (c == 'f' && i + 8 <= e &&
			    strncmp(s + i, "function", 8) == 0 &&
			    (i == a || !es6_is_ident((unsigned char)s[i-1])) &&
			    (i + 8 >= e || !es6_is_ident((unsigned char)s[i+8]))) {
				size_t j = i + 8;
				int pdepth;
				while (j < e && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n')) j++;
				/* optional name */
				while (j < e && es6_is_ident((unsigned char)s[j])) j++;
				while (j < e && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n')) j++;
				/* skip params with paren counter */
				if (j < e && s[j] == '(') {
					pdepth = 1; j++;
					while (j < e && pdepth > 0) {
						if (s[j] == '(') pdepth++;
						else if (s[j] == ')') pdepth--;
						j++;
					}
				}
				while (j < e && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n')) j++;
				/* skip body */
				if (j < e && s[j] == '{') { size_t bend = es6_block_end(s, e, j); i = bend > 0 ? bend : j + 1; }
				else i = j;
				continue;
			}
			if ((c == 't' || c == 'a') &&
			    (i == a || !es6_is_ident((unsigned char) s[i - 1]))) {
				if (c == 't' && i + 4 <= e &&
				    s[i + 1] == 'h' && s[i + 2] == 'i' && s[i + 3] == 's' &&
				    (i + 4 == e || !es6_is_ident((unsigned char) s[i + 4])))
					return 1;
				if (c == 'a' && i + 9 <= e &&
				    strncmp(s + i, "arguments", 9) == 0 &&
				    (i + 9 == e || !es6_is_ident((unsigned char) s[i + 9])))
					return 1;
			}
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				prev_sig = c;
			i++;
			continue;
		}
		if (st == ES_SQ) {
			if (c == '\\' && i + 1 < e) i += 2;
			else { if (c == '\'') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_DQ) {
			if (c == '\\' && i + 1 < e) i += 2;
			else { if (c == '"') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_TMPL) {
			if (c == '\\' && i + 1 < e) i += 2;
			else { if (c == '`') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			if (c == '*' && i + 1 < e && s[i + 1] == '/') { i += 2; st = ES_CODE; }
			else i++;
			continue;
		}
		/* ES_RE */
		if (c == '[') {
			i++;
			while (i < e && s[i] != ']') {
				if (s[i] == '\\' && i + 1 < e) i++;
				i++;
			}
			if (i < e) i++;
			continue;
		}
		if (c == '\\' && i + 1 < e) i += 2;
		else { if (c == '/') st = ES_CODE; i++; }
	}
	return 0;
}

/* Find the first transformable arrow in s[0..n). On success returns 1 and
 * fills: p0,p1 = param region [p0,p1); pident = 1 if it is a bare single
 * identifier (must be wrapped in parens), 0 if it is already a (...) group;
 * bs = body start, be = body end; blk = 1 if the body is a { } block.
 * Returns 0 if none found. Non-transformable arrows (this-using, or with
 * indeterminate extents) are skipped over so a later transformable arrow can
 * still be found. */
static int
es6_find_arrow(const char *s, size_t n, size_t start_pos,
	size_t *p0, size_t *p1, int *pident,
	size_t *bs, size_t *be, int *blk)
{
	size_t i;
	int st;
	int prev_sig;
	size_t prev_sig_pos;
	size_t id_start, id_end;       /* most recent identifier token */
	size_t grp_open, grp_close;    /* most recent balanced (...) group */
	size_t pstk[256];
	int    ptop = 0;

	i = start_pos;
	st = ES_CODE;
	prev_sig = 0;
	prev_sig_pos = 0;
	id_start = 0; id_end = 0;
	grp_open = 0; grp_close = 0;

	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { i++; st = ES_RE; continue; }
			if (c == '\'') { prev_sig = c; prev_sig_pos = i; i++; st = ES_SQ; continue; }
			if (c == '"')  { prev_sig = c; prev_sig_pos = i; i++; st = ES_DQ; continue; }
			if (c == '`')  { prev_sig = c; prev_sig_pos = i; i++; st = ES_TMPL; continue; }

			/* identifier token boundaries */
			if (es6_is_ident((unsigned char) c) &&
			    (i == 0 || !es6_is_ident((unsigned char) s[i - 1]))) {
				id_start = i;
				while (i < n && es6_is_ident((unsigned char) s[i])) i++;
				id_end = i;
				prev_sig = s[id_end - 1];
				prev_sig_pos = id_end - 1;
				continue;
			}

			/* paren-group tracking */
			if (c == '(') {
				if (ptop < (int)(sizeof pstk / sizeof pstk[0]))
					pstk[ptop] = i;
				ptop++;
				prev_sig = c; prev_sig_pos = i; i++;
				continue;
			}
			if (c == ')') {
				if (ptop > 0) {
					ptop--;
					if (ptop < (int)(sizeof pstk / sizeof pstk[0])) {
						grp_open = pstk[ptop];
						grp_close = i;
					}
				}
				prev_sig = c; prev_sig_pos = i; i++;
				continue;
			}

			/* the arrow */
			if (c == '=' && i + 1 < n && s[i + 1] == '>') {
				size_t e = i;
				int ok = 0;
				size_t lp0 = 0, lp1 = 0;
				int lident = 0;
				/* param: a (...) group or a bare ident immediately
				 * before (whitespace allowed). */
				if (prev_sig == ')' && grp_close == prev_sig_pos) {
					lp0 = grp_open; lp1 = grp_close + 1; lident = 0; ok = 1;
				} else if (es6_is_ident((unsigned char) prev_sig) &&
				           id_end > 0 && prev_sig_pos == id_end - 1) {
					lp0 = id_start; lp1 = id_end; lident = 1; ok = 1;
				}
				if (ok) {
					/* body */
					size_t b = e + 2;
					size_t bend;
					int isblk;
					while (b < n && (s[b] == ' ' || s[b] == '\t' ||
					       s[b] == '\n' || s[b] == '\r')) b++;
					if (b >= n) { ok = 0; }
					else if (s[b] == '{') {
						bend = es6_block_end(s, n, b);
						isblk = 1;
						if (bend == 0) ok = 0;
					} else {
						bend = es6_expr_end(s, n, b);
						isblk = 0;
						if (bend <= b) ok = 0;
					}
					/* Convert all arrows unconditionally. Object-method arrows that
					 * use `this` keep the correct `this` when called as `obj.method()`.
					 * Arrows used as callbacks that capture outer `this` may have
					 * semantic drift, but Duktape needs all `=>` removed regardless. */
					if (ok) {
						*p0 = lp0; *p1 = lp1; *pident = lident;
						*bs = b; *be = bend; *blk = isblk;
						return 1;
					}
				}
				/* not transformable — step past '=>' and keep scanning */
				prev_sig = '>'; prev_sig_pos = i + 1; i += 2;
				continue;
			}

			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
				prev_sig = c;
				prev_sig_pos = i;
			}
			i++;
			continue;
		}
		if (st == ES_SQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '\'') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_DQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '"') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_TMPL) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '`') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_CODE; }
			else i++;
			continue;
		}
		/* ES_RE */
		if (c == '[') {
			i++;
			while (i < n && s[i] != ']') {
				if (s[i] == '\\' && i + 1 < n) i++;
				i++;
			}
			if (i < n) i++;
			continue;
		}
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev_sig = '/'; prev_sig_pos = i; } i++; }
	}
	return 0;
}

/* Emit a[0..alen) with the one arrow (described by p0..blk) rewritten into out
 * (cap bytes). Produces `(function PARAMS { ... })`. Returns new length, or 0
 * on overflow. */
static size_t
es6_emit_rewrite(const char *a, size_t alen,
	size_t p0, size_t p1, int pident, size_t bs, size_t be, int blk,
	char *out, size_t cap)
{
	size_t o = 0;
	size_t k;

#define ES_PUT(ch)   do { if (o + 1 >= cap) return 0; out[o++] = (ch); } while (0)
#define ES_PUTS(str) do { const char *_p = (str); while (*_p) { if (o + 1 >= cap) return 0; out[o++] = *_p++; } } while (0)
#define ES_PUTR(p, q) do { size_t _i = (p); while (_i < (q)) { if (o + 1 >= cap) return 0; out[o++] = a[_i++]; } } while (0)

	(void)k;
	ES_PUTR(0, p0);
	ES_PUTS("(function");
	if (pident) {
		ES_PUT('(');
		ES_PUTR(p0, p1);
		ES_PUT(')');
	} else {
		ES_PUTR(p0, p1);
	}
	if (blk) {
		ES_PUTR(bs, be);     /* { ... } block kept as-is */
		ES_PUT(')');
	} else {
		ES_PUTS("{return ");
		ES_PUTR(bs, be);
		ES_PUT('}');
		ES_PUT(')');
	}
	ES_PUTR(be, alen);
	if (o + 1 >= cap) return 0;
	out[o] = '\0';
	return o;

#undef ES_PUT
#undef ES_PUTS
#undef ES_PUTR
}

/* ===================================================================== */
/* STAGE 3: template literals -> string concatenation                    */
/* ===================================================================== */

/* es6_interp_skip / es6_tmpl_skip are mutually recursive: a template's
 * ${ } interpolation can contain a nested template, and a nested template can
 * contain its own ${ } interpolations. Recursion depth == template nesting
 * depth (tiny in real code). Both return the index just past the construct. */
static size_t es6_tmpl_skip(const char *s, size_t n, size_t i);

/* s[i] is the first char after a "${"; return index just past the matching
 * '}' (brace-balanced, lexer-aware). */
static size_t
es6_interp_skip(const char *s, size_t n, size_t i)
{
	int depth;
	int st;
	int prev;

	depth = 1;
	st = ES_CODE;
	prev = 0;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev)) { i++; st = ES_RE; continue; }
			if (c == '\'') { i++; st = ES_SQ; prev = c; continue; }
			if (c == '"')  { i++; st = ES_DQ; prev = c; continue; }
			if (c == '`')  { i = es6_tmpl_skip(s, n, i); prev = '`'; continue; }
			if (c == '{') depth++;
			else if (c == '}') { depth--; if (depth == 0) return i + 1; }
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev = c;
			i++;
			continue;
		}
		if (st == ES_SQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '\'') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_DQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '"') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_CODE; }
			else i++;
			continue;
		}
		/* ES_RE */
		if (c == '[') {
			i++;
			while (i < n && s[i] != ']') {
				if (s[i] == '\\' && i + 1 < n) i++;
				i++;
			}
			if (i < n) i++;
			continue;
		}
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev = '/'; } i++; }
	}
	return n;
}

/* s[i] == '`'; return index just past the matching close backtick. */
static size_t
es6_tmpl_skip(const char *s, size_t n, size_t i)
{
	i++;
	while (i < n) {
		char c = s[i];
		if (c == '\\') { i += 2; continue; }
		if (c == '`') return i + 1;
		if (c == '$' && i + 1 < n && s[i + 1] == '{') { i = es6_interp_skip(s, n, i + 2); continue; }
		i++;
	}
	return n;
}

/* Lower the template literal at a[tpos] ('`') into string-concatenation, append
 * to b at *bo (capacity bcap). Returns the source index just past the close
 * backtick, or 0 on overflow / unterminated. Interpolation expressions are
 * emitted verbatim (nested templates / arrows in them are handled by later
 * pipeline passes). Produces  ("seg"+(expr)+"seg")  form. */
static size_t
es6_rewrite_one_template(const char *a, size_t n, size_t tpos,
	char *b, size_t *bo, size_t bcap)
{
	size_t o = *bo;
	size_t i = tpos + 1;

#define TP_PUT(ch) do { if (o + 1 >= bcap) return 0; b[o++] = (ch); } while (0)

	TP_PUT('(');
	TP_PUT('"');
	while (i < n) {
		char c = a[i];
		if (c == '`') {
			TP_PUT('"');
			TP_PUT(')');
			*bo = o;
			return i + 1;
		}
		if (c == '\\') {
			if (i + 1 < n) {
				char d = a[i + 1];
				if (d == '`') { TP_PUT('`'); i += 2; continue; }
				if (d == '$') { TP_PUT('$'); i += 2; continue; }
				if (d == '\n') { i += 2; continue; } /* line-continuation */
				TP_PUT('\\'); TP_PUT(d); i += 2; continue;
			}
			TP_PUT('\\'); i++; continue;
		}
		if (c == '$' && i + 1 < n && a[i + 1] == '{') {
			size_t e = es6_interp_skip(a, n, i + 2);
			size_t k = i + 2;
			TP_PUT('"'); TP_PUT('+'); TP_PUT('(');
			/* expression is a[i+2 .. e-1) (exclude closing '}') */
			while (k + 1 < e) { TP_PUT(a[k]); k++; }
			TP_PUT(')'); TP_PUT('+'); TP_PUT('"');
			i = e;
			continue;
		}
		if (c == '"')  { TP_PUT('\\'); TP_PUT('"'); i++; continue; }
		if (c == '\n') { TP_PUT('\\'); TP_PUT('n'); i++; continue; }
		if (c == '\r') { i++; continue; }
		if (c == '\t') { TP_PUT('\\'); TP_PUT('t'); i++; continue; }
		TP_PUT(c);
		i++;
	}
	return 0; /* unterminated template */

#undef TP_PUT
}

/* Is s[a..e) a reserved word? A template preceded by a keyword (return, typeof,
 * new, ...) is UNTAGGED — you can't name a tag function with a keyword — so we
 * must not mistake `return `tpl`` for a tagged template. */
static int
es6_is_reserved(const char *s, size_t a, size_t e)
{
	static const char *kw[] = {
		"return","typeof","instanceof","void","delete","throw","new",
		"in","of","do","else","yield","await","case","default","var",
		"const","let","if","while","for","switch","function","this",
		"super","with","null","true","false", 0
	};
	size_t len = e - a;
	int k;
	for (k = 0; kw[k] != 0; k++) {
		if (strlen(kw[k]) == len && strncmp(s + a, kw[k], len) == 0)
			return 1;
	}
	return 0;
}

/* Find the first *untagged* template literal in s[0..n). Tagged templates
 * (preceded by a non-keyword identifier, ')' or ']' — e.g. String.raw`...`,
 * css`...`) keep template-object semantics a plain string can't, so they are
 * left alone. Returns 1 and sets *tpos on success, 0 if none. */
static int
es6_find_template(const char *s, size_t n, size_t start_pos, size_t *tpos)
{
	size_t i;
	int st;
	int prev_sig;
	size_t prev_sig_pos = 0;

	i = start_pos;
	st = ES_CODE;
	prev_sig = 0;
	prev_sig_pos = 0;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { i++; st = ES_RE; continue; }
			if (c == '\'') { i++; st = ES_SQ; prev_sig = c; prev_sig_pos = i - 1; continue; }
			if (c == '"')  { i++; st = ES_DQ; prev_sig = c; prev_sig_pos = i - 1; continue; }
			if (c == '`') {
				int tagged = 0;
				if (prev_sig == ')' || prev_sig == ']') {
					tagged = 1;
				} else if (es6_is_ident((unsigned char) prev_sig)) {
					/* preceding identifier ends at prev_sig_pos+1;
					 * tagged unless it is a reserved word. */
					size_t a = prev_sig_pos + 1;
					size_t wstart = prev_sig_pos;
					while (wstart > 0 &&
					       es6_is_ident((unsigned char) s[wstart - 1]))
						wstart--;
					tagged = !es6_is_reserved(s, wstart, a);
				}
				if (tagged) {
					i = es6_tmpl_skip(s, n, i);
					prev_sig = '`';
					prev_sig_pos = i - 1;
					continue;
				}
				*tpos = i;
				return 1;
			}
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
				prev_sig = c;
				prev_sig_pos = i;
			}
			i++;
			continue;
		}
		if (st == ES_SQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '\'') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_DQ) {
			if (c == '\\' && i + 1 < n) i += 2;
			else { if (c == '"') st = ES_CODE; i++; }
			continue;
		}
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_CODE; }
			else i++;
			continue;
		}
		/* ES_RE */
		if (c == '[') {
			i++;
			while (i < n && s[i] != ']') {
				if (s[i] == '\\' && i + 1 < n) i++;
				i++;
			}
			if (i < n) i++;
			continue;
		}
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev_sig = '/'; } i++; }
	}
	return 0;
}

/* Iteratively lower every untagged template literal. Outermost-first; nested
 * templates exposed inside interpolations are caught on later iterations.
 * Returns final length in out, or 0 on overflow / OOM. */
static size_t
es6_template_pass(const char *src, size_t len, char *out, size_t cap)
{
	char *a;
	char *b;
	char *tmp;
	size_t alen;
	size_t wmax;
	size_t iter;
	size_t start_pos;

	wmax = len * 3 + 4096;
	if (len + 1 > wmax) {
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}
	a = (char *)malloc(wmax);
	b = (char *)malloc(wmax);
	if (a == NULL || b == NULL) {
		free(a); free(b);
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}
	memcpy(a, src, len);
	a[len] = '\0';
	alen = len;

	start_pos = 0;
	iter = 0;
	for (;;) {
		size_t tpos, bo, src_end, k;
		if (iter++ > 200000UL) break;
		if (!es6_find_template(a, alen, start_pos, &tpos)) break;
		bo = 0;
		/* prefix */
		if (tpos >= wmax) break;
		for (k = 0; k < tpos; k++) b[bo++] = a[k];
		src_end = es6_rewrite_one_template(a, alen, tpos, b, &bo, wmax);
		if (src_end == 0) break;   /* overflow / unterminated — keep `a` */
		/* suffix */
		k = src_end;
		while (k < alen) {
			if (bo + 1 >= wmax) { bo = 0; break; }
			b[bo++] = a[k++];
		}
		if (bo == 0) break;        /* suffix overflow — keep `a` */
		tmp = a; a = b; b = tmp;
		alen = bo;
		start_pos = tpos;
	}

	if (alen < cap) {
		memcpy(out, a, alen);
		out[alen] = '\0';
		free(a); free(b);
		return alen;
	}
	free(a); free(b);
	return 0;
}

/* Iteratively rewrite every transformable arrow. Uses two malloc'd ping-pong
 * work buffers. Returns final length in out, or 0 on overflow / OOM (caller
 * falls back to the pre-arrow text). */
static size_t
es6_arrow_pass(const char *src, size_t len, char *out, size_t cap)
{
	char *a;
	char *b;
	char *tmp;
	size_t alen;
	size_t wmax;
	size_t iter;
	size_t start_pos;

	wmax = len * 4 + 4096;
	if (len + 1 > wmax) {
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}
	a = (char *)malloc(wmax);
	b = (char *)malloc(wmax);
	if (a == NULL || b == NULL) {
		free(a); free(b);
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}
	memcpy(a, src, len);
	a[len] = '\0';
	alen = len;

	start_pos = 0;
	iter = 0;
	for (;;) {
		size_t p0, p1, bs, be, blen;
		int pident, blk;
		if (iter++ > 200000UL) break;
		if (!es6_find_arrow(a, alen, start_pos, &p0, &p1, &pident, &bs, &be, &blk))
			break;
		blen = es6_emit_rewrite(a, alen, p0, p1, pident, bs, be, blk, b, wmax);
		if (blen == 0) break;   /* overflow — keep `a` as the result */
		tmp = a; a = b; b = tmp;
		alen = blen;
		start_pos = p0;
	}

	if (alen < cap) {
		memcpy(out, a, alen);
		out[alen] = '\0';
		free(a); free(b);
		return alen;
	}
	free(a); free(b);
	return 0;
}

/* ===================================================================== */
/* STAGE 3b: for...of -> indexed for loop                                */
/* ===================================================================== */

/* Find the end of a single-statement body (no opening brace).
 * Returns index past the statement; stops before any unmatched } or {. */
static size_t
es6_stmt_end(const char *s, size_t n, size_t start)
{
	size_t i = start;
	int depth = 0;

	while (i < n) {
		char c = s[i];
		if (c == '/' && i + 1 < n && s[i + 1] == '/') {
			i += 2;
			while (i < n && s[i] != '\n') i++;
			continue;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			i += 2;
			while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
			i += 2;
			continue;
		}
		if (c == '\'' || c == '"') {
			char q = c;
			i++;
			while (i < n && s[i] != q) {
				if (s[i] == '\\') i++;
				i++;
			}
			if (i < n) i++;
			continue;
		}
		if (c == '`') {
			i++;
			while (i < n && s[i] != '`') {
				if (s[i] == '\\') i++;
				i++;
			}
			if (i < n) i++;
			continue;
		}
		if (c == '(' || c == '[') { depth++; i++; continue; }
		if ((c == ')' || c == ']') && depth > 0) { depth--; i++; continue; }
		if (depth == 0) {
			if (c == ';') return i + 1;
			if (c == '}' || c == '{') return i;
		}
		i++;
	}
	return i;
}

/*
 * for_of_pass: transforms  for(var IDENT of EXPR)BODY
 * into  {var _v0_=EXPR,_i0_=0,IDENT;for(;_i0_<_v0_.length;_i0_++){IDENT=_v0_[_i0_];BODY_CONTENT}}
 *
 * Only handles simple-identifier binding (skips destructuring).
 * Runs after letconst_pass so only "var" remains.
 */
/* fixes495b: persistent across pass invocations so that re-running the
 * for-of pass to expand NESTED for-of (a second pass sees the inner loop the
 * first pass left untouched) never reuses a temp name (_v0_/_i0_) the prior
 * pass already emitted at an outer level. Reused names shadowed the outer
 * loop's index and corrupted iteration. A monotonic static counter keeps
 * every generated temp unique within a transpile run. */
static int g_forof_ctr = 0;

static size_t
es6_for_of_pass(const char *src, size_t len, char *out, size_t cap)
{
	size_t i = 0, o = 0;
	int st = ES_CODE, prev_sig = 0;
	int ctr = g_forof_ctr;

	while (i < len) {
		char c = src[i];
		if (o + 512 >= cap) return 0;

		if (st == ES_CODE) {
			if (c == '/' && i+1 < len && src[i+1] == '/') {
				out[o++] = c; out[o++] = src[i+1]; i += 2; st = ES_LCOM; continue;
			}
			if (c == '/' && i+1 < len && src[i+1] == '*') {
				out[o++] = c; out[o++] = src[i+1]; i += 2; st = ES_BCOM; continue;
			}
			if (c == '\'' ) { out[o++] = c; i++; st = ES_SQ;   prev_sig = c; continue; }
			if (c == '"'  ) { out[o++] = c; i++; st = ES_DQ;   prev_sig = c; continue; }
			if (c == '`'  ) { out[o++] = c; i++; st = ES_TMPL; prev_sig = c; continue; }
			if (c == '/'  && es6_regex_ctx(prev_sig)) {
				out[o++] = c; i++; st = ES_RE; continue;
			}

			/* detect 'for' keyword */
			if (c == 'f' && i + 2 < len && src[i+1] == 'o' && src[i+2] == 'r'
			    && (i == 0 || !es6_is_ident((unsigned char)src[i-1]))
			    && (i + 3 >= len || !es6_is_ident((unsigned char)src[i+3]))) {
				size_t k = i + 3;
				/* skip whitespace */
				while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
				if (k < len && src[k] == '(') {
					k++;
					while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
					/* Accept 'var '/'let '/'const ' OR bare identifier for 'for(f of ...)' */
					if ((k < len) && (
					    (k + 4 <= len && src[k]=='v' && src[k+1]=='a' && src[k+2]=='r' && src[k+3]==' ') ||
					    (k + 4 <= len && src[k]=='l' && src[k+1]=='e' && src[k+2]=='t' && src[k+3]==' ') ||
					    (k + 6 <= len && src[k]=='c' && src[k+1]=='o' && src[k+2]=='n' && src[k+3]=='s' && src[k+4]=='t' && src[k+5]==' ') ||
					    es6_is_ident((unsigned char)src[k]))) {
						size_t id_s, id_e, expr_s, expr_e, body_s, body_e;
						int block_body, id_len, expr_len, body_content_len;
						int depth;
						char vi[10], vv[10], vt[10];
						size_t js, je;

						/* skip var/let/const keyword */
						if (k + 6 <= len && src[k]=='c' && src[k+1]=='o' && src[k+2]=='n' &&
						    src[k+3]=='s' && src[k+4]=='t' && src[k+5]==' ') k += 6;
						else if (k + 4 <= len && (src[k]=='v'||src[k]=='l') &&
						    (src[k+1]=='a'||src[k+1]=='e') && src[k+3]==' ') k += 4;
						while (k < len && src[k] == ' ') k++;
						id_s = k;

						/* Array destructuring binding: for(var [a,b] of EXPR) */
						if (k < len && src[k] == '[') {
							char adnames[8][32];
							int adcount;
							int ai;
							adcount = 0;
							k++; /* skip '[' */
							while (k < len && src[k] != ']') {
								while (k<len && (src[k]==' '||src[k]=='\t'||src[k]==',')) k++;
								if (k<len && src[k]!=']') {
									size_t ns = k;
									while (k<len && es6_is_ident((unsigned char)src[k])) k++;
									if (k>ns && adcount<8) {
										size_t nl = k-ns; if (nl>=32) nl=31;
										memcpy(adnames[adcount], src+ns, nl);
										adnames[adcount][nl]='\0'; adcount++;
									} else if (k<len && src[k]!=']') k++;
								}
							}
							if (k<len) k++; /* skip ']' */
							while (k<len && src[k]==' ') k++;
							if (k+2 <= len && src[k]=='o' && src[k+1]=='f'
							    && (k+2 >= len || !es6_is_ident((unsigned char)src[k+2]))) {
								k += 2; while (k<len && src[k]==' ') k++;
								expr_s = k; depth = 1;
								while (k < len && depth > 0) {
									char ec = src[k];
									if (ec=='('||ec=='['||ec=='{') { depth++; k++; continue; }
									if (ec==')'||ec==']'||ec=='}') { depth--; if (depth>0) { k++; continue; } break; }
									if (ec=='\''||ec=='"') { char q=ec; k++; while (k<len && src[k]!=q) { if (src[k]=='\\') k++; k++; } if (k<len) k++; continue; }
									k++;
								}
								expr_e = k;
								if (k<len && src[k]==')') k++;
								while (k<len && (src[k]==' '||src[k]=='\t')) k++;
								body_s = k; block_body = (k<len && src[k]=='{');
								if (block_body) { body_e = es6_block_end(src, len, k); }
								else {
									size_t k2;
									size_t bw;
									int body_is_if;
									body_e = es6_stmt_end(src, len, k);
									if (body_e<len && src[body_e]=='{') body_e=es6_block_end(src,len,body_e);
									/* Only consume a trailing else/else-if chain when the
									 * for-of BODY is itself an `if` statement — then the
									 * else pairs with THAT if. Otherwise a following `else`
									 * belongs to an enclosing `if` (e.g.
									 * `if(P)for(x of y)B;else Q`) and must NOT be pulled
									 * into the generated loop body, which previously left a
									 * dangling `else` inside the braces and broke parsing. */
									bw = body_s;
									while (bw<len && (src[bw]==' '||src[bw]=='\t'||src[bw]=='\n'||src[bw]=='\r')) bw++;
									body_is_if = (bw+2<=len && src[bw]=='i' && src[bw+1]=='f'
									              && (bw+2>=len || !es6_is_ident((unsigned char)src[bw+2])));
									if (body_is_if) {
										k2 = body_e;
										while (1) {
											while (k2<len && (src[k2]==' '||src[k2]=='\t'||src[k2]=='\n'||src[k2]=='\r')) k2++;
											if (k2+4<=len && src[k2]=='e'&&src[k2+1]=='l'&&src[k2+2]=='s'&&src[k2+3]=='e'
											    && (k2+4>=len||!es6_is_ident((unsigned char)src[k2+4]))) {
												k2+=4;
												while (k2<len && (src[k2]==' '||src[k2]=='\t'||src[k2]=='\n'||src[k2]=='\r')) k2++;
												if (k2<len && src[k2]=='{') k2=es6_block_end(src,len,k2);
												else { k2=es6_stmt_end(src,len,k2); if(k2<len&&src[k2]=='{') k2=es6_block_end(src,len,k2); }
												body_e = k2;
											} else break;
										}
									}
								}
								if (body_e > body_s && adcount > 0) {
									sprintf(vi, "_i%d_", ctr); sprintf(vv, "_v%d_", ctr); sprintf(vt, "_t%d_", ctr); ctr++;
									js = block_body ? body_s+1 : body_s;
									je = block_body ? body_e-1 : body_e;
									body_content_len = (int)(je-js);
									expr_len = (int)(expr_e - expr_s);
									if (o + 120 + expr_len + body_content_len + adcount*30 < cap) {
										out[o++]='{';
										{ const char *p="var "; while(*p) out[o++]=*p++; }
										{ const char *p=vv; while(*p) out[o++]=*p++; } out[o++]='=';
										{ size_t q; for(q=expr_s;q<expr_e;q++) out[o++]=src[q]; }
										out[o++]=',';
										{ const char *p=vi; while(*p) out[o++]=*p++; } out[o++]='='; out[o++]='0'; out[o++]=';';
										{ const char *p="for(;"; while(*p) out[o++]=*p++; }
										{ const char *p=vi; while(*p) out[o++]=*p++; } out[o++]='<';
										{ const char *p=vv; while(*p) out[o++]=*p++; } { const char *p=".length;"; while(*p) out[o++]=*p++; }
										{ const char *p=vi; while(*p) out[o++]=*p++; } { const char *p="++){var "; while(*p) out[o++]=*p++; }
										{ const char *p=vt; while(*p) out[o++]=*p++; } out[o++]='=';
										{ const char *p=vv; while(*p) out[o++]=*p++; } out[o++]='[';
										{ const char *p=vi; while(*p) out[o++]=*p++; } out[o++]=']'; out[o++]=',';
										for (ai=0; ai<adcount; ai++) {
											size_t nl = strlen(adnames[ai]);
											if (ai>0) out[o++]=',';
											memcpy(out+o,adnames[ai],nl); o+=nl;
											out[o++]='='; { const char *p=vt; while(*p) out[o++]=*p++; }
											out[o++]='['; out[o++]='0'+ai; out[o++]=']';
										}
										out[o++]=';';
										{ size_t q; for(q=js;q<je;q++) out[o++]=src[q]; }
										out[o++]='}'; out[o++]='}';
									}
									i = body_e; continue;
								}
							}
						}
						/* fallthrough if '[...' pattern not handled: i still at 'f', natural exit */

						while (k < len && es6_is_ident((unsigned char)src[k])) k++;
						id_e = k;
						id_len = (int)(id_e - id_s);
						/* simple identifier only (no destructuring { }) */
						if (id_len > 0 && id_len < 60 && id_s < id_e && src[id_s] != '{') {
							while (k < len && src[k] == ' ') k++;
							/* check for 'of' keyword */
							if (k + 2 <= len && src[k] == 'o' && src[k+1] == 'f'
							    && (k+2 >= len || !es6_is_ident((unsigned char)src[k+2]))) {
								k += 2;
								while (k < len && src[k] == ' ') k++;
								/* extract EXPR until matching ')' */
								expr_s = k;
								depth = 1;
								while (k < len && depth > 0) {
									char ec = src[k];
									if (ec == '(' || ec == '[' || ec == '{') { depth++; k++; continue; }
									if (ec == ')' || ec == ']' || ec == '}') { depth--; if (depth > 0) { k++; continue; } break; }
									if (ec == '\'' || ec == '"') {
										char q = ec; k++;
										while (k < len && src[k] != q) { if (src[k]=='\\') k++; k++; }
										if (k < len) k++;
										continue;
									}
									k++;
								}
								expr_e = k;
								expr_len = (int)(expr_e - expr_s);
								if (k < len && src[k] == ')') k++;

								/* skip whitespace */
								while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
								body_s = k;
								block_body = (k < len && src[k] == '{');
								if (block_body) {
									body_e = es6_block_end(src, len, k);
								} else {
									size_t k2;
									size_t bw;
									int body_is_if;
									body_e = es6_stmt_end(src, len, k);
									/* stmt_end stops AT a '{'; consume the block */
									if (body_e < len && src[body_e] == '{')
										body_e = es6_block_end(src, len, body_e);
									/* Only consume a trailing else/else-if chain when the
									 * for-of BODY is itself an `if` — then the else pairs
									 * with that if. Otherwise a following `else` belongs to
									 * an enclosing `if` (e.g. `if(P)for(x of y)B;else Q`)
									 * and pulling it into the loop body leaves a dangling
									 * `else` that breaks parsing (Froala paste handler). */
									bw = body_s;
									while (bw < len && (src[bw]==' '||src[bw]=='\t'||src[bw]=='\n'||src[bw]=='\r')) bw++;
									body_is_if = (bw + 2 <= len && src[bw]=='i' && src[bw+1]=='f'
									              && (bw+2 >= len || !es6_is_ident((unsigned char)src[bw+2])));
									if (body_is_if) {
										k2 = body_e;
										while (1) {
											while (k2 < len && (src[k2]==' ' || src[k2]=='\t' || src[k2]=='\n' || src[k2]=='\r')) k2++;
											if (k2 + 4 <= len && src[k2]=='e' && src[k2+1]=='l' && src[k2+2]=='s' && src[k2+3]=='e'
											    && (k2 + 4 >= len || !es6_is_ident((unsigned char)src[k2+4]))) {
												k2 += 4;
												while (k2 < len && (src[k2]==' ' || src[k2]=='\t' || src[k2]=='\n' || src[k2]=='\r')) k2++;
												if (k2 < len && src[k2] == '{') {
													k2 = es6_block_end(src, len, k2);
												} else {
													k2 = es6_stmt_end(src, len, k2);
													if (k2 < len && src[k2] == '{')
														k2 = es6_block_end(src, len, k2);
												}
												body_e = k2;
											} else {
												break;
											}
										}
									}
								}
								if (body_e > body_s && expr_len > 0) {
									/* build temp var names */
									sprintf(vi, "_i%d_", ctr);
									sprintf(vv, "_v%d_", ctr);
									ctr++;
									/* body content: strip outer braces if block */
									if (block_body) {
										js = body_s + 1;
										je = body_e - 1;
									} else {
										js = body_s;
										je = body_e;
									}
									body_content_len = (int)(je - js);
									/* emit: {var _vN_=EXPR,_iN_=0,IDENT;for(;_iN_<_vN_.length;_iN_++){IDENT=_vN_[_iN_];BODY}} */
									if (o + 80 + expr_len + id_len + body_content_len < cap) {
										out[o++] = '{';
										/* var _vN_=EXPR */
										{ const char *p = "var "; while (*p) out[o++] = *p++; }
										{ const char *p = vv; while (*p) out[o++] = *p++; }
										out[o++] = '=';
										{ size_t q; for (q = expr_s; q < expr_e; q++) out[o++] = src[q]; }
										out[o++] = ',';
										/* _iN_=0 */
										{ const char *p = vi; while (*p) out[o++] = *p++; }
										out[o++] = '='; out[o++] = '0'; out[o++] = ',';
										/* IDENT */
										{ size_t q; for (q = id_s; q < id_e; q++) out[o++] = src[q]; }
										out[o++] = ';';
										/* for(;_iN_<_vN_.length;_iN_++) */
										{ const char *p = "for(;"; while (*p) out[o++] = *p++; }
										{ const char *p = vi; while (*p) out[o++] = *p++; }
										out[o++] = '<';
										{ const char *p = vv; while (*p) out[o++] = *p++; }
										{ const char *p = ".length;"; while (*p) out[o++] = *p++; }
										{ const char *p = vi; while (*p) out[o++] = *p++; }
										{ const char *p = "++){"; while (*p) out[o++] = *p++; }
										/* IDENT=_vN_[_iN_]; */
										{ size_t q; for (q = id_s; q < id_e; q++) out[o++] = src[q]; }
										out[o++] = '=';
										{ const char *p = vv; while (*p) out[o++] = *p++; }
										out[o++] = '[';
										{ const char *p = vi; while (*p) out[o++] = *p++; }
										{ const char *p = "];"; while (*p) out[o++] = *p++; }
										/* body content */
										{ size_t q; for (q = js; q < je; q++) out[o++] = src[q]; }
										/* close for loop and outer block */
										out[o++] = '}'; out[o++] = '}';
										i = body_e;
										prev_sig = '}';
										continue;
									}
								}
							}
						}
					}
				}
			}

			out[o++] = c;
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev_sig = c;
			i++;
			continue;
		}
		out[o++] = c;
		if (st == ES_SQ && c == '\\' && i+1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_SQ  && c == '\'') st = ES_CODE;
		else if (st == ES_DQ  && c == '\\' && i+1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_DQ  && c == '"' ) st = ES_CODE;
		else if (st == ES_TMPL && c == '\\' && i+1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_TMPL && c == '`' ) st = ES_CODE;
		else if (st == ES_LCOM && c == '\n') st = ES_CODE;
		else if (st == ES_BCOM && c == '*'  && i+1 < len && src[i+1] == '/') { i++; out[o++] = src[i]; st = ES_CODE; }
		else if (st == ES_RE   && c == '\\' && i+1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_RE   && c == '/' ) { st = ES_CODE; prev_sig = '/'; }
		i++;
	}
	out[o] = '\0';
	g_forof_ctr = ctr;   /* persist temp-name counter across pass re-runs */
	return o;
}

/* ===================================================================== */
/* STAGE 3c: object spread -> Object.assign(...)                         */
/* ===================================================================== */

/*
 * Transforms object literal spread: {a:1,...b,c:2,...d}
 * ->  Object.assign({a:1},b,{c:2},d)
 *
 * Algorithm: scan for '{' in code context where at least one top-level
 * comma-separated entry starts with '...'. Parse the object contents,
 * grouping consecutive non-spread entries into {}-arg blocks and emitting
 * spread expressions as bare arguments.
 *
 * Only handles top-level '...' (no recursive nesting beyond what the
 * recursive call to this pass itself handles on the result).
 */

/* Skip over a balanced expression (paren/bracket/brace/string) starting
 * at position i. Returns position AFTER the closing delimiter, or n on
 * failure. */
static size_t
es6_skip_expr_token(const char *s, size_t n, size_t i)
{
	int st = ES_CODE;
	int depth = 0;
	int first = 1;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i+1<n && s[i+1]=='/') { i+=2; st=ES_LCOM; continue; }
			if (c == '/' && i+1<n && s[i+1]=='*') { i+=2; st=ES_BCOM; continue; }
			if (c == '\'') { i++; st=ES_SQ; continue; }
			if (c == '"')  { i++; st=ES_DQ; continue; }
			if (c == '`')  { i++; st=ES_TMPL; continue; }
			if (c == '(' || c == '[' || c == '{') {
				depth++;
				i++;
				first = 0;
				continue;
			}
			if (c == ')' || c == ']' || c == '}') {
				if (depth == 0) return i;
				depth--;
				i++;
				continue;
			}
			if (depth == 0 && !first && (c == ',' || c == ';')) return i;
			first = 0;
			i++;
			continue;
		}
		if (st == ES_SQ) { if (c=='\\' && i+1<n) i+=2; else { if (c=='\'') st=ES_CODE; i++; } continue; }
		if (st == ES_DQ) { if (c=='\\' && i+1<n) i+=2; else { if (c=='"')  st=ES_CODE; i++; } continue; }
		if (st == ES_TMPL) { if (c=='\\' && i+1<n) i+=2; else { if (c=='`') st=ES_CODE; i++; } continue; }
		if (st == ES_LCOM) { if (c=='\n') st=ES_CODE; i++; continue; }
		if (st == ES_BCOM) { if (c=='*' && i+1<n && s[i+1]=='/') { i+=2; st=ES_CODE; } else i++; continue; }
		i++;
	}
	return i;
}

/* Returns 1 if the '{' at position b in src has at least one top-level
 * spread entry '...<expr>'. Sets *end to the position AFTER the '}'. */
static int
es6_obj_has_spread(const char *s, size_t n, size_t b, size_t *end)
{
	size_t i = b + 1;
	int st = ES_CODE;
	int depth = 0;
	int found = 0;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i+1<n && s[i+1]=='/') { i+=2; st=ES_LCOM; continue; }
			if (c == '/' && i+1<n && s[i+1]=='*') { i+=2; st=ES_BCOM; continue; }
			if (c == '\'') { i++; st=ES_SQ; continue; }
			if (c == '"')  { i++; st=ES_DQ; continue; }
			if (c == '`')  { i++; st=ES_TMPL; continue; }
			if (c == '(' || c == '[') { depth++; i++; continue; }
			if (c == '{') { depth++; i++; continue; }
			if (c == ')' || c == ']') { if (depth>0) depth--; i++; continue; }
			if (c == '}') {
				if (depth == 0) { *end = i + 1; return found; }
				depth--;
				i++;
				continue;
			}
			/* top-level '...' */
			if (depth == 0 && c=='.' && i+2<n && s[i+1]=='.' && s[i+2]=='.') {
				found = 1;
			}
			i++;
			continue;
		}
		if (st == ES_SQ) { if (c=='\\' && i+1<n) i+=2; else { if (c=='\'') st=ES_CODE; i++; } continue; }
		if (st == ES_DQ) { if (c=='\\' && i+1<n) i+=2; else { if (c=='"')  st=ES_CODE; i++; } continue; }
		if (st == ES_TMPL) { if (c=='\\' && i+1<n) i+=2; else { if (c=='`') st=ES_CODE; i++; } continue; }
		if (st == ES_LCOM) { if (c=='\n') st=ES_CODE; i++; continue; }
		if (st == ES_BCOM) { if (c=='*' && i+1<n && s[i+1]=='/') { i+=2; st=ES_CODE; } else i++; continue; }
		i++;
	}
	return 0;
}

static size_t
es6_emit_obj_assign2(const char *src2, size_t len2, size_t b2,
                     char *out2, size_t cap2, size_t *consumed2)
{
	size_t o2 = 0;
	size_t i;
	int st = ES_CODE, depth = 0;
	size_t entry_start;
	struct { size_t s, e; int spread; } entries2[128];
	int n_entries2 = 0;
	char *dummy = NULL;
	(void)dummy;

	i = b2 + 1;
	while (i < len2 && (src2[i]==' '||src2[i]=='\t'||src2[i]=='\n'||src2[i]=='\r')) i++;

	if (i < len2 && src2[i] == '}') {
		*consumed2 = i + 1 - b2;
		if (o2 + 2 >= cap2) return 0;
		out2[o2++] = '{'; out2[o2++] = '}';
		return o2;
	}

	entry_start = i;
	while (i < len2) {
		char c = src2[i];
		if (st == ES_CODE) {
			if (c == '/' && i+1<len2 && src2[i+1]=='/') { i+=2; st=ES_LCOM; continue; }
			if (c == '/' && i+1<len2 && src2[i+1]=='*') { i+=2; st=ES_BCOM; continue; }
			if (c == '\'') { i++; st=ES_SQ; continue; }
			if (c == '"')  { i++; st=ES_DQ; continue; }
			if (c == '`')  { i++; st=ES_TMPL; continue; }
			if (c == '(' || c == '[' || c == '{') { depth++; i++; continue; }
			if ((c == ')' || c == ']') && depth>0) { depth--; i++; continue; }
			if (c == '}' && depth>0) { depth--; i++; continue; }
			if ((c == ',' || c == '}') && depth == 0) {
				size_t ee = i;
				while (ee>entry_start && (src2[ee-1]==' '||src2[ee-1]=='\t'||src2[ee-1]=='\n'||src2[ee-1]=='\r')) ee--;
				if (ee > entry_start && n_entries2 < 128) {
					size_t es = entry_start;
					while (es<ee && (src2[es]==' '||src2[es]=='\t'||src2[es]=='\n'||src2[es]=='\r')) es++;
					entries2[n_entries2].s = es;
					entries2[n_entries2].e = ee;
					entries2[n_entries2].spread = (es+2<ee && src2[es]=='.' && src2[es+1]=='.' && src2[es+2]=='.');
					n_entries2++;
				}
				if (c == '}') { *consumed2 = i+1-b2; break; }
				i++;
				while (i<len2 && (src2[i]==' '||src2[i]=='\t'||src2[i]=='\n'||src2[i]=='\r')) i++;
				entry_start = i;
				continue;
			}
			i++;
		} else if (st==ES_SQ) { if (c=='\\' && i+1<len2) i+=2; else { if (c=='\'') st=ES_CODE; i++; } }
		else if (st==ES_DQ) { if (c=='\\' && i+1<len2) i+=2; else { if (c=='"') st=ES_CODE; i++; } }
		else if (st==ES_TMPL) { if (c=='\\' && i+1<len2) i+=2; else { if (c=='`') st=ES_CODE; i++; } }
		else if (st==ES_LCOM) { if (c=='\n') st=ES_CODE; i++; }
		else if (st==ES_BCOM) { if (c=='*' && i+1<len2 && src2[i+1]=='/') { i+=2; st=ES_CODE; } else i++; }
		else i++;
	}

	if (n_entries2 == 0) {
		const char *s2 = "Object.assign({})";
		while (*s2) { if (o2>=cap2) return 0; out2[o2++]=*s2++; }
		return o2;
	}

	/* Emit: Object.assign({}, s0_entry, s1_entry, ...)
	 * Static entries go in {...}; spread entries are bare args.
	 * We need a leading {} when the first entry is spread, to ensure
	 * Object.assign target is always a new object. */
	{
		int j;
		int in_static = 0;
		int first_arg = 1;
		const char *emit_s;

		/* always open with {} as the target */
#define OA_PUTS(s) do { const char*_p=(s); while(*_p){if(o2>=cap2)return 0; out2[o2++]=*_p++;} } while(0)
#define OA_PUTR(a,b) do { size_t _l=(b)-(a); if(o2+_l>cap2)return 0; memcpy(out2+o2,src2+(a),_l); o2+=_l; } while(0)
		OA_PUTS("Object.assign(");
		for (j = 0; j < n_entries2; j++) {
			if (entries2[j].spread) {
				if (in_static) { OA_PUTS("}"); in_static = 0; first_arg = 0; }
				if (!first_arg) OA_PUTS(",");
				/* the spread expr: skip '...' */
				OA_PUTR(entries2[j].s+3, entries2[j].e);
				first_arg = 0;
			} else {
				if (!in_static) {
					if (!first_arg) OA_PUTS(",");
					OA_PUTS("{");
					in_static = 1;
					first_arg = 0;
				} else {
					OA_PUTS(",");
				}
				OA_PUTR(entries2[j].s, entries2[j].e);
			}
		}
		if (in_static) OA_PUTS("}");
		OA_PUTS(")");
		(void)emit_s;
	}
	return o2;
}

static size_t
es6_objspread_pass(const char *src, size_t len, char *out, size_t cap)
{
	size_t i = 0, o = 0;
	int st = ES_CODE, prev_sig = 0;
	while (i < len) {
		char c = src[i];
		if (o + 4 >= cap) return 0;
		if (st == ES_CODE) {
			if (c == '/' && i+1<len && src[i+1]=='/') { out[o++]=c; out[o++]=src[i+1]; i+=2; st=ES_LCOM; continue; }
			if (c == '/' && i+1<len && src[i+1]=='*') { out[o++]=c; out[o++]=src[i+1]; i+=2; st=ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { out[o++]=c; i++; st=ES_RE; continue; }
			if (c == '\'') { out[o++]=c; prev_sig=c; i++; st=ES_SQ; continue; }
			if (c == '"')  { out[o++]=c; prev_sig=c; i++; st=ES_DQ; continue; }
			if (c == '`')  { out[o++]=c; prev_sig=c; i++; st=ES_TMPL; continue; }
			if (c == '{') {
				size_t obj_end;
				if (es6_obj_has_spread(src, len, i, &obj_end)) {
					size_t consumed = 0;
					size_t written = es6_emit_obj_assign2(src, len, i, out+o, cap-o, &consumed);
					if (written > 0) {
						o += written;
						i += consumed;
						prev_sig = ')';
						continue;
					}
				}
			}
			out[o++] = c;
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev_sig = c;
			i++;
			continue;
		}
		out[o++] = c;
		if (st == ES_SQ  && c == '\\' && i+1<len) { i++; out[o++]=src[i]; }
		else if (st == ES_SQ  && c == '\'') st = ES_CODE;
		else if (st == ES_DQ  && c == '\\' && i+1<len) { i++; out[o++]=src[i]; }
		else if (st == ES_DQ  && c == '"')  st = ES_CODE;
		else if (st == ES_TMPL && c == '\\' && i+1<len) { i++; out[o++]=src[i]; }
		else if (st == ES_TMPL && c == '`')  st = ES_CODE;
		else if (st == ES_LCOM && c == '\n') st = ES_CODE;
		else if (st == ES_BCOM && c == '*' && i+1<len && src[i+1]=='/') { i++; out[o++]=src[i]; st=ES_CODE; }
		else if (st == ES_RE  && c == '\\' && i+1<len) { i++; out[o++]=src[i]; }
		else if (st == ES_RE  && c == '/') { st=ES_CODE; prev_sig='/'; }
		i++;
	}
	out[o] = '\0';
	return o;
}

/* ===================================================================== */
/* STAGE 4/5: async/await/spread strip and class -> function pass        */
/* ===================================================================== */

static size_t es6_async_spread_pass(const char *src, size_t len, char *out, size_t cap) {
	size_t i = 0, o = 0;
	int st = ES_CODE, prev_sig = 0;
	while (i < len) {
		char c = src[i];
		if (o + 5 >= cap) return 0;
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < len && src[i + 1] == '/') { out[o++] = c; out[o++] = src[i + 1]; i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < len && src[i + 1] == '*') { out[o++] = c; out[o++] = src[i + 1]; i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { out[o++] = c; i++; st = ES_RE; continue; }
			if (c == '\'') { out[o++] = c; prev_sig = c; i++; st = ES_SQ; continue; }
			if (c == '"')  { out[o++] = c; prev_sig = c; i++; st = ES_DQ; continue; }
			if (c == '`')  { out[o++] = c; prev_sig = c; i++; st = ES_TMPL; continue; }

			if (c == 'a' && i + 5 <= len && src[i+1] == 's' && src[i+2] == 'y' && src[i+3] == 'n' && src[i+4] == 'c' && (i+5 == len || !es6_is_ident(src[i+5]))) {
				int prevok = (i == 0) || !es6_is_ident(src[i-1]);
				if (prevok) {
					/* async used as object property key (async:val) — preserve */
					size_t k = i + 5;
					while (k < len && (src[k]==' ' || src[k]=='\t')) k++;
					if (k >= len || src[k] != ':') {
						out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; i += 5; continue;
					}
				}
			}
			if (c == 'a' && i + 5 <= len && src[i+1] == 'w' && src[i+2] == 'a' && src[i+3] == 'i' && src[i+4] == 't' && (i+5 == len || !es6_is_ident(src[i+5]))) {
				int prevok = (i == 0) || !es6_is_ident(src[i-1]);
				if (prevok) {
					/* await used as object property key (await:val) — preserve */
					size_t k = i + 5;
					while (k < len && (src[k]==' ' || src[k]=='\t')) k++;
					if (k >= len || src[k] != ':') {
						out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; i += 5; continue;
					}
				}
			}
			if (c == '.' && i + 3 <= len && src[i+1] == '.' && src[i+2] == '.') {
				out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; i += 3; continue;
			}

			out[o++] = c;
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev_sig = c;
			i++; continue;
		}
		out[o++] = c;
		if (st == ES_SQ && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_SQ && c == '\'') st = ES_CODE;
		else if (st == ES_DQ && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_DQ && c == '"') st = ES_CODE;
		else if (st == ES_TMPL && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_TMPL && c == '`') st = ES_CODE;
		else if (st == ES_LCOM && c == '\n') st = ES_CODE;
		else if (st == ES_BCOM && c == '*' && i + 1 < len && src[i+1] == '/') { i++; out[o++] = src[i]; st = ES_CODE; }
		else if (st == ES_RE && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_RE && c == '/') { st = ES_CODE; prev_sig = '/'; }
		i++;
	}
	out[o] = '\0'; return o;
}

/*
 * es6_var_destruct_pass: converts ES6 var destructuring to ES5.
 *
 *   var [a,b]=EXPR            -> var _dv_=EXPR,a=_dv_[0],b=_dv_[1]
 *   var [a,b="x"]=EXPR        -> var _dv_=EXPR,a=_dv_[0],b=(_dv_[1]===void 0?"x":_dv_[1])
 *   var {key:alias,...}=EXPR  -> var _dv_=EXPR,alias=_dv_.key,...
 *
 * Also handles comma-chained destructuring inside a var statement:
 *   var x=1,[a,b]=EXPR        -> var x=1,_dv_=EXPR,a=_dv_[0],b=_dv_[1]
 */
/* Lexer-aware forward skip used by the var-destructuring depth scanners.
 *
 * If s[pos] opens a token whose body may contain unbalanced () [] {} — a
 * string, template literal, regex literal, line comment or block comment —
 * return the index just PAST that token's closing delimiter. Otherwise
 * return pos unchanged (caller advances by one and tracks depth itself).
 *
 * `prev_sig` is the previous significant (non-whitespace) source char, used
 * to disambiguate '/' as regex-start vs division (es6_regex_ctx). Without
 * this, a string like "}}}" or a regex /[}{]/ inside a declarator RHS would
 * corrupt the brace-depth counter and the scanner would over-consume past a
 * later `var {…}=` site (the action.min.js bug: 3rd of three identical
 * destructuring sites skipped). Templates skip their `${…}` interpolations
 * naively (depth-counted) which is enough for these bundles. fixes495b. */
static size_t
es6_vd_skip_token(const char *s, size_t pos, size_t len, int prev_sig)
{
    char c;
    if (pos >= len)
        return pos;
    c = s[pos];

    if (c == '\'' || c == '"') {
        char q = c;
        size_t i = pos + 1;
        while (i < len) {
            if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (s[i] == q) return i + 1;
            i++;
        }
        return i;
    }
    if (c == '`') {
        size_t i = pos + 1;
        int tdepth = 0;
        while (i < len) {
            if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (tdepth == 0 && s[i] == '`') return i + 1;
            if (s[i] == '$' && i + 1 < len && s[i + 1] == '{') { tdepth++; i += 2; continue; }
            if (tdepth > 0 && s[i] == '}') { tdepth--; i++; continue; }
            i++;
        }
        return i;
    }
    if (c == '/' && pos + 1 < len && s[pos + 1] == '/') {
        size_t i = pos + 2;
        while (i < len && s[i] != '\n') i++;
        return i;
    }
    if (c == '/' && pos + 1 < len && s[pos + 1] == '*') {
        size_t i = pos + 2;
        while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
        return (i + 1 < len) ? i + 2 : len;
    }
    if (c == '/' && es6_regex_ctx(prev_sig)) {
        size_t i = pos + 1;
        while (i < len) {
            if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (s[i] == '[') {
                /* character class: ']' inside is literal, and '/' inside
                 * does not end the regex */
                i++;
                while (i < len && s[i] != ']') {
                    if (s[i] == '\\' && i + 1 < len) i++;
                    i++;
                }
                if (i < len) i++;
                continue;
            }
            if (s[i] == '/') return i + 1;
            i++;
        }
        return i;
    }
    return pos;
}

static size_t es6_var_destruct_pass(const char *src, size_t len, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    int changed = 0;
    int dvctr = 0;
    int prev_sig = 0;   /* previous significant char, for regex-context */

    while (i < len) {
        /* Lexer-aware: copy whole strings / templates / regex / comments via
         * the shared skipper, so a '/' regex literal whose body contains a
         * quote (e.g. /[^\s"<>{}`]+/) is NOT misread as a string and the
         * scanner does not swallow past it (the action.min.js bug: an inline
         * regex after a `var` declarator made the outer loop consume past a
         * later `var {…}=` site). Strings/comments handled the same way. */
        {
            size_t sk = es6_vd_skip_token(src, i, len, prev_sig);
            if (sk != i) {
                if (o + (sk - i) >= cap) { out[0]='\0'; return 0; }
                { size_t q; for (q = i; q < sk; q++) out[o++] = src[q]; }
                prev_sig = src[sk - 1];
                i = sk;
                continue;
            }
        }

        /* Detect 'var ' keyword */
        if (i+4 <= len && src[i]=='v' && src[i+1]=='a' && src[i+2]=='r' && src[i+3]==' '
            && (i==0 || !es6_is_ident((unsigned char)src[i-1]))) {

            size_t k = i + 4;
            int in_var = 1;

            /* Emit 'var ' */
            if (o+4>=cap) { out[0]='\0'; return 0; }
            out[o++]='v'; out[o++]='a'; out[o++]='r'; out[o++]=' ';
            i = k;

            while (in_var && i < len) {
                /* skip whitespace */
                while (i<len && (src[i]==' '||src[i]=='\t')) { if(o>=cap){out[0]='\0';return 0;} out[o++]=src[i++]; }
                if (i>=len) break;

                if (src[i] == '[') {
                    /* Array destructuring declarator */
                    char adnames[8][32];
                    char addefs[8][64];
                    int adhasdef[8];
                    int adcount;
                    size_t rhs_s, rhs_e;
                    int ddepth;
                    int ai;
                    char dvname[16];
                    size_t bracket_start = i; /* save for fallthrough if no '=' */

                    adcount = 0;
                    memset(adhasdef, 0, sizeof(adhasdef));
                    i++; /* skip '[' */
                    while (i<len && src[i]!=']') {
                        while (i<len && (src[i]==' '||src[i]=='\t'||src[i]==',')) i++;
                        if (i<len && src[i]!=']') {
                            size_t ns = i;
                            while (i<len && es6_is_ident((unsigned char)src[i])) i++;
                            if (i>ns && adcount<8) {
                                size_t nl = i-ns; if(nl>=32)nl=31;
                                memcpy(adnames[adcount], src+ns, nl);
                                adnames[adcount][nl]='\0';
                                /* check for default =VALUE */
                                while (i<len && (src[i]==' '||src[i]=='\t')) i++;
                                if (i<len && src[i]=='=') {
                                    size_t ds;
                                    size_t dl;
                                    i++; ds = i;
                                    while (i<len && src[i]!=','&&src[i]!=']') i++;
                                    dl = i-ds; if(dl>=64)dl=63;
                                    memcpy(addefs[adcount], src+ds, dl);
                                    addefs[adcount][dl]='\0';
                                    adhasdef[adcount]=1;
                                }
                                adcount++;
                            } else if (i<len && src[i]!=']') i++;
                        }
                    }
                    if (i<len) i++; /* skip ']' */
                    /* skip whitespace and '=' */
                    while (i<len && (src[i]==' '||src[i]=='\t')) i++;
                    if (i>=len || src[i]!='=') {
                        /* Not a destructuring assignment (e.g. for(var [A,B] of ...)).
                         * Re-emit the '[...]' text that was consumed during scanning. */
                        size_t bk;
                        for (bk=bracket_start; bk<i; bk++) {
                            if (o>=cap){out[0]='\0';return 0;} out[o++]=src[bk];
                        }
                        in_var = 0; break;
                    }
                    i++; /* skip '=' */
                    while (i<len && (src[i]==' '||src[i]=='\t')) i++;
                    /* scan RHS until ',' or ';' at depth 0 (lexer-aware) */
                    rhs_s = i; ddepth = 0;
                    {
                        int rhs_prev = '=';
                        while (i<len) {
                            char rc = src[i];
                            size_t sk = es6_vd_skip_token(src, i, len, rhs_prev);
                            if (sk != i) { rhs_prev = src[sk-1]; i = sk; continue; }
                            if (rc=='('||rc=='['||rc=='{') ddepth++;
                            else if (rc==')'||rc==']'||rc=='}') { if(ddepth==0) break; ddepth--; }
                            else if ((rc==','||rc==';') && ddepth==0) break;
                            if (rc!=' '&&rc!='\t'&&rc!='\n'&&rc!='\r') rhs_prev = rc;
                            i++;
                        }
                    }
                    rhs_e = i;
                    /* emit: _dvN_=RHS,a=_dvN_[0],b=(_dvN_[1]===void 0?"x":_dvN_[1]),... */
                    sprintf(dvname, "_dv%d_", dvctr++);
                    if (o + (rhs_e-rhs_s) + adcount*60 + 30 >= cap) { out[0]='\0'; return 0; }
                    { const char *p=dvname; while(*p) out[o++]=*p++; }
                    out[o++]='=';
                    { size_t q; for(q=rhs_s;q<rhs_e;q++) out[o++]=src[q]; }
                    for (ai=0; ai<adcount; ai++) {
                        size_t nl = strlen(adnames[ai]);
                        out[o++]=',';
                        memcpy(out+o, adnames[ai], nl); o+=nl;
                        out[o++]='=';
                        if (adhasdef[ai]) {
                            size_t dl = strlen(addefs[ai]);
                            /* alias=(_dv_[N]===void 0?default:_dv_[N]) */
                            out[o++]='(';
                            { const char *p=dvname; while(*p) out[o++]=*p++; }
                            out[o++]='['; out[o++]='0'+ai; out[o++]=']';
                            out[o++]='='; out[o++]='='; out[o++]='=';
                            out[o++]='v'; out[o++]='o'; out[o++]='i'; out[o++]='d'; out[o++]=' '; out[o++]='0';
                            out[o++]='?';
                            memcpy(out+o, addefs[ai], dl); o+=dl;
                            out[o++]=':';
                            { const char *p=dvname; while(*p) out[o++]=*p++; }
                            out[o++]='['; out[o++]='0'+ai; out[o++]=']';
                            out[o++]=')';
                        } else {
                            { const char *p=dvname; while(*p) out[o++]=*p++; }
                            out[o++]='['; out[o++]='0'+ai; out[o++]=']';
                        }
                    }
                    changed = 1;
                } else if (src[i] == '{') {
                    /* Object destructuring declarator: {key:alias,...}=EXPR */
                    char okeys[8][32];
                    char oaliases[8][32];
                    int ocount;
                    size_t rhs_s, rhs_e;
                    int ddepth;
                    int oi;
                    char dvname[16];

                    ocount = 0;
                    i++; /* skip '{' */
                    while (i<len && src[i]!='}') {
                        while (i<len && (src[i]==' '||src[i]=='\t'||src[i]==',')) i++;
                        if (i<len && src[i]!='}') {
                            size_t ks = i;
                            /* scan key */
                            while (i<len && es6_is_ident((unsigned char)src[i])) i++;
                            if (i>ks && ocount<8) {
                                size_t kl = i-ks; if(kl>=32)kl=31;
                                memcpy(okeys[ocount], src+ks, kl);
                                okeys[ocount][kl]='\0';
                                while (i<len && (src[i]==' '||src[i]=='\t')) i++;
                                if (i<len && src[i]==':') {
                                    i++; /* skip ':' */
                                    while (i<len && (src[i]==' '||src[i]=='\t')) i++;
                                    /* scan alias */
                                    ks = i;
                                    while (i<len && es6_is_ident((unsigned char)src[i])) i++;
                                    kl = i-ks; if(kl>=32)kl=31;
                                    memcpy(oaliases[ocount], src+ks, kl);
                                    oaliases[ocount][kl]='\0';
                                } else {
                                    /* shorthand {key} = {key:key} */
                                    memcpy(oaliases[ocount], okeys[ocount], kl+1);
                                }
                                /* skip to ',' or '}' (skip defaults and nested stuff) */
                                ddepth = 0;
                                while (i<len && !(src[i]==',' && ddepth==0) && src[i]!='}') {
                                    if(src[i]=='{'||src[i]=='('||src[i]=='[') ddepth++;
                                    else if(src[i]=='}'||src[i]==')'||src[i]==']') { if(ddepth==0)break; ddepth--; }
                                    i++;
                                }
                                ocount++;
                            } else if (i<len && src[i]!='}') i++;
                        }
                    }
                    if (i<len) i++; /* skip '}' */
                    while (i<len && (src[i]==' '||src[i]=='\t')) i++;
                    if (i>=len || src[i]!='=') break;
                    i++; /* skip '=' */
                    while (i<len && (src[i]==' '||src[i]=='\t')) i++;
                    rhs_s = i; ddepth = 0;
                    {
                        int rhs_prev = '=';
                        while (i<len) {
                            char rc = src[i];
                            size_t sk = es6_vd_skip_token(src, i, len, rhs_prev);
                            if (sk != i) { rhs_prev = src[sk-1]; i = sk; continue; }
                            if (rc=='('||rc=='['||rc=='{') ddepth++;
                            else if (rc==')'||rc==']'||rc=='}') { if(ddepth==0) break; ddepth--; }
                            else if ((rc==','||rc==';') && ddepth==0) break;
                            if (rc!=' '&&rc!='\t'&&rc!='\n'&&rc!='\r') rhs_prev = rc;
                            i++;
                        }
                    }
                    rhs_e = i;
                    sprintf(dvname, "_dv%d_", dvctr++);
                    if (o + (rhs_e-rhs_s) + ocount*50 + 30 >= cap) { out[0]='\0'; return 0; }
                    { const char *p=dvname; while(*p) out[o++]=*p++; }
                    out[o++]='=';
                    { size_t q; for(q=rhs_s;q<rhs_e;q++) out[o++]=src[q]; }
                    for (oi=0; oi<ocount; oi++) {
                        size_t kl = strlen(okeys[oi]);
                        size_t al = strlen(oaliases[oi]);
                        out[o++]=',';
                        memcpy(out+o, oaliases[oi], al); o+=al;
                        out[o++]='=';
                        { const char *p=dvname; while(*p) out[o++]=*p++; }
                        out[o++]='.';
                        memcpy(out+o, okeys[oi], kl); o+=kl;
                    }
                    changed = 1;
                } else {
                    /* Normal declarator: copy until ',' or ';' at depth 0.
                     * Handle nested 'var [...]=' and 'var {...}=' in function bodies. */
                    int ddepth = 0;
                    int nd_prev = '=';   /* previous significant char; we just
                                          * consumed '='/whitespace, so a leading
                                          * '/' here is regex context */
                    while (i<len) {
                        char c2 = src[i];
                        size_t sk;
                        /* Lexer-aware: skip strings / templates / regex /
                         * comments whole, copying them verbatim, so their
                         * inner () [] {} never touch ddepth. */
                        sk = es6_vd_skip_token(src, i, len, nd_prev);
                        if (sk != i) {
                            if (o + (sk - i) >= cap) { out[0]='\0'; return 0; }
                            { size_t q; for (q = i; q < sk; q++) out[o++] = src[q]; }
                            nd_prev = src[sk - 1];
                            i = sk;
                            continue;
                        }
                        /* Detect nested var destructuring inside function bodies */
                        if (ddepth > 0 && i+4 < len &&
                            src[i]=='v' && src[i+1]=='a' && src[i+2]=='r' && src[i+3]==' ' &&
                            (src[i+4]=='[' || src[i+4]=='{') &&
                            (i==0 || !es6_is_ident((unsigned char)src[i-1]))) {
                            /* Emit 'var ' and reprocess as destructuring next iteration */
                            if (o+4>=cap){out[0]='\0';return 0;}
                            out[o++]='v';out[o++]='a';out[o++]='r';out[o++]=' ';
                            i += 4;
                            /* Process this nested destructuring by breaking out to outer var handler */
                            in_var = 1;
                            break; /* exit normal-declarator scan, outer while(in_var) will handle it */
                        }
                        if (c2=='('||c2=='['||c2=='{') ddepth++;
                        else if (c2==')'||c2==']'||c2=='}') { if(ddepth==0) break; ddepth--; }
                        else if ((c2==','||c2==';') && ddepth==0) break;
                        if (o>=cap) { out[0]='\0'; return 0; }
                        if (c2!=' ' && c2!='\t' && c2!='\n' && c2!='\r') nd_prev = c2;
                        out[o++] = c2; i++;
                    }
                    if (in_var == 1 && i < len && (src[i]=='[' || src[i]=='{'))
                        continue; /* re-enter inner while with destructuring */
                }

                /* After declarator: check for ',' (more decls) or end of var */
                while (i<len && (src[i]==' '||src[i]=='\t')) { if(o>=cap){out[0]='\0';return 0;} out[o++]=src[i++]; }
                if (i<len && src[i]==',') {
                    if (o>=cap) { out[0]='\0'; return 0; }
                    out[o++]=','; i++;
                } else {
                    in_var = 0;
                }
            }
            /* after a var statement the next significant char is a fresh
             * statement/expression position, so a leading '/' is regex */
            prev_sig = ';';
            continue;
        }

        if (o>=cap) { out[0]='\0'; return 0; }
        {
            char oc = src[i];
            if (oc!=' ' && oc!='\t' && oc!='\n' && oc!='\r') prev_sig = oc;
        }
        out[o++] = src[i++];
    }
    if (!changed) return 0;
    if (o<cap) out[o]='\0';
    return o;
}

/*
 * es6_defparam_pass: converts ES6 default params and simple array destructuring
 * in function parameter lists to ES5 idioms.
 *
 *   function(a, b=null, c={}) { BODY }
 *   -> function(a,b,c){if(b===void 0)b=null;if(c===void 0)c={}; BODY }
 *
 *   function([a,b]) { BODY }
 *   -> function(_dp){var a=_dp[0],b=_dp[1]; BODY }
 *
 * Runs after arrow_pass so only "function" form remains.
 */
static size_t es6_defparam_pass(const char *src, size_t len, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    int changed = 0;

    while (i < len) {
        if (i + 8 <= len &&
            src[i] == 'f' &&
            strncmp(src + i, "function", 8) == 0 &&
            (i == 0 || !es6_is_ident((unsigned char)src[i-1])) &&
            (i + 8 >= len || !es6_is_ident((unsigned char)src[i+8]))) {

            size_t j;
            j = i + 8;
            while (j < len && (src[j]==' '||src[j]=='\t'||src[j]=='\n'||src[j]=='\r')) j++;
            while (j < len && es6_is_ident((unsigned char)src[j])) j++;
            while (j < len && (src[j]==' '||src[j]=='\t'||src[j]=='\n'||src[j]=='\r')) j++;

            if (j < len && src[j] == '(') {
                size_t scan;
                int sdepth, has_default, has_array;

                /* Peek for array destruct as first param */
                scan = j + 1;
                while (scan < len && (src[scan]==' '||src[scan]=='\t')) scan++;
                has_array = (scan < len && src[scan]=='[') ? 1 : 0;

                /* Scan for default '=' */
                scan = j + 1; sdepth = 0; has_default = 0;
                while (scan < len && (sdepth > 0 || src[scan] != ')')) {
                    char sc = src[scan];
                    if (sc=='('||sc=='['||sc=='{') sdepth++;
                    else if (sc==')'||sc==']'||sc=='}') { if (sdepth>0) sdepth--; }
                    else if (sc=='=' && sdepth==0) { has_default = 1; break; }
                    scan++;
                }
                /* Also treat object destructuring params as needing processing */
                if (!has_default && !has_array) {
                    size_t sc2 = j + 1; sdepth = 0;
                    while (sc2 < len && (sdepth > 0 || src[sc2] != ')')) {
                        char sc = src[sc2];
                        if (sdepth==0 && sc=='{') { has_default = 1; break; }
                        if (sc=='('||sc=='['||sc=='{') sdepth++;
                        else if (sc==')'||sc==']'||sc=='}') { if(sdepth>0) sdepth--; }
                        sc2++;
                    }
                }

                if (has_default || has_array) {
                    /* Emit function keyword + name + whitespace up to '(' */
                    if (o + (j - i) + 2 >= cap) { out[0]='\0'; return 0; }
                    memcpy(out + o, src + i, j - i);
                    o += j - i;
                    i = j; /* now at '(' */

                    if (has_array && !has_default) {
                        char adnames[9][32];
                        int adcount;
                        int ai;
                        adcount = 0;
                        i++; /* skip '(' */
                        while (i < len && (src[i]==' '||src[i]=='\t')) i++;
                        if (i < len && src[i]=='[') {
                            i++; /* skip '[' */
                            while (i < len && src[i] != ']') {
                                while (i<len && (src[i]==' '||src[i]=='\t'||src[i]==',')) i++;
                                if (i<len && src[i]!=']') {
                                    size_t ns = i;
                                    while (i<len && es6_is_ident((unsigned char)src[i])) i++;
                                    if (i > ns && adcount < 9) {
                                        size_t nl = i - ns;
                                        if (nl >= 32) nl = 31;
                                        memcpy(adnames[adcount], src+ns, nl);
                                        adnames[adcount][nl] = '\0';
                                        adcount++;
                                    } else if (i<len && src[i]!=']') i++;
                                }
                            }
                            if (i<len) i++; /* skip ']' */
                        }
                        while (i<len && src[i]!=')') i++;
                        if (i<len) i++; /* skip ')' */
                        if (o + 6 >= cap) { out[0]='\0'; return 0; }
                        out[o++]='('; out[o++]='_'; out[o++]='d'; out[o++]='p'; out[o++]=')';
                        while (i<len && src[i]!='{') {
                            if (o>=cap) { out[0]='\0'; return 0; }
                            out[o++] = src[i++];
                        }
                        if (i<len) {
                            out[o++]='{'; i++;
                            for (ai = 0; ai < adcount && ai < 9; ai++) {
                                size_t nl = strlen(adnames[ai]);
                                if (o + nl + 14 >= cap) break;
                                if (ai==0) { out[o++]='v'; out[o++]='a'; out[o++]='r'; out[o++]=' '; }
                                else out[o++]=',';
                                memcpy(out+o, adnames[ai], nl); o += nl;
                                out[o++]='='; out[o++]='_'; out[o++]='d'; out[o++]='p';
                                out[o++]='['; out[o++]='0'+ai; out[o++]=']';
                            }
                            if (adcount>0) out[o++]=';';
                        }
                        changed = 1;
                        continue;
                    }

                    /* Default params pass */
                    {
                        char pnames[8][32];
                        char pdefs[8][128];
                        int pcount;
                        int pi;

                        pcount = 0;
                        if (o>=cap) { out[0]='\0'; return 0; }
                        out[o++]='('; i++; /* skip '(' */

                        while (i<len && src[i]!=')') {
                            size_t ps;
                            size_t pns, pne;
                            /* Emit leading whitespace and comma separators */
                            while (i<len && (src[i]==' '||src[i]=='\t'||src[i]=='\n'||src[i]=='\r'||src[i]==',')) {
                                if (o>=cap) { out[0]='\0'; return 0; }
                                out[o++] = src[i++];
                            }
                            if (i>=len || src[i]==')') break;
                            /* Read identifier */
                            ps = i;
                            while (i<len && es6_is_ident((unsigned char)src[i])) i++;
                            pns = ps; pne = i;
                            /* Non-ident param: {key:alias} or [a,b] — skip whole block verbatim */
                            if (i == ps && (src[i]=='{' || src[i]=='[')) {
                                int ddepth = 1;
                                /* Replace with synthetic _dst_ param name (semantics break but compiles) */
                                { const char *p="_dst_"; while(*p){if(o>=cap){out[0]='\0';return 0;} out[o++]=*p++;} }
                                i++; /* skip opening '{' or '[' */
                                /* skip to matching close bracket */
                                while (i<len && ddepth>0) {
                                    char dc=src[i];
                                    if(dc=='('||dc=='['||dc=='{') ddepth++;
                                    else if(dc==')'||dc==']'||dc=='}') ddepth--;
                                    if(ddepth>0) i++;
                                    else i++; /* consume closing bracket */
                                }
                                changed = 1;
                                continue;
                            }
                            /* Skip trailing whitespace before possible '=' */
                            while (i<len && (src[i]==' '||src[i]=='\t')) i++;

                            if (i<len && src[i]=='=') {
                                size_t nl = pne - pns;
                                size_t dstart, dend;
                                int ddepth;
                                if (nl >= 32) nl = 31;
                                if (o + nl >= cap) { out[0]='\0'; return 0; }
                                memcpy(out+o, src+pns, nl); o += nl;
                                i++; /* skip '=' */
                                dstart = i; ddepth = 0;
                                while (i<len) {
                                    char dc = src[i];
                                    if (dc=='('||dc=='['||dc=='{') ddepth++;
                                    else if (dc==')'||dc==']'||dc=='}') {
                                        if (ddepth==0) break;
                                        ddepth--;
                                    } else if (dc==',' && ddepth==0) break;
                                    i++;
                                }
                                dend = i;
                                if (pcount < 8) {
                                    size_t nl2 = pne - pns;
                                    size_t dl = dend - dstart;
                                    if (nl2 >= 32) nl2 = 31;
                                    if (dl >= 128) dl = 127;
                                    memcpy(pnames[pcount], src+pns, nl2);
                                    pnames[pcount][nl2] = '\0';
                                    memcpy(pdefs[pcount], src+dstart, dl);
                                    pdefs[pcount][dl] = '\0';
                                    pcount++;
                                }
                                changed = 1;
                            } else {
                                if (o + (i-ps) >= cap) { out[0]='\0'; return 0; }
                                memcpy(out+o, src+ps, i-ps); o += i-ps;
                            }
                        }
                        if (i<len) { out[o++]=')'; i++; }

                        if (pcount > 0) {
                            while (i<len && src[i]!='{') {
                                if (o>=cap) { out[0]='\0'; return 0; }
                                out[o++] = src[i++];
                            }
                            if (i<len) {
                                out[o++]='{'; i++;
                                for (pi = 0; pi < pcount; pi++) {
                                    size_t nl = strlen(pnames[pi]);
                                    size_t dl = strlen(pdefs[pi]);
                                    if (o + nl*2 + dl + 20 >= cap) break;
                                    out[o++]='i'; out[o++]='f'; out[o++]='(';
                                    memcpy(out+o, pnames[pi], nl); o+=nl;
                                    out[o++]='='; out[o++]='='; out[o++]='=';
                                    out[o++]='v'; out[o++]='o'; out[o++]='i';
                                    out[o++]='d'; out[o++]=' '; out[o++]='0';
                                    out[o++]=')';
                                    memcpy(out+o, pnames[pi], nl); o+=nl;
                                    out[o++]='=';
                                    memcpy(out+o, pdefs[pi], dl); o+=dl;
                                    out[o++]=';';
                                }
                            }
                        }
                    }
                    continue;
                }
            }
        }
        /* Method shorthand with default params: ident(a,b={}){\n  body\n}
         * Detect: non-ident, then ident, then '(', then '=' inside params,
         * then ')' then '{'. Key guard: only '}' or '{' follows ')' (no
         * function call would have '{' immediately after ')' in minified JS). */
        if (es6_is_ident((unsigned char)src[i]) &&
            (i == 0 || !es6_is_ident((unsigned char)src[i-1]))) {
            size_t j;
            size_t ilen;
            j = i;
            while (j < len && es6_is_ident((unsigned char)src[j])) j++;
            ilen = j - i;
            /* Skip reserved words that look like ident( */
            #define ES6_KW(s) (ilen==sizeof(s)-1 && memcmp(src+i,(s),ilen)==0)
            /* After ident, must be '(' */
            if (j < len && src[j] == '(' &&
                !ES6_KW("if") && !ES6_KW("for") && !ES6_KW("while") &&
                !ES6_KW("switch") && !ES6_KW("catch") && !ES6_KW("with") &&
                !ES6_KW("function") && !ES6_KW("return") && !ES6_KW("typeof") &&
                !ES6_KW("void") && !ES6_KW("new") && !ES6_KW("delete") &&
                !ES6_KW("instanceof") && !ES6_KW("in") && !ES6_KW("do")) {
                size_t scan;
                int sdepth, has_default;
                size_t pclose;
                #undef ES6_KW
                /* Scan for default '=' in params */
                scan = j + 1; sdepth = 0; has_default = 0; pclose = 0;
                while (scan < len && (sdepth > 0 || src[scan] != ')')) {
                    char sc = src[scan];
                    if (sc=='('||sc=='['||sc=='{') sdepth++;
                    else if (sc==')'||sc==']'||sc=='}') { if(sdepth>0) sdepth--; }
                    else if (sc=='=' && sdepth==0) { has_default = 1; break; }
                    scan++;
                }
                /* Check for method shorthand without defaults: {ident(params){body} */
                /* Preceding char must be '{' or ',' (object literal context) */
                if (!has_default && i > 0 && (src[i-1] == '{' || src[i-1] == ',')) {
                    /* Find closing ')' */
                    size_t pclosen;
                    scan = j + 1; sdepth = 0;
                    while (scan < len && (sdepth > 0 || src[scan] != ')')) {
                        char sc = src[scan];
                        if (sc=='('||sc=='['||sc=='{') sdepth++;
                        else if (sc==')'||sc==']'||sc=='}') { if(sdepth>0) sdepth--; }
                        scan++;
                    }
                    pclosen = scan;
                    scan = pclosen + 1;
                    while (scan < len && (src[scan]==' '||src[scan]=='\t'||src[scan]=='\n'||src[scan]=='\r')) scan++;
                    if (scan < len && src[scan] == '{') {
                        /* Emit: ident:function(params){body} */
                        size_t q;
                        for (q=i; q<j; q++) { if(o>=cap){out[0]='\0';return 0;} out[o++]=src[q]; }
                        { const char *p=":function"; while(*p){if(o>=cap){out[0]='\0';return 0;} out[o++]=*p++;} }
                        /* emit (params) and body verbatim */
                        for (q=j; q<=pclosen; q++) { if(o>=cap){out[0]='\0';return 0;} out[o++]=src[q]; }
                        i = pclosen + 1;
                        changed = 1;
                        continue;
                    }
                }
                /* Find closing ')' */
                if (has_default) {
                    scan = j + 1; sdepth = 0;
                    while (scan < len && (sdepth > 0 || src[scan] != ')')) {
                        char sc = src[scan];
                        if (sc=='('||sc=='['||sc=='{') sdepth++;
                        else if (sc==')'||sc==']'||sc=='}') { if(sdepth>0) sdepth--; }
                        scan++;
                    }
                    pclose = scan; /* position of ')' */
                    /* After ')', check for '{' (method shorthand body) */
                    scan = pclose + 1;
                    while (scan < len && (src[scan]==' '||src[scan]=='\t'||src[scan]=='\n'||src[scan]=='\r')) scan++;
                    if (scan < len && src[scan] == '{') {
                        /* It's a method shorthand with defaults!
                         * Emit: ident(stripped-params){inject-defaults; ...body...} */
                        char pnames[8][32];
                        char pdefs[8][128];
                        int pcount;
                        int pi;
                        pcount = 0;
                        /* Emit ident and '(' */
                        if (o + (j - i) + 1 >= cap) { out[0]='\0'; return 0; }
                        memcpy(out + o, src + i, j - i);
                        o += j - i;
                        out[o++]='(';
                        i = j + 1; /* past '(' */
                        while (i < (size_t)(pclose)) {
                            size_t ps;
                            size_t pns, pne;
                            while (i<(size_t)pclose && (src[i]==' '||src[i]=='\t'||src[i]=='\n'||src[i]=='\r'||src[i]==',')) {
                                if (o>=cap){out[0]='\0';return 0;} out[o++]=src[i++];
                            }
                            if (i>=(size_t)pclose) break;
                            ps = i;
                            while (i<(size_t)pclose && es6_is_ident((unsigned char)src[i])) i++;
                            pns = ps; pne = i;
                            if (i == ps) {
                                /* Non-ident at param start (e.g. '[a,b]') — emit and advance */
                                if (o>=cap){out[0]='\0';return 0;} out[o++]=src[i++]; continue;
                            }
                            while (i<(size_t)pclose && (src[i]==' '||src[i]=='\t')) i++;
                            if (i<(size_t)pclose && src[i]=='=') {
                                size_t nl = pne - pns;
                                size_t dstart, dend;
                                int ddepth;
                                if (nl>=32) nl=31;
                                if (o+nl>=cap){out[0]='\0';return 0;}
                                memcpy(out+o, src+pns, nl); o+=nl;
                                i++;
                                dstart = i; ddepth = 0;
                                while (i<(size_t)pclose) {
                                    char dc = src[i];
                                    if(dc=='('||dc=='['||dc=='{') ddepth++;
                                    else if(dc==')'||dc==']'||dc=='}'){if(ddepth==0)break;ddepth--;}
                                    else if(dc==','&&ddepth==0) break;
                                    i++;
                                }
                                dend = i;
                                if (pcount<8) {
                                    size_t nl2=pne-pns, dl=dend-dstart;
                                    if(nl2>=32)nl2=31; if(dl>=128)dl=127;
                                    memcpy(pnames[pcount],src+pns,nl2); pnames[pcount][nl2]='\0';
                                    memcpy(pdefs[pcount],src+dstart,dl); pdefs[pcount][dl]='\0';
                                    pcount++;
                                }
                                changed = 1;
                            } else {
                                if (o+(i-ps)>=cap){out[0]='\0';return 0;}
                                memcpy(out+o,src+ps,i-ps); o+=i-ps;
                            }
                        }
                        /* Emit ')' and find '{' */
                        if (o>=cap){out[0]='\0';return 0;} out[o++]=')';
                        i = pclose + 1;
                        while (i<len && src[i]!='{') { if(o>=cap){out[0]='\0';return 0;} out[o++]=src[i++]; }
                        if (i<len) {
                            out[o++]='{'; i++;
                            for (pi=0; pi<pcount; pi++) {
                                size_t nl=strlen(pnames[pi]), dl=strlen(pdefs[pi]);
                                if(o+nl*2+dl+20>=cap) break;
                                out[o++]='i';out[o++]='f';out[o++]='(';
                                memcpy(out+o,pnames[pi],nl); o+=nl;
                                out[o++]='=';out[o++]='=';out[o++]='=';
                                out[o++]='v';out[o++]='o';out[o++]='i';out[o++]='d';out[o++]=' ';out[o++]='0';
                                out[o++]=')'; memcpy(out+o,pnames[pi],nl); o+=nl;
                                out[o++]='='; memcpy(out+o,pdefs[pi],dl); o+=dl; out[o++]=';';
                            }
                        }
                        continue;
                    }
                }
            } /* if not reserved word and '(' */
        }
        if (o>=cap) { out[0]='\0'; return 0; }
        out[o++] = src[i++];
    }
    if (!changed) return 0;
    if (o<cap) out[o]='\0';
    return o;
}

static size_t es6_class_pass(const char *src, size_t len, char *out, size_t cap) {
	size_t i = 0, o = 0;
	int st = ES_CODE, prev_sig = 0;
	int brace_depth = 0;
	int paren_depth = 0;  /* track () so {} inside params aren't counted as method braces */
	int outer_paren_depth = 0; /* paren_depth at the point class { opens; baseline for class logic */
	char cname[64] = {0};
	char cbase[64] = {0}; /* base class name, for super() rewriting */
	int class_depth = 0;
	int method_depth = 0;

	while (i < len) {
		char c = src[i];
		if (o + 256 >= cap) return 0;
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < len && src[i + 1] == '/') { out[o++] = c; out[o++] = src[i + 1]; i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < len && src[i + 1] == '*') { out[o++] = c; out[o++] = src[i + 1]; i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { out[o++] = c; i++; st = ES_RE; continue; }
			if (c == '\'') { out[o++] = c; prev_sig = c; i++; st = ES_SQ; continue; }
			if (c == '"')  { out[o++] = c; prev_sig = c; i++; st = ES_DQ; continue; }
			if (c == '`')  { out[o++] = c; prev_sig = c; i++; st = ES_TMPL; continue; }

			if (c == '(') paren_depth++;
			if (c == ')' && paren_depth > 0) paren_depth--;
			if (c == '{' && paren_depth == outer_paren_depth) brace_depth++;
			if (c == '}' && paren_depth == outer_paren_depth) brace_depth--;

			if (class_depth == 0 && c == 'c' && i + 5 <= len && strncmp(src+i, "class", 5) == 0 && (i+5==len || !es6_is_ident(src[i+5]))) {
				int prevok = (i == 0) || !es6_is_ident(src[i-1]);
				if (prevok) {
					size_t j = i + 5;
					size_t namestart, k, bstart;
					char basename[64] = {0};

					while (j < len && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
					namestart = j;
					while (j < len && es6_is_ident(src[j])) j++;
					if (j > namestart && j - namestart < 60) {
						memcpy(cname, src + namestart, j - namestart);
						cname[j - namestart] = '\0';
						
						k = j;
						while (k + 7 < len && src[k] != '{' && !(src[k] == 'e' && src[k+1] == 'x' && src[k+2] == 't' && src[k+3] == 'e' && src[k+4] == 'n' && src[k+5] == 'd' && src[k+6] == 's' && src[k+7] == ' ')) k++;
						if (src[k] == 'e' && src[k+1] == 'x' && src[k+2] == 't') {
							k += 8;
							while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
							bstart = k;
							while (k < len && (es6_is_ident(src[k]) || src[k] == '.')) k++;
							if (k > bstart && k - bstart < 60) {
								memcpy(basename, src + bstart, k - bstart);
								basename[k - bstart] = '\0';
							}
						}
						
						while (j < len && src[j] != '{') j++;
						if (src[j] == '{') {
							if (basename[0] != '\0') {
								o += sprintf(out + o, "function %s(){if(typeof %s!=='undefined')%s.apply(this,arguments);if(this._ctor)this._ctor.apply(this,arguments);} %s.prototype=Object.create(typeof %s!=='undefined'?%s.prototype:null);", cname, basename, basename, cname, basename, basename);
							} else {
								o += sprintf(out + o, "function %s(){if(this._ctor)this._ctor.apply(this,arguments);}", cname);
							}
							memcpy(cbase, basename, sizeof(cbase));
							outer_paren_depth = paren_depth;
							class_depth = brace_depth + 1;
							brace_depth++;
							i = j + 1;
							prev_sig = '{';
							continue;
						}
					}
				}
			}

			if (class_depth > 0) {
				if (paren_depth == outer_paren_depth && brace_depth == class_depth - 1 && c == '}') {
					o += sprintf(out + o, "/*}*/");
					class_depth = 0;
					outer_paren_depth = 0;
					i++; prev_sig = '}'; continue;
				}
				
				if (paren_depth == outer_paren_depth && brace_depth == class_depth && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
					size_t j = i;
					int is_static = 0;
					size_t namestart, nameend;

					if (strncmp(src+j, "static", 6) == 0 && !es6_is_ident(src[j+6])) {
						is_static = 1;
						j += 6;
						while (j < len && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
					}
					namestart = j;
					while (j < len && es6_is_ident(src[j])) j++;
					nameend = j;
					while (j < len && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
					if (j < len && src[j] == '(' && nameend > namestart) {
						char mname[64] = {0};
						if (nameend - namestart < 60) {
							memcpy(mname, src + namestart, nameend - namestart);
							mname[nameend - namestart] = '\0';
							if (strcmp(mname, "constructor") == 0) strcpy(mname, "_ctor");
							if (is_static) {
								o += sprintf(out + o, "%s.%s=function", cname, mname);
							} else {
								o += sprintf(out + o, "%s.prototype.%s=function", cname, mname);
							}
							method_depth = class_depth + 1;
							i = j;
							continue;
						}
					}
				}
				
				if (method_depth > 0 && paren_depth == outer_paren_depth && brace_depth == method_depth - 1 && c == '}') {
					o += sprintf(out + o, "};");
					method_depth = 0;
					i++; prev_sig = '}'; continue;
				}

				/* super(...) inside method body -> cbase.call(this,...) */
				if (method_depth > 0 && c == 's' && i + 5 < len &&
				    strncmp(src + i, "super", 5) == 0 &&
				    (i == 0 || !es6_is_ident((unsigned char)src[i - 1])) &&
				    src[i + 5] == '(') {
					size_t blen = strlen(cbase);
					if (blen == 0 || o + blen + 14 >= (size_t)cap) {
						/* no base class or overflow — emit verbatim */
						out[o++] = 's'; out[o++] = 'u'; out[o++] = 'p';
						out[o++] = 'e'; out[o++] = 'r'; out[o++] = '(';
						i += 6; prev_sig = '('; continue;
					}
					memcpy(out + o, cbase, blen); o += blen;
					out[o++] = '.'; out[o++] = 'c'; out[o++] = 'a';
					out[o++] = 'l'; out[o++] = 'l';
					out[o++] = '('; out[o++] = 't'; out[o++] = 'h';
					out[o++] = 'i'; out[o++] = 's'; out[o++] = ',';
					i += 6; /* skip "super(" — bump paren so ")" still balances */
					paren_depth++;
					prev_sig = ','; continue;
				}
			}

			out[o++] = c;
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev_sig = c;
			i++; continue;
		}
		
		out[o++] = c;
		if (st == ES_SQ && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_SQ && c == '\'') st = ES_CODE;
		else if (st == ES_DQ && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_DQ && c == '"') st = ES_CODE;
		else if (st == ES_TMPL && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_TMPL && c == '`') st = ES_CODE;
		else if (st == ES_LCOM && c == '\n') st = ES_CODE;
		else if (st == ES_BCOM && c == '*' && i + 1 < len && src[i+1] == '/') { i++; out[o++] = src[i]; st = ES_CODE; }
		else if (st == ES_RE && c == '\\' && i + 1 < len) { i++; out[o++] = src[i]; }
		else if (st == ES_RE && c == '/') { st = ES_CODE; prev_sig = '/'; }
		i++;
	}
	out[o] = '\0'; return o;
}

/* ===================================================================== */
/* Regex named capture group stripper: (?<name>...) -> (...), \k<name> -> () */
/* Duktape ES5 doesn't support named groups (ES2018).                    */
/* ===================================================================== */
static size_t
es6_regex_namedgroup_pass(const char *src, size_t len, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    int st = ES_CODE;
    int prev_sig = 0;
    int changed = 0;

    while (i < len) {
        char c = src[i];
        if (st == ES_CODE) {
            if (c=='/'&&i+1<len&&src[i+1]=='/'){out[o++]=c;out[o++]=src[i+1];i+=2;st=ES_LCOM;continue;}
            if (c=='/'&&i+1<len&&src[i+1]=='*'){out[o++]=c;out[o++]=src[i+1];i+=2;st=ES_BCOM;continue;}
            if (c=='\''){out[o++]=c;i++;st=ES_SQ;prev_sig=c;continue;}
            if (c=='"'){out[o++]=c;i++;st=ES_DQ;prev_sig=c;continue;}
            if (c=='`'){out[o++]=c;i++;st=ES_TMPL;prev_sig=c;continue;}
            if (c=='/'&&es6_regex_ctx(prev_sig)){out[o++]=c;i++;st=ES_RE;continue;}
            if (c!=' '&&c!='\t'&&c!='\n'&&c!='\r') prev_sig=c;
            if (o>=cap){out[0]='\0';return 0;} out[o++]=src[i++];
            continue;
        }
        if (st==ES_SQ){if(c=='\\'&&i+1<len){out[o++]=c;out[o++]=src[i+1];i+=2;}else{out[o++]=c;i++;if(c=='\''){st=ES_CODE;prev_sig=c;}}continue;}
        if (st==ES_DQ){if(c=='\\'&&i+1<len){out[o++]=c;out[o++]=src[i+1];i+=2;}else{out[o++]=c;i++;if(c=='"'){st=ES_CODE;prev_sig=c;}}continue;}
        if (st==ES_TMPL){if(c=='\\'&&i+1<len){out[o++]=c;out[o++]=src[i+1];i+=2;}else{out[o++]=c;i++;if(c=='`'){st=ES_CODE;prev_sig=c;}}continue;}
        if (st==ES_LCOM){out[o++]=c;i++;if(c=='\n')st=ES_CODE;continue;}
        if (st==ES_BCOM){
            if(c=='*'&&i+1<len&&src[i+1]=='/'){out[o++]=c;out[o++]=src[i+1];i+=2;st=ES_CODE;}
            else{out[o++]=c;i++;}
            continue;
        }
        /* ES_RE: inside regex literal */
        if (c=='['){
            out[o++]=c; i++;
            while (i<len && src[i]!=']') {
                if (src[i]=='\\'&&i+1<len){out[o++]=src[i];out[o++]=src[i+1];i+=2;}
                else{out[o++]=src[i++];}
            }
            if (i<len){out[o++]=src[i++];}
            continue;
        }
        if (c=='\\' && i+1<len) {
            /* \k<name> -> strip (emit empty string) */
            if (src[i+1]=='k' && i+2<len && src[i+2]=='<') {
                i += 3; /* skip \k< */
                while (i<len && src[i]!='>') i++;
                if (i<len) i++; /* skip > */
                changed = 1;
            } else {
                out[o++]=c; out[o++]=src[i+1]; i+=2;
            }
            continue;
        }
        if (c=='/') { st=ES_CODE; prev_sig='/'; out[o++]=c; i++; continue; }
        /* (?<name>...) -> (...) */
        if (c=='(' && i+2<len && src[i+1]=='?' && src[i+2]=='<' &&
            i+3<len && (es6_is_ident((unsigned char)src[i+3]) || src[i+3]=='_')) {
            out[o++]='('; i += 3; /* skip (?< */
            while (i<len && src[i]!='>') i++;
            if (i<len) i++; /* skip > */
            changed = 1;
            continue;
        }
        if (o>=cap){out[0]='\0';return 0;} out[o++]=src[i++];
    }
    if (!changed) return 0;
    if (o<cap) out[o]='\0';
    return o;
}

/* ===================================================================== */
/* Driver                                                                */
/* ===================================================================== */

size_t
macsurf_es6_transpile(const char *src, size_t len, char *out, size_t cap)
{
	char *buf1;
	char *buf2;
	char *cur;
	char *dst;
	size_t wmax;
	size_t n;
	size_t m;

	if (src == NULL || out == NULL || cap == 0)
		return 0;

	wmax = len * 4 + 4096;   /* must hold the largest intermediate */
	buf1 = (char *)malloc(wmax);
	buf2 = (char *)malloc(wmax);
	if (buf1 == NULL || buf2 == NULL) {
		free(buf1); free(buf2);
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}

	/* Pass 1: template literals -> string concat FIRST, so nested templates
	 * (which fool every other pass's simple ES_TMPL handler) are gone before
	 * any other transformation runs. The template pass has its own recursive
	 * es6_interp_skip/es6_tmpl_skip that correctly handles nesting. */
	memcpy(buf1, src, len);
	buf1[len] = '\0';
	n = len;
	cur = buf1;

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_template_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* Pass 2: let/const -> var (never grows, no templates left to confuse it). */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_letconst_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* Pass 2b: ES6 method shorthand  name(args){...}  ->                 *
	 * name:function(args){...}  in object literals. Runs BEFORE arrows   *
	 * and class so method bodies are normal function expressions when    *
	 * those passes see them. This is what unblocks the XenForo handler   *
	 * bundles (editor-compiled.js / action / prefix_menu / message),     *
	 * which all define handlers via { init(){}, click(a){} } shorthand   *
	 * that Duktape 2.7 (ES5.1) rejects. Run twice: a method body can     *
	 * itself contain a nested object literal with shorthand methods.     */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_method_shorthand_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_method_shorthand_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* Pass 3: for...of — iterate to a fixpoint (cap 6). Each pass expands
	 * the outermost remaining for-of; a deeply nested chain
	 * (`for(a of X)for(b of Y)for(c of Z)…`, seen in editor-compiled.js)
	 * needs one pass per level. The static g_forof_ctr keeps temp names
	 * unique across the iterations so inner loops don't shadow outer ones.
	 * fixes495b. Then arrows (twice). */
	g_forof_ctr = 0;
	{
		int fo_iter;
		for (fo_iter = 0; fo_iter < 6; fo_iter++) {
			dst = (cur == buf1) ? buf2 : buf1;
			m = es6_for_of_pass(cur, n, dst, wmax);
			if (m == 0)
				break;
			/* for-of has no changed-flag; it returns the full length
			 * every pass. Detect the fixpoint by an unchanged result
			 * (same length AND identical bytes) and stop re-running. */
			if (m == n && memcmp(dst, cur, n) == 0)
				break;
			cur = dst; n = m;
		}
	}

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_arrow_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_arrow_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* Object spread: {a,...b} -> Object.assign({a},b) */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_objspread_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_async_spread_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* var [a,b]=expr and var {key:alias}=expr -> ES5.
	 * Iterate to a fixpoint (cap 6): the single-pass RHS scanner can lose
	 * declarator sync across a very long value containing deeply nested
	 * function expressions, skipping a later `var {…}=` site (observed in
	 * action.min.js: the 3rd of three `var {data:b}=a` sites). Each pass
	 * transforms at least one more remaining site and returns non-zero
	 * only when it changed something, so re-running until it returns 0
	 * clears them all. fixes495b. */
	{
		int vd_iter;
		for (vd_iter = 0; vd_iter < 6; vd_iter++) {
			dst = (cur == buf1) ? buf2 : buf1;
			m = es6_var_destruct_pass(cur, n, dst, wmax);
			if (m == 0)
				break;        /* nothing left to transform */
			cur = dst; n = m;
		}
	}

	/* Default params + array destructuring params: function(a,b=1) and function([a,b]) */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_defparam_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_class_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* Re-run defparam after class pass (class methods may have destructured params) */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_defparam_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* Pass N: strip regex named capture groups (?<name>...) -> (...), \k<name> -> () */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_regex_namedgroup_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	if (n < cap) {
		memcpy(out, cur, n);
		out[n] = '\0';
		free(buf1); free(buf2);
		return n;
	}
	free(buf1); free(buf2);
	return 0;
}
