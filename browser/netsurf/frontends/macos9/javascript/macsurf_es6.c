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
	return strchr("(){}[],;=!&|?:+-*%<>~^", prev) != NULL;
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
		if (c == '\\' && i + 1 < len) {
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
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev_sig = '/'; } i++; }
	}
	return 0; /* unbalanced */
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
es6_find_arrow(const char *s, size_t n,
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
	int    ptop;

	i = 0;
	st = ES_CODE;
	prev_sig = 0;
	prev_sig_pos = 0;
	id_start = 0; id_end = 0;
	grp_open = 0; grp_close = 0;
	ptop = 0;

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
					if (ok && !es6_region_uses_this(s, b, bend)) {
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
es6_find_template(const char *s, size_t n, size_t *tpos)
{
	size_t i;
	int st;
	int prev_sig;
	size_t prev_sig_pos;

	i = 0;
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

	iter = 0;
	for (;;) {
		size_t tpos, bo, src_end, k;
		if (iter++ > 200000UL) break;
		if (!es6_find_template(a, alen, &tpos)) break;
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
/* STAGE 4: async / await (parse-level)                                  */
/* ===================================================================== */

/* s[pos] == '('; return index just past the matching ')', lexer-aware, or 0
 * on imbalance. */
static size_t
es6_skip_parens(const char *s, size_t n, size_t pos)
{
	size_t i;
	int st;
	int prev;
	int depth;
	if (pos >= n || s[pos] != '(') return 0;
	i = pos + 1;
	st = ES_CODE;
	prev = '(';
	depth = 1;
	while (i < n) {
		char c = s[i];
		if (st == ES_CODE) {
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev)) { i++; st = ES_RE; continue; }
			if (c == '\'') { i++; st = ES_SQ; prev = c; continue; }
			if (c == '"')  { i++; st = ES_DQ; prev = c; continue; }
			if (c == '`')  { i++; st = ES_TMPL; prev = c; continue; }
			if (c == '(') depth++;
			else if (c == ')') { depth--; if (depth == 0) return i + 1; }
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev = c;
			i++;
			continue;
		}
		if (st == ES_SQ) { if (c == '\\' && i + 1 < n) i += 2; else { if (c == '\'') st = ES_CODE; i++; } continue; }
		if (st == ES_DQ) { if (c == '\\' && i + 1 < n) i += 2; else { if (c == '"') st = ES_CODE; i++; } continue; }
		if (st == ES_TMPL) { if (c == '\\' && i + 1 < n) i += 2; else { if (c == '`') st = ES_CODE; i++; } continue; }
		if (st == ES_LCOM) { if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) { if (c == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; st = ES_CODE; } else i++; continue; }
		/* ES_RE */
		if (c == '\\' && i + 1 < n) i += 2;
		else { if (c == '/') { st = ES_CODE; prev = '/'; } i++; }
	}
	return 0;
}

/* Strip the `async` keyword (before a function or arrow) and the `await`
 * unary keyword, so async source at least PARSES and runs synchronously under
 * Duktape ES5.1 (the parse gate FB's bootstrap trips on). This is parse-level
 * only: it does NOT reproduce Promise/await scheduling semantics (a later
 * stage). `async` / `await` used as ordinary identifiers are left untouched.
 * Streaming, lexer-aware; output is never longer than input (only removes
 * tokens). Returns bytes written, 0 on overflow. */
static size_t
es6_async_pass(const char *src, size_t len, char *out, size_t cap)
{
	size_t i;
	size_t o;
	int    st;
	int    prev_sig;

	if (src == NULL || out == NULL || cap == 0) return 0;
	i = 0; o = 0; st = ES_CODE; prev_sig = 0;

	while (i < len) {
		char c = src[i];
		if (o + 1 >= cap) return 0;

		if (st == ES_CODE) {
			if (c == '/' && i + 1 < len && src[i + 1] == '/') { out[o++] = c; if (o + 1 >= cap) return 0; out[o++] = src[i + 1]; i += 2; st = ES_LCOM; continue; }
			if (c == '/' && i + 1 < len && src[i + 1] == '*') { out[o++] = c; if (o + 1 >= cap) return 0; out[o++] = src[i + 1]; i += 2; st = ES_BCOM; continue; }
			if (c == '/' && es6_regex_ctx(prev_sig)) { out[o++] = c; i++; st = ES_RE; continue; }
			if (c == '\'') { out[o++] = c; prev_sig = c; i++; st = ES_SQ; continue; }
			if (c == '"')  { out[o++] = c; prev_sig = c; i++; st = ES_DQ; continue; }
			if (c == '`')  { out[o++] = c; prev_sig = c; i++; st = ES_TMPL; continue; }

			if (es6_is_ident((unsigned char) c) &&
			    (i == 0 || !es6_is_ident((unsigned char) src[i - 1]))) {
				size_t ids = i;
				size_t ide;
				while (i < len && es6_is_ident((unsigned char) src[i])) i++;
				ide = i;

				/* async before function / arrow -> strip */
				if (ide - ids == 5 && strncmp(src + ids, "async", 5) == 0) {
					size_t j = ide;
					int strip = 0;
					while (j < len && (src[j] == ' ' || src[j] == '\t' ||
					       src[j] == '\n' || src[j] == '\r')) j++;
					if (j + 8 <= len && strncmp(src + j, "function", 8) == 0 &&
					    (j + 8 == len || !es6_is_ident((unsigned char) src[j + 8]))) {
						strip = 1;
					} else if (j < len && src[j] == '(') {
						size_t k = es6_skip_parens(src, len, j);
						if (k > 0) {
							while (k < len && (src[k] == ' ' || src[k] == '\t' ||
							       src[k] == '\n' || src[k] == '\r')) k++;
							if (k + 1 < len && src[k] == '=' && src[k + 1] == '>') strip = 1;
						}
					} else if (j < len && es6_is_ident((unsigned char) src[j])) {
						size_t k = j;
						while (k < len && es6_is_ident((unsigned char) src[k])) k++;
						while (k < len && (src[k] == ' ' || src[k] == '\t' ||
						       src[k] == '\n' || src[k] == '\r')) k++;
						if (k + 1 < len && src[k] == '=' && src[k + 1] == '>') strip = 1;
					}
					if (strip) {
						i = j;          /* drop "async" + following ws */
						prev_sig = 0;   /* still expression position */
						continue;
					}
				}

				/* await unary keyword (followed by an expression start) -> strip */
				if (ide - ids == 5 && strncmp(src + ids, "await", 5) == 0) {
					size_t j = ide;
					int nextok = 0;
					while (j < len && (src[j] == ' ' || src[j] == '\t' ||
					       src[j] == '\n' || src[j] == '\r')) j++;
					if (j < len) {
						char d = src[j];
						if (es6_is_ident((unsigned char) d) || d == '(' ||
						    d == '[' || d == '{' || d == '"' || d == '\'' ||
						    d == '`' || (d >= '0' && d <= '9') ||
						    d == '!' || d == '~')
							nextok = 1;
					}
					if (nextok) {
						i = j;          /* drop "await" + following ws */
						prev_sig = 0;
						continue;
					}
				}

				/* ordinary identifier: emit verbatim */
				{
					size_t q;
					for (q = ids; q < ide; q++) {
						if (o + 1 >= cap) return 0;
						out[o++] = src[q];
					}
				}
				prev_sig = src[ide - 1];
				continue;
			}

			out[o++] = c;
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev_sig = c;
			i++;
			continue;
		}

		if (st == ES_SQ) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
			else if (c == '\'') st = ES_CODE;
			i++; continue;
		}
		if (st == ES_DQ) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
			else if (c == '"') st = ES_CODE;
			i++; continue;
		}
		if (st == ES_TMPL) {
			out[o++] = c;
			if (c == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
			else if (c == '`') st = ES_CODE;
			i++; continue;
		}
		if (st == ES_LCOM) { out[o++] = c; if (c == '\n') st = ES_CODE; i++; continue; }
		if (st == ES_BCOM) {
			out[o++] = c;
			if (c == '*' && i + 1 < len && src[i + 1] == '/') { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; st = ES_CODE; }
			i++; continue;
		}
		/* ES_RE */
		out[o++] = c;
		if (c == '\\' && i + 1 < len) { i++; if (o + 1 >= cap) return 0; out[o++] = src[i]; }
		else if (c == '/') { st = ES_CODE; prev_sig = '/'; }
		i++;
	}

	out[o] = '\0';
	return o;
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

	iter = 0;
	for (;;) {
		size_t p0, p1, bs, be, blen;
		int pident, blk;
		if (iter++ > 200000UL) break;
		if (!es6_find_arrow(a, alen, &p0, &p1, &pident, &bs, &be, &blk))
			break;
		blen = es6_emit_rewrite(a, alen, p0, p1, pident, bs, be, blk, b, wmax);
		if (blen == 0) break;   /* overflow — keep `a` as the result */
		tmp = a; a = b; b = tmp;
		alen = blen;
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

	/* Pass 1: let/const -> var (never grows). */
	n = es6_letconst_pass(src, len, buf1, wmax);
	if (n == 0) {
		/* unexpected overflow — pass the input through untouched */
		free(buf1); free(buf2);
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}
	cur = buf1;

	/* Pass 2: async/await strip (parse-level). Runs BEFORE arrows so an
	 * `async (x) => ...` becomes a plain arrow the arrow pass then lowers. */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_async_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* Pass 3: arrow functions. Pass 4: template literals. Pass 5: arrows
	 * again (a template interpolation can expose an arrow that the first
	 * arrow pass, running before the templates were lowered, never saw).
	 * Each pass returns 0 only on overflow, in which case we keep the prior
	 * buffer. */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_arrow_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_template_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	/* A template interpolation is emitted verbatim, so async/await/arrows
	 * that lived inside `${ ... }` only become plain code AFTER the template
	 * pass. Re-run async then arrows to lower those. */
	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_async_pass(cur, n, dst, wmax);
	if (m != 0) { cur = dst; n = m; }

	dst = (cur == buf1) ? buf2 : buf1;
	m = es6_arrow_pass(cur, n, dst, wmax);
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
