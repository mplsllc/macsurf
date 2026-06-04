/*
 * MacSurf — macsurf_es6.c
 *
 * Minimal ES6 -> ES5 source transpiler for the Duktape engine (ES5.1).
 * Raises the JS ceiling so modern scripts parse. Applied transform-on-failure
 * in js_exec: a script that Duktape rejects with a SyntaxError is re-tried
 * after this transform, so working ES5 scripts are never touched.
 *
 * STAGE 1 (this file): `let` / `const` -> `var`, lexer-aware — only in CODE
 * regions, never inside strings / comments / regex / template literals. This
 * is the foundation; arrow functions, template literals, optional chaining,
 * classes and async/await are later stages.
 *
 * C89 / CW8. No external deps. Part of MacSurf, NetSurf engine. GPL v2.
 */

#include <string.h>
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

/* Identifier char — used to find keyword token boundaries. */
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

/* Transpile src[0..len) into out (cap bytes incl. terminating NUL). Returns
 * the number of bytes written (excluding NUL), or 0 on overflow. Stage 1:
 * let/const -> var in CODE regions. Output is never longer than the input
 * (let->var same length, const->var shorter), so cap = len + 1 always fits. */
size_t
macsurf_es6_transpile(const char *src, size_t len, char *out, size_t cap)
{
	size_t i;
	size_t o;
	int    st;
	int    prev_sig;   /* last significant (non-ws) char, for regex ctx */

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

			/* let / const at a fresh token boundary -> var */
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
			/* template treated opaque for Stage 1 (let/const inside a
			 * ${} interpolation is rare; arrow/template stage revisits). */
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
