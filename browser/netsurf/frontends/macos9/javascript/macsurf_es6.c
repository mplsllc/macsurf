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
	char *scratch;
	size_t n1;
	size_t n2;

	if (src == NULL || out == NULL || cap == 0)
		return 0;

	/* scratch needs slack: es6_letconst_pass guards with `o + 2 >= cap`
	 * before each write, so a buffer of exactly len+1 false-overflows on
	 * the final byte whenever the output length equals the input (a script
	 * with no let/const to shrink it). len+16 gives ample headroom; the
	 * let/const pass never grows the text. */
	scratch = (char *)malloc(len + 16);
	if (scratch == NULL) {
		/* no transform possible — pass through if it fits */
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}

	n1 = es6_letconst_pass(src, len, scratch, len + 16);
	if (n1 == 0) {
		/* let/const pass overflowed (shouldn't with len+1) — pass through */
		free(scratch);
		if (len < cap) { memcpy(out, src, len); out[len] = '\0'; return len; }
		return 0;
	}

	n2 = es6_arrow_pass(scratch, n1, out, cap);
	if (n2 == 0) {
		/* arrow pass overflowed — fall back to the let/const-only result */
		if (n1 < cap) {
			memcpy(out, scratch, n1);
			out[n1] = '\0';
			n2 = n1;
		}
	}
	free(scratch);
	return n2;
}
