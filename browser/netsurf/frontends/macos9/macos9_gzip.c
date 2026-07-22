/*
 * MacSurf - macos9_gzip.c
 *
 * Streaming gzip / DEFLATE decoder. See macos9_gzip.h for why this exists
 * instead of a call to lodepng_inflate().
 *
 * Structure: a resumable state machine over a bit accumulator. Every state
 * that needs bits asks for them with gz_need(); if the current input chunk
 * cannot supply them the state machine returns, keeping the partially-filled
 * accumulator, and resumes at exactly the same point on the next push. There
 * is no rollback and no carry buffer, which is the whole reason the atomic
 * steps below are sized so none of them ever needs more than 16 bits at once:
 *
 *      literal/length symbol   <= 15 bits   (G_LEN)
 *      length extra bits       <=  5 bits   (G_LENEXT)
 *      distance symbol         <= 15 bits   (G_DIST)
 *      distance extra bits     <= 13 bits   (G_DISTEXT)
 *      code-length symbol+rep  <= 14 bits   (G_DYN_LENS)
 *      stored LEN / NLEN          16 bits   (G_STORED_*)
 *
 * A naive implementation decodes a whole length/distance PAIR as one unit,
 * which needs up to 48 bits and therefore either a 64-bit accumulator (CW8
 * PPC miscompiles 64-bit multiply, and we would rather not find out what else)
 * or a rollback buffer. Splitting the pair across four states removes both.
 *
 * The Huffman decoder is the canonical count[]/symbol[] walk from Mark
 * Adler's puff.c, with one change: it peeks bits out of the accumulator and
 * only consumes them once the symbol is known, so a caller that has already
 * guaranteed 15 bits can never leave the reader half-consumed.
 *
 * C89 (CW8): no //, no declarations after statements, no long long,
 * no designated initialisers, no for-scope declarations.
 */

#include <stdlib.h>
#include <string.h>

#include "macos9_gzip.h"

#define GZ_WBITS  15
#define GZ_WSIZE  (1L << GZ_WBITS)      /* 32768 */
#define GZ_WMASK  (GZ_WSIZE - 1)

/* State machine. */
#define G_HEADER        0
#define G_BLOCK         1
#define G_STORED_ALIGN  2
#define G_STORED_LEN    3
#define G_STORED_NLEN   4
#define G_STORED_COPY   5
#define G_DYN_HDR       6
#define G_DYN_CLEN      7
#define G_DYN_LENS      8
#define G_LEN           9
#define G_LENEXT       10
#define G_DIST         11
#define G_DISTEXT      12
#define G_COPY         13
#define G_BLOCK_END    14
#define G_TRAILER_ALIGN 15
#define G_TRAILER      16
#define G_DONE         17
#define G_ERR          18

struct macos9_gunzip {
	macos9_gunzip_emit_fn emit;
	void          *cbctx;

	int            state;
	const char    *msg;

	/* Bit reader over the CURRENT push buffer only. in/in_len/in_pos are
	 * reset every push; bitbuf/bitcnt persist across pushes and are what
	 * makes the machine resumable without a carry buffer. */
	const unsigned char *in;
	long           in_len;
	long           in_pos;
	unsigned long  bitbuf;
	int            bitcnt;

	/* gzip member header */
	int            hdr_step;
	int            hdr_flg;
	int            hdr_hcrc;
	long           hdr_skip;

	/* deflate block */
	int            final_block;
	int            btype;
	long           stored_left;

	int            hlit;
	int            hdist;
	int            hclen;
	int            n_read;

	short          clenlen[19];
	short          clencnt[16];
	short          clensym[19];

	short          lengths[320];
	short          lencnt[16];
	short          lensym[288];
	short          distcnt[16];
	short          distsym[32];

	/* Active tables for this block: either the shared fixed tables or the
	 * per-block dynamic ones above. */
	const short   *lc;
	const short   *ls;
	const short   *dc;
	const short   *ds;

	int            sym;
	unsigned long  copy_len;
	unsigned long  copy_dist;

	/* trailer */
	int            tr_step;
	unsigned long  tr_crc;
	unsigned long  tr_isize;

	/* sliding window doubles as the output staging buffer. wflush is the
	 * first byte not yet handed to emit(). */
	unsigned long  wpos;
	unsigned long  wflush;

	unsigned long  crc;
	unsigned long  total_in;
	unsigned long  total_out;

	unsigned char  win[GZ_WSIZE];
};

/* ------------------------------------------------------------------ CRC32 */

/* Implemented here rather than calling lodepng_crc32() on purpose: this file
 * then has no link dependency at all, which is what lets the Linux test build
 * exercise byte-for-byte the same source that ships to the Mac. A helper that
 * can only be tested in a different configuration from the one that ships is
 * not really tested. */
static unsigned long gz_crc_table[256];
static int gz_crc_ready = 0;

static void gz_crc_init(void)
{
	unsigned long c;
	int n;
	int k;

	for (n = 0; n < 256; n++) {
		c = (unsigned long)n;
		for (k = 0; k < 8; k++) {
			if ((c & 1UL) != 0UL) {
				c = 0xEDB88320UL ^ (c >> 1);
			} else {
				c = c >> 1;
			}
		}
		gz_crc_table[n] = c;
	}
	gz_crc_ready = 1;
}

/* ------------------------------------------------------- DEFLATE constants */

static const short gz_lbase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short gz_lext[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const short gz_dbase[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577
};
static const short gz_dext[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const short gz_clorder[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* --------------------------------------------------------- Huffman tables */

/* puff.c's construct(): build count[] and symbol[] from a code-length list.
 * Returns 0 for a complete code, >0 for an incomplete one (legal for a
 * distance code with a single symbol), <0 for an over-subscribed one. */
static int gz_construct(short *count, short *symbol, const short *length, int n)
{
	int  i;
	int  len;
	int  left;
	short offs[16];

	for (len = 0; len <= 15; len++) {
		count[len] = 0;
	}
	for (i = 0; i < n; i++) {
		count[length[i]]++;
	}
	if ((int)count[0] == n) {
		return 0;
	}

	left = 1;
	for (len = 1; len <= 15; len++) {
		left <<= 1;
		left -= (int)count[len];
		if (left < 0) {
			return left;
		}
	}

	offs[1] = 0;
	for (len = 1; len < 15; len++) {
		offs[len + 1] = (short)(offs[len] + count[len]);
	}
	for (i = 0; i < n; i++) {
		if (length[i] != 0) {
			symbol[offs[length[i]]++] = (short)i;
		}
	}
	return left;
}

static short gz_fix_lencnt[16];
static short gz_fix_lensym[288];
static short gz_fix_distcnt[16];
static short gz_fix_distsym[30];
static int   gz_fixed_ready = 0;

static void gz_build_fixed(void)
{
	short lengths[288];
	int i;

	for (i = 0; i < 144; i++) lengths[i] = 8;
	for (; i < 256; i++)      lengths[i] = 9;
	for (; i < 280; i++)      lengths[i] = 7;
	for (; i < 288; i++)      lengths[i] = 8;
	gz_construct(gz_fix_lencnt, gz_fix_lensym, lengths, 288);

	for (i = 0; i < 30; i++)  lengths[i] = 5;
	gz_construct(gz_fix_distcnt, gz_fix_distsym, lengths, 30);

	gz_fixed_ready = 1;
}

/* ------------------------------------------------------------- bit reader */

/* Never called with n > 16, which keeps bitcnt <= 23 and therefore keeps the
 * shift below inside a 32-bit unsigned long. */
static int gz_need(struct macos9_gunzip *z, int n)
{
	while (z->bitcnt < n) {
		if (z->in_pos >= z->in_len) {
			return 0;
		}
		z->bitbuf |= ((unsigned long)z->in[z->in_pos]) << z->bitcnt;
		z->in_pos++;
		z->bitcnt += 8;
	}
	return 1;
}

static unsigned long gz_take(struct macos9_gunzip *z, int n)
{
	unsigned long v;

	if (n <= 0) {
		return 0UL;
	}
	v = z->bitbuf & ((1UL << n) - 1UL);
	z->bitbuf >>= n;
	z->bitcnt -= n;
	return v;
}

static void gz_align(struct macos9_gunzip *z)
{
	int drop;

	drop = z->bitcnt & 7;
	if (drop != 0) {
		z->bitbuf >>= drop;
		z->bitcnt -= drop;
	}
}

/* Caller must have guaranteed 15 bits. Peeks, then consumes exactly the
 * length of the matched code. */
static int gz_decode(struct macos9_gunzip *z, const short *count,
		     const short *symbol)
{
	int len;
	int code;
	int first;
	int index;
	int cnt;
	int used;

	code = 0;
	first = 0;
	index = 0;
	used = 0;
	for (len = 1; len <= 15; len++) {
		code |= (int)((z->bitbuf >> used) & 1UL);
		used++;
		cnt = (int)count[len];
		if (code - cnt < first) {
			z->bitbuf >>= used;
			z->bitcnt -= used;
			return (int)symbol[index + (code - first)];
		}
		index += cnt;
		first += cnt;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* ------------------------------------------------------------ output path */

static void gz_flush(struct macos9_gunzip *z)
{
	if (z->wpos > z->wflush) {
		z->emit(z->cbctx, (const char *)(z->win + z->wflush),
			(long)(z->wpos - z->wflush));
	}
	z->wflush = z->wpos;
}

static int gz_fail(struct macos9_gunzip *z, const char *why)
{
	z->state = G_ERR;
	z->msg = why;
	return MACOS9_GUNZIP_ERROR;
}

/* Returns 1 on success, 0 if a guard tripped (state already set to G_ERR). */
static int gz_out(struct macos9_gunzip *z, unsigned char b)
{
	z->win[z->wpos] = b;
	z->wpos++;
	z->crc = gz_crc_table[(z->crc ^ (unsigned long)b) & 0xFFUL]
		 ^ (z->crc >> 8);
	z->total_out++;

	if (z->total_out > MACOS9_GUNZIP_MAX_OUT) {
		gz_fail(z, "gzip output cap");
		return 0;
	}
	if (z->wpos == (unsigned long)GZ_WSIZE) {
		gz_flush(z);
		z->wpos = 0;
		z->wflush = 0;
	}
	return 1;
}

/* ---------------------------------------------------------- state machine */

static int gz_run(struct macos9_gunzip *z)
{
	for (;;) {
		switch (z->state) {

		case G_HEADER:
			while (z->hdr_step < 16) {
				unsigned int b;

				/* Steps that consume no byte because their
				 * FLG bit is clear. */
				if (z->hdr_step == 10 &&
				    (z->hdr_flg & 0x04) == 0) {
					z->hdr_step = 13;
					continue;
				}
				if (z->hdr_step == 12 && z->hdr_skip <= 0) {
					z->hdr_step = 13;
					continue;
				}
				if (z->hdr_step == 13 &&
				    (z->hdr_flg & 0x08) == 0) {
					z->hdr_step = 14;
					continue;
				}
				if (z->hdr_step == 14 &&
				    (z->hdr_flg & 0x10) == 0) {
					z->hdr_step = 15;
					continue;
				}
				if (z->hdr_step == 15 &&
				    (z->hdr_flg & 0x02) == 0) {
					z->hdr_step = 16;
					continue;
				}

				if (!gz_need(z, 8)) {
					return MACOS9_GUNZIP_OK;
				}
				b = (unsigned int)gz_take(z, 8);

				switch (z->hdr_step) {
				case 0:
					if (b != 0x1FU) {
						return gz_fail(z,
							"gzip bad magic");
					}
					z->hdr_step++;
					break;
				case 1:
					if (b != 0x8BU) {
						return gz_fail(z,
							"gzip bad magic");
					}
					z->hdr_step++;
					break;
				case 2:
					if (b != 8U) {
						return gz_fail(z,
							"gzip bad method");
					}
					z->hdr_step++;
					break;
				case 3:
					z->hdr_flg = (int)b;
					if ((z->hdr_flg & 0xE0) != 0) {
						return gz_fail(z,
							"gzip reserved flag");
					}
					z->hdr_step++;
					break;
				case 4: case 5: case 6: case 7:	/* MTIME */
				case 8:				/* XFL   */
				case 9:				/* OS    */
					z->hdr_step++;
					break;
				case 10:
					z->hdr_skip = (long)b;
					z->hdr_step++;
					break;
				case 11:
					z->hdr_skip |= ((long)b) << 8;
					z->hdr_step++;
					break;
				case 12:			/* FEXTRA */
					z->hdr_skip--;
					break;
				case 13:			/* FNAME */
					if (b == 0U) z->hdr_step++;
					break;
				case 14:			/* FCOMMENT */
					if (b == 0U) z->hdr_step++;
					break;
				case 15:			/* FHCRC */
					z->hdr_hcrc++;
					if (z->hdr_hcrc >= 2) z->hdr_step++;
					break;
				default:
					z->hdr_step++;
					break;
				}
			}
			z->state = G_BLOCK;
			break;

		case G_BLOCK:
			if (!gz_need(z, 3)) {
				return MACOS9_GUNZIP_OK;
			}
			z->final_block = (int)gz_take(z, 1);
			z->btype = (int)gz_take(z, 2);
			if (z->btype == 0) {
				z->state = G_STORED_ALIGN;
			} else if (z->btype == 1) {
				if (!gz_fixed_ready) {
					gz_build_fixed();
				}
				z->lc = gz_fix_lencnt;
				z->ls = gz_fix_lensym;
				z->dc = gz_fix_distcnt;
				z->ds = gz_fix_distsym;
				z->state = G_LEN;
			} else if (z->btype == 2) {
				z->state = G_DYN_HDR;
			} else {
				return gz_fail(z, "gzip bad block type");
			}
			break;

		case G_STORED_ALIGN:
			gz_align(z);
			z->state = G_STORED_LEN;
			break;

		case G_STORED_LEN:
			if (!gz_need(z, 16)) {
				return MACOS9_GUNZIP_OK;
			}
			z->stored_left = (long)gz_take(z, 16);
			z->state = G_STORED_NLEN;
			break;

		case G_STORED_NLEN:
			if (!gz_need(z, 16)) {
				return MACOS9_GUNZIP_OK;
			}
			{
				unsigned long nl;

				nl = gz_take(z, 16);
				if (((~nl) & 0xFFFFUL) !=
				    (unsigned long)z->stored_left) {
					return gz_fail(z,
						"gzip stored len mismatch");
				}
			}
			z->state = G_STORED_COPY;
			break;

		case G_STORED_COPY:
			while (z->stored_left > 0) {
				if (!gz_need(z, 8)) {
					return MACOS9_GUNZIP_OK;
				}
				if (!gz_out(z,
					(unsigned char)gz_take(z, 8))) {
					return MACOS9_GUNZIP_ERROR;
				}
				z->stored_left--;
			}
			z->state = G_BLOCK_END;
			break;

		case G_DYN_HDR:
			if (!gz_need(z, 14)) {
				return MACOS9_GUNZIP_OK;
			}
			z->hlit  = (int)gz_take(z, 5) + 257;
			z->hdist = (int)gz_take(z, 5) + 1;
			z->hclen = (int)gz_take(z, 4) + 4;
			if (z->hlit > 286 || z->hdist > 30) {
				return gz_fail(z, "gzip bad dynamic counts");
			}
			{
				int i;

				for (i = 0; i < 19; i++) {
					z->clenlen[i] = 0;
				}
			}
			z->n_read = 0;
			z->state = G_DYN_CLEN;
			break;

		case G_DYN_CLEN:
			while (z->n_read < z->hclen) {
				if (!gz_need(z, 3)) {
					return MACOS9_GUNZIP_OK;
				}
				z->clenlen[gz_clorder[z->n_read]] =
					(short)gz_take(z, 3);
				z->n_read++;
			}
			if (gz_construct(z->clencnt, z->clensym,
					 z->clenlen, 19) != 0) {
				return gz_fail(z, "gzip bad code-length code");
			}
			z->n_read = 0;
			z->state = G_DYN_LENS;
			break;

		case G_DYN_LENS:
			while (z->n_read < z->hlit + z->hdist) {
				int s;

				/* 7-bit code + up to 7 extra bits = 14. */
				if (!gz_need(z, 15)) {
					return MACOS9_GUNZIP_OK;
				}
				s = gz_decode(z, z->clencnt, z->clensym);
				if (s < 0) {
					return gz_fail(z,
						"gzip bad code-length symbol");
				}
				if (s < 16) {
					z->lengths[z->n_read] = (short)s;
					z->n_read++;
				} else {
					int fill;
					int rep;

					fill = 0;
					if (s == 16) {
						if (z->n_read == 0) {
							return gz_fail(z,
							  "gzip repeat at start");
						}
						fill = (int)z->lengths[
							z->n_read - 1];
						rep = 3 + (int)gz_take(z, 2);
					} else if (s == 17) {
						rep = 3 + (int)gz_take(z, 3);
					} else {
						rep = 11 + (int)gz_take(z, 7);
					}
					if (z->n_read + rep >
					    z->hlit + z->hdist) {
						return gz_fail(z,
						  "gzip repeat overflow");
					}
					while (rep > 0) {
						z->lengths[z->n_read] =
							(short)fill;
						z->n_read++;
						rep--;
					}
				}
			}
			if (z->lengths[256] == 0) {
				return gz_fail(z, "gzip no end-of-block code");
			}
			{
				int e;

				e = gz_construct(z->lencnt, z->lensym,
						 z->lengths, z->hlit);
				if (e != 0 && (e < 0 || z->hlit !=
				    (int)z->lencnt[0] + (int)z->lencnt[1])) {
					return gz_fail(z,
						"gzip bad literal code");
				}
				e = gz_construct(z->distcnt, z->distsym,
						 z->lengths + z->hlit,
						 z->hdist);
				if (e != 0 && (e < 0 || z->hdist !=
				    (int)z->distcnt[0] + (int)z->distcnt[1])) {
					return gz_fail(z,
						"gzip bad distance code");
				}
			}
			z->lc = z->lencnt;
			z->ls = z->lensym;
			z->dc = z->distcnt;
			z->ds = z->distsym;
			z->state = G_LEN;
			break;

		case G_LEN:
			if (!gz_need(z, 15)) {
				return MACOS9_GUNZIP_OK;
			}
			z->sym = gz_decode(z, z->lc, z->ls);
			if (z->sym < 0) {
				return gz_fail(z, "gzip bad literal code");
			}
			if (z->sym < 256) {
				if (!gz_out(z, (unsigned char)z->sym)) {
					return MACOS9_GUNZIP_ERROR;
				}
				break;
			}
			if (z->sym == 256) {
				z->state = G_BLOCK_END;
				break;
			}
			z->sym -= 257;
			if (z->sym >= 29) {
				return gz_fail(z, "gzip bad length code");
			}
			z->state = G_LENEXT;
			break;

		case G_LENEXT:
			{
				int e;

				e = (int)gz_lext[z->sym];
				if (!gz_need(z, e)) {
					return MACOS9_GUNZIP_OK;
				}
				z->copy_len = (unsigned long)gz_lbase[z->sym]
					      + gz_take(z, e);
			}
			z->state = G_DIST;
			break;

		case G_DIST:
			if (!gz_need(z, 15)) {
				return MACOS9_GUNZIP_OK;
			}
			z->sym = gz_decode(z, z->dc, z->ds);
			if (z->sym < 0 || z->sym >= 30) {
				return gz_fail(z, "gzip bad distance code");
			}
			z->state = G_DISTEXT;
			break;

		case G_DISTEXT:
			{
				int e;

				e = (int)gz_dext[z->sym];
				if (!gz_need(z, e)) {
					return MACOS9_GUNZIP_OK;
				}
				z->copy_dist = (unsigned long)gz_dbase[z->sym]
					       + gz_take(z, e);
			}
			if (z->copy_dist > z->total_out) {
				return gz_fail(z, "gzip distance before start");
			}
			z->state = G_COPY;
			break;

		case G_COPY:
			/* Needs no input, so it always runs to completion.
			 * Reading win[src] before gz_out() writes win[wpos] is
			 * what makes distance == 32768 (src == wpos) correct:
			 * the byte read is the one about to be overwritten,
			 * i.e. exactly 32768 bytes back. */
			while (z->copy_len > 0) {
				unsigned long src;

				src = (z->wpos - z->copy_dist) &
					(unsigned long)GZ_WMASK;
				if (!gz_out(z, z->win[src])) {
					return MACOS9_GUNZIP_ERROR;
				}
				z->copy_len--;
			}
			z->state = G_LEN;
			break;

		case G_BLOCK_END:
			z->state = z->final_block ? G_TRAILER_ALIGN : G_BLOCK;
			break;

		case G_TRAILER_ALIGN:
			gz_align(z);
			z->tr_step = 0;
			z->tr_crc = 0UL;
			z->tr_isize = 0UL;
			z->state = G_TRAILER;
			break;

		case G_TRAILER:
			while (z->tr_step < 8) {
				unsigned long b;

				if (!gz_need(z, 8)) {
					return MACOS9_GUNZIP_OK;
				}
				b = gz_take(z, 8);
				if (z->tr_step < 4) {
					z->tr_crc |= b << (8 * z->tr_step);
				} else {
					z->tr_isize |=
						b << (8 * (z->tr_step - 4));
				}
				z->tr_step++;
			}
			gz_flush(z);
			if (((z->crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL) !=
			    z->tr_crc) {
				return gz_fail(z, "gzip CRC mismatch");
			}
			if ((z->total_out & 0xFFFFFFFFUL) != z->tr_isize) {
				return gz_fail(z, "gzip length mismatch");
			}
			z->state = G_DONE;
			return MACOS9_GUNZIP_DONE;

		case G_DONE:
			return MACOS9_GUNZIP_DONE;

		default:
			return MACOS9_GUNZIP_ERROR;
		}
	}
}

/* ---------------------------------------------------------- public API */

struct macos9_gunzip *macos9_gunzip_create(macos9_gunzip_emit_fn emit,
					   void *cbctx)
{
	struct macos9_gunzip *z;

	if (emit == NULL) {
		return NULL;
	}
	if (!gz_crc_ready) {
		gz_crc_init();
	}
	z = (struct macos9_gunzip *)malloc(sizeof(struct macos9_gunzip));
	if (z == NULL) {
		return NULL;
	}
	memset(z, 0, sizeof(struct macos9_gunzip));
	z->emit = emit;
	z->cbctx = cbctx;
	z->state = G_HEADER;
	z->msg = "ok";
	z->crc = 0xFFFFFFFFUL;
	return z;
}

void macos9_gunzip_destroy(struct macos9_gunzip *z)
{
	if (z != NULL) {
		free(z);
	}
}

int macos9_gunzip_push(struct macos9_gunzip *z, const char *data, long len)
{
	int r;

	if (z == NULL) {
		return MACOS9_GUNZIP_ERROR;
	}
	if (z->state == G_ERR) {
		return MACOS9_GUNZIP_ERROR;
	}
	if (z->state == G_DONE) {
		return MACOS9_GUNZIP_DONE;
	}
	if (data == NULL || len <= 0) {
		return MACOS9_GUNZIP_OK;
	}

	z->in = (const unsigned char *)data;
	z->in_len = len;
	z->in_pos = 0;
	z->total_in += (unsigned long)len;

	r = gz_run(z);

	z->in = NULL;
	z->in_len = 0;
	z->in_pos = 0;

	if (r == MACOS9_GUNZIP_OK) {
		/* Hand on whatever this chunk produced rather than sitting on
		 * it until the window wraps -- that is the whole point of
		 * streaming. */
		gz_flush(z);
		/* Ratio guard, checked per push rather than per byte so the
		 * inner loop stays a compare and not a divide. The absolute
		 * cap in gz_out() is what bounds a single push. */
		if (z->total_out > MACOS9_GUNZIP_RATIO_FLOOR &&
		    z->total_in > 0UL &&
		    (z->total_out / z->total_in) > MACOS9_GUNZIP_MAX_RATIO) {
			return gz_fail(z, "gzip expansion ratio cap");
		}
	}
	return r;
}

int macos9_gunzip_finish(struct macos9_gunzip *z)
{
	if (z == NULL) {
		return MACOS9_GUNZIP_ERROR;
	}
	if (z->state == G_DONE) {
		return MACOS9_GUNZIP_DONE;
	}
	if (z->state == G_ERR) {
		return MACOS9_GUNZIP_ERROR;
	}
	gz_flush(z);
	return gz_fail(z, "gzip stream truncated");
}

const char *macos9_gunzip_status(struct macos9_gunzip *z)
{
	if (z == NULL || z->msg == NULL) {
		return "gzip (null)";
	}
	return z->msg;
}

long macos9_gunzip_total_in(struct macos9_gunzip *z)
{
	return (z == NULL) ? 0L : (long)z->total_in;
}

long macos9_gunzip_total_out(struct macos9_gunzip *z)
{
	return (z == NULL) ? 0L : (long)z->total_out;
}
