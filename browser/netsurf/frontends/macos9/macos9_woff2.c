/*
 * MacSurf - macos9_woff2.c
 *
 * WOFF2 (RFC 7936 / W3C WOFF2) -> TTF reconstruction for @font-face glyph
 * rendering. Modern sites ship their icon fonts (FontAwesome, Material Design
 * Icons, Google Fonts) as WOFF2 bodies; the raw bytes are a Brotli-compressed
 * TRANSFORMED sfnt (glyf/loca/hmtx get a font-specific transform on top of
 * Brotli). This file reverses the whole pipeline:
 *
 *   1. Parse the WOFF2 header + table directory (UIntBase128 lengths, the
 *      63 known tags, transform flags).
 *   2. Brotli-decompress the one stream into the full transformed sfnt.
 *   3. Reconstruct a standard TTF: sfnt directory (sorted by tag, real
 *      checksums), raw tables copied through, the glyf/loca transform undone
 *      (255UInt16 point counts, triplet-encoded deltas, composite glyph
 *      reordering, short-loca repacking), and the hmtx transform undone
 *      (proportional/monospace lsb split using glyf xMin values).
 *   4. Recompute checkSumAdjustment into 'head'.
 *
 * The result is byte-identical to the reference google/woff2 decoder
 * (woff2_dec.cc, Apache 2.0 - this is a direct C89 port of its non-TTC path;
 * TrueType Collections are rejected, matching MacSurf's sfnt parser which
 * never handled 'ttcf' either). Verified by the harness (Test 65) against
 * libwoff2dec on real fonts (FontAwesome, Roboto, Inter variable).
 *
 * C89 throughout (CW8): no //, no declarations after statements, no for-scope
 * declarations, no long long, no std::anything. All allocations are malloc'd
 * and bounded: decompressed stream <= 32 MB, per-glyph point buffers sized by
 * the 16-bit endPtsOfContours limit.
 *
 * Part of MacSurf, built on the NetSurf engine. GPL v2 (this file); the
 * ported algorithm is Apache 2.0 (google/woff2).
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "macos9_brotli.h"
#include "macos9_woff2.h"

typedef uint8_t  w2_u8;
typedef uint16_t w2_u16;
typedef uint32_t w2_u32;
typedef int16_t  w2_s16;

/* ---- constants (google/woff2 woff2_common.h / table_tags.h) ------------ */

#define W2_SFNT_HEADER_SIZE  12
#define W2_SFNT_ENTRY_SIZE   16
#define W2_WOFF2_SIGNATURE   0x774f4632UL    /* "wOF2" */
#define W2_TTC_FONT_FLAVOR   0x74746366UL    /* "ttcf" */
#define W2_FLAGS_TRANSFORM   (1UL << 8)

#define W2_TAG_GLYF 0x676c7966UL
#define W2_TAG_HEAD 0x68656164UL
#define W2_TAG_HHEA 0x68686561UL
#define W2_TAG_HMTX 0x686d7478UL
#define W2_TAG_LOCA 0x6c6f6361UL

#define W2_CHECKSUM_ADJ_OFFSET  8
#define W2_ENDPTS_OFFSET        10      /* simple glyph: endPts starts here */

#define W2_DEFAULT_GLYPH_BUF    5120    /* 98% of Google Fonts glyphs < 5 KB */
#define W2_MAX_PLAUSIBLE_RATIO  100.0   /* uncompressed/file-length sanity  */
#define W2_MAX_UNCOMPRESSED     (32UL * 1024UL * 1024UL)  /* bomb guard    */
#define W2_MAX_TABLES           4096

/* simple glyph flags */
#define W2_GLYF_ON_CURVE    (1 << 0)
#define W2_GLYF_X_SHORT     (1 << 1)
#define W2_GLYF_Y_SHORT     (1 << 2)
#define W2_GLYF_REPEAT      (1 << 3)
#define W2_GLYF_X_SAME      (1 << 4)
#define W2_GLYF_Y_SAME      (1 << 5)

/* composite glyph flags (sfntly CompositeGlyph) */
#define W2_ARG_1_AND_2_ARE_WORDS      (1 << 0)
#define W2_WE_HAVE_A_SCALE            (1 << 3)
#define W2_MORE_COMPONENTS            (1 << 5)
#define W2_WE_HAVE_AN_X_AND_Y_SCALE   (1 << 6)
#define W2_WE_HAVE_A_TWO_BY_TWO       (1 << 7)
#define W2_WE_HAVE_INSTRUCTIONS       (1 << 8)

/* The 63 known table tags, indexable by the low 6 bits of a directory flag
 * byte (google/woff2 table_tags.cc). */
static const w2_u32 w2_known_tags[63] = {
	0x636d6170UL, 0x68656164UL, 0x68686561UL, 0x686d7478UL,
	0x6d617870UL, 0x6e616d65UL, 0x4f532f32UL, 0x706f7374UL,
	0x63767420UL, 0x6670676dUL, 0x676c7966UL, 0x6c6f6361UL,
	0x70726570UL, 0x43464620UL, 0x564f5247UL, 0x45424454UL,
	0x45424c43UL, 0x67617370UL, 0x68646d78UL, 0x6b65726eUL,
	0x4c545348UL, 0x50434c54UL, 0x56444d58UL, 0x76686561UL,
	0x766d7478UL, 0x42415345UL, 0x47444546UL, 0x47504f53UL,
	0x47535542UL, 0x45425343UL, 0x4a535446UL, 0x4d415448UL,
	0x43424454UL, 0x43424c43UL, 0x434f4c52UL, 0x4350414cUL,
	0x53564720UL, 0x73626978UL, 0x61636e74UL, 0x61766172UL,
	0x62646174UL, 0x626c6f63UL, 0x62736c6eUL, 0x63766172UL,
	0x66647363UL, 0x66656174UL, 0x666d7478UL, 0x66766172UL,
	0x67766172UL, 0x68737479UL, 0x6a757374UL, 0x6c636172UL,
	0x6d6f7274UL, 0x6d6f7278UL, 0x6f706264UL, 0x70726f70UL,
	0x7472616bUL, 0x5a617066UL, 0x53696c66UL, 0x476c6174UL,
	0x476c6f63UL, 0x46656174UL, 0x53696c6cUL
};

/* ---- bounds-checked reader (google/woff2 buffer.h) ---------------------- */

struct w2_buf {
	const w2_u8 *d;
	size_t       len;
	size_t       off;
};

static void w2_buf_init(struct w2_buf *b, const void *d, size_t len)
{
	b->d = (const w2_u8 *) d;
	b->len = len;
	b->off = 0;
}

static int w2_buf_read(struct w2_buf *b, void *dst, size_t n)
{
	if (n > b->len - b->off)
		return 0;
	if (dst != NULL)
		memcpy(dst, b->d + b->off, n);
	b->off += n;
	return 1;
}

static int w2_buf_read_u8(struct w2_buf *b, w2_u8 *v)
{
	if (b->off + 1 > b->len)
		return 0;
	*v = b->d[b->off];
	b->off++;
	return 1;
}

static int w2_buf_read_u16(struct w2_buf *b, w2_u16 *v)
{
	if (b->off + 2 > b->len)
		return 0;
	*v = (w2_u16) (((w2_u16) b->d[b->off] << 8) | b->d[b->off + 1]);
	b->off += 2;
	return 1;
}

static int w2_buf_read_s16(struct w2_buf *b, w2_s16 *v)
{
	w2_u16 u;
	if (!w2_buf_read_u16(b, &u))
		return 0;
	*v = (w2_s16) u;
	return 1;
}

static int w2_buf_read_u32(struct w2_buf *b, w2_u32 *v)
{
	if (b->off + 4 > b->len)
		return 0;
	*v = ((w2_u32) b->d[b->off] << 24) |
	     ((w2_u32) b->d[b->off + 1] << 16) |
	     ((w2_u32) b->d[b->off + 2] << 8) |
	     (w2_u32) b->d[b->off + 3];
	b->off += 4;
	return 1;
}

/* ---- growable output writer (google/woff2 woff2_out.h) ------------------ */

struct w2_out {
	w2_u8 *d;
	size_t len;     /* bytes written */
	size_t cap;     /* allocated */
};

static int w2_out_need(struct w2_out *o, size_t extra)
{
	size_t ncap;

	if (extra > (size_t) -1 - o->len)
		return 0;
	if (o->len + extra <= o->cap)
		return 1;
	ncap = (o->cap > 0) ? o->cap : 4096;
	while (ncap < o->len + extra)
		ncap *= 2;
	{
		w2_u8 *nd = (w2_u8 *) realloc(o->d, ncap);
		if (nd == NULL)
			return 0;
		o->d = nd;
		o->cap = ncap;
	}
	return 1;
}

static int w2_out_append(struct w2_out *o, const void *p, size_t n)
{
	if (n == 0)
		return 1;
	if (!w2_out_need(o, n))
		return 0;
	memcpy(o->d + o->len, p, n);
	o->len += n;
	return 1;
}

/* Write at an arbitrary offset (table directory entries, checkSumAdjustment).
 * The target must already lie within written bytes. */
static int w2_out_write_at(struct w2_out *o, const void *p, size_t off,
		size_t n)
{
	if (off + n > o->len)
		return 0;
	memcpy(o->d + off, p, n);
	return 1;
}

static int w2_out_pad4(struct w2_out *o)
{
	static const w2_u8 zeroes[3] = { 0, 0, 0 };
	size_t pad = ((o->len + 3) & ~(size_t) 3) - o->len;
	if (pad > 0 && !w2_out_append(o, zeroes, pad))
		return 0;
	return 1;
}

/* ---- big-endian stores (google/woff2 store_bytes.h) --------------------- */

static size_t w2_store16(w2_u8 *dst, size_t off, int x)
{
	dst[off] = (w2_u8) (x >> 8);
	dst[off + 1] = (w2_u8) x;
	return off + 2;
}

static size_t w2_store32(w2_u8 *dst, size_t off, w2_u32 x)
{
	dst[off] = (w2_u8) (x >> 24);
	dst[off + 1] = (w2_u8) (x >> 16);
	dst[off + 2] = (w2_u8) (x >> 8);
	dst[off + 3] = (w2_u8) x;
	return off + 4;
}

static void w2_store16_ptr(w2_u8 *dst, size_t *off, int x)
{
	*off = w2_store16(dst, *off, x);
}

static void w2_store32_ptr(w2_u8 *dst, size_t *off, w2_u32 x)
{
	*off = w2_store32(dst, *off, x);
}

/* ---- table directory + header (google/woff2 woff2_dec.cc) --------------- */

struct w2_table {
	w2_u32 tag;
	w2_u32 flags;           /* W2_FLAGS_TRANSFORM set = transformed data  */
	w2_u32 src_offset;      /* into the decompressed stream               */
	w2_u32 src_length;      /* bytes in stream (transformed length)       */
	w2_u32 transform_length;
	w2_u32 dst_offset;      /* into the reconstructed output              */
	w2_u32 dst_length;
};

struct w2_hdr {
	w2_u32        flavor;
	w2_u16        num_tables;
	size_t        compressed_offset;
	w2_u32        compressed_length;
	size_t        uncompressed_size;  /* size of the decompressed stream   */
	struct w2_table *tables;
};

struct w2_glyph_info {
	w2_u16  num_glyphs;
	w2_u16  index_format;
	w2_u16  num_hmetrics;
	w2_s16 *x_mins;         /* num_glyphs entries; set from glyf bboxes   */
};

/* UIntBase128 (WOFF2 spec 5.2). */
static int w2_read_base128(struct w2_buf *b, w2_u32 *value)
{
	w2_u32 result = 0;
	size_t i;

	for (i = 0; i < 5; i++) {
		w2_u8 code;
		if (!w2_buf_read_u8(b, &code))
			return 0;
		if (i == 0 && code == 0x80)
			return 0;       /* leading zero is invalid */
		if (result & 0xfe000000UL)
			return 0;       /* about to overflow */
		result = (result << 7) | (w2_u32) (code & 0x7f);
		if ((code & 0x80) == 0) {
			*value = result;
			return 1;
		}
	}
	return 0;               /* too many bytes */
}

/* 255UInt16 (WOFF2 spec 5.1). */
static int w2_read_255us16(struct w2_buf *b, unsigned int *value)
{
	w2_u8 code;

	if (!w2_buf_read_u8(b, &code))
		return 0;
	if (code == 253) {
		w2_u16 result;
		if (!w2_buf_read_u16(b, &result))
			return 0;
		*value = result;
		return 1;
	} else if (code == 255) {
		w2_u8 result;
		if (!w2_buf_read_u8(b, &result))
			return 0;
		*value = (unsigned int) result + 253;
		return 1;
	} else if (code == 254) {
		w2_u8 result;
		if (!w2_buf_read_u8(b, &result))
			return 0;
		*value = (unsigned int) result + 506;
		return 1;
	}
	*value = code;
	return 1;
}

/* Checksum over big-endian 32-bit words (google/woff2 woff2_common.cc). */
static w2_u32 w2_compute_ulong_sum(const w2_u8 *buf, size_t size)
{
	w2_u32 checksum = 0;
	size_t aligned_size = size & ~(size_t) 3;
	size_t i;

	for (i = 0; i < aligned_size; i += 4) {
		checksum += ((w2_u32) buf[i] << 24) |
			    ((w2_u32) buf[i + 1] << 16) |
			    ((w2_u32) buf[i + 2] << 8) |
			    (w2_u32) buf[i + 3];
	}
	/* The final partial u32 is treated as padded with zeros (never read
	 * past the end): matching ComputeULongSum in the reference decoder. */
	if (size != aligned_size) {
		w2_u32 v = 0;
		for (i = aligned_size; i < size; i++)
			v |= (w2_u32) buf[i] << (24 - 8 * (i & 3));
		checksum += v;
	}
	return checksum;
}

/* Table directory: flag byte + optional tag + UIntBase128 lengths. The
 * directory's src_offset chain lands the LAST table's end exactly at the end
 * of the transformed stream, which is how the uncompressed size is known. */
static int w2_read_table_directory(struct w2_buf *file,
		struct w2_table *tables, size_t num_tables)
{
	w2_u32 src_offset = 0;
	size_t i;

	for (i = 0; i < num_tables; i++) {
		struct w2_table *table = &tables[i];
		w2_u8 flag_byte;
		w2_u32 tag;
		w2_u32 flags;
		w2_u8 xform_version;
		w2_u32 dst_length;
		w2_u32 transform_length;

		if (!w2_buf_read_u8(file, &flag_byte))
			return 0;
		if ((flag_byte & 0x3f) == 0x3f) {
			if (!w2_buf_read_u32(file, &tag))
				return 0;
		} else {
			tag = w2_known_tags[flag_byte & 0x3f];
		}
		flags = 0;
		xform_version = (w2_u8) ((flag_byte >> 6) & 0x03);

		/* 0 means xform for glyf/loca, non-0 for others. */
		if (tag == W2_TAG_GLYF || tag == W2_TAG_LOCA) {
			if (xform_version == 0)
				flags |= W2_FLAGS_TRANSFORM;
		} else if (xform_version != 0) {
			flags |= W2_FLAGS_TRANSFORM;
		}

		if (!w2_read_base128(file, &dst_length))
			return 0;
		transform_length = dst_length;
		if ((flags & W2_FLAGS_TRANSFORM) != 0) {
			if (!w2_read_base128(file, &transform_length))
				return 0;
			if (tag == W2_TAG_LOCA && transform_length != 0)
				return 0;       /* transformed loca carries no data */
		}
		if (src_offset + transform_length < src_offset)
			return 0;
		table->src_offset = src_offset;
		table->src_length = transform_length;
		src_offset += transform_length;

		table->tag = tag;
		table->flags = flags;
		table->transform_length = transform_length;
		table->dst_length = dst_length;
	}
	return 1;
}

/* WOFF2 header layout validation (google/woff2 ReadWOFF2Header, non-TTC). */
static int w2_read_woff2_header(const w2_u8 *data, size_t length,
		struct w2_hdr *hdr)
{
	struct w2_buf file;
	w2_u32 signature, reported_length;
	w2_u32 meta_offset, meta_length, meta_orig_length;
	w2_u32 priv_offset, priv_length;
	size_t src_offset;
	struct w2_table *last;

	w2_buf_init(&file, data, length);

	if (!w2_buf_read_u32(&file, &signature) || signature != W2_WOFF2_SIGNATURE)
		return 0;
	if (!w2_buf_read_u32(&file, &hdr->flavor))
		return 0;
	if (!w2_buf_read_u32(&file, &reported_length) ||
			length != reported_length)
		return 0;
	if (!w2_buf_read_u16(&file, &hdr->num_tables) || hdr->num_tables == 0)
		return 0;
	if (hdr->num_tables > W2_MAX_TABLES)
		return 0;
	/* reserved(2) + totalSfntSize(4) - the reference decoder deliberately
	 * does NOT trust totalSfntSize; it derives the size from the directory. */
	if (!w2_buf_read(&file, NULL, 6))
		return 0;
	if (!w2_buf_read_u32(&file, &hdr->compressed_length))
		return 0;
	/* major/minor version (4 bytes) */
	if (!w2_buf_read(&file, NULL, 4))
		return 0;
	if (!w2_buf_read_u32(&file, &meta_offset) ||
			!w2_buf_read_u32(&file, &meta_length) ||
			!w2_buf_read_u32(&file, &meta_orig_length))
		return 0;
	if (meta_offset != 0 &&
			(meta_offset >= length || length - meta_offset < meta_length))
		return 0;
	if (!w2_buf_read_u32(&file, &priv_offset) ||
			!w2_buf_read_u32(&file, &priv_length))
		return 0;
	if (priv_offset != 0 &&
			(priv_offset >= length || length - priv_offset < priv_length))
		return 0;

	hdr->tables = (struct w2_table *)
			malloc((size_t) hdr->num_tables * sizeof(struct w2_table));
	if (hdr->tables == NULL)
		return 0;
	if (!w2_read_table_directory(&file, hdr->tables, hdr->num_tables)) {
		free(hdr->tables);
		hdr->tables = NULL;
		return 0;
	}

	/* The last table's src end IS the uncompressed (transformed) size. */
	last = &hdr->tables[hdr->num_tables - 1];
	hdr->uncompressed_size = (size_t) last->src_offset +
			(size_t) last->src_length;
	if (hdr->uncompressed_size < last->src_offset) {
		free(hdr->tables);
		hdr->tables = NULL;
		return 0;
	}

	/* TTC collections are out of scope for the MacSurf sfnt path - reject
	 * the same way the raw-sfnt parse would (the magic check bails on a
	 * directory it cannot interpret). */
	if (hdr->flavor == W2_TTC_FONT_FLAVOR) {
		free(hdr->tables);
		hdr->tables = NULL;
		return 0;
	}

	hdr->compressed_offset = file.off;

	/* Layout: compressed block, then optional meta block, then optional
	 * priv block; everything padded to 4; total file size must match. */
	src_offset = (hdr->compressed_offset + hdr->compressed_length + 3) & ~(size_t) 3;
	if (src_offset > length) {
		free(hdr->tables);
		hdr->tables = NULL;
		return 0;
	}
	if (meta_offset != 0) {
		if (src_offset != meta_offset) {
			free(hdr->tables);
			hdr->tables = NULL;
			return 0;
		}
		src_offset = ((size_t) meta_offset + meta_length + 3) & ~(size_t) 3;
	}
	if (priv_offset != 0) {
		if (src_offset != priv_offset) {
			free(hdr->tables);
			hdr->tables = NULL;
			return 0;
		}
		src_offset = ((size_t) priv_offset + priv_length + 3) & ~(size_t) 3;
	}
	if (src_offset != ((length + 3) & ~(size_t) 3)) {
		free(hdr->tables);
		hdr->tables = NULL;
		return 0;
	}
	return 1;
}

static void w2_hdr_destroy(struct w2_hdr *hdr)
{
	if (hdr->tables != NULL) {
		free(hdr->tables);
		hdr->tables = NULL;
	}
}

/* ---- sfnt header/directory emission (google/woff2 woff2_dec.cc) --------- */

static size_t w2_store_offset_table(w2_u8 *result, size_t offset,
		w2_u32 flavor, w2_u16 num_tables)
{
	unsigned int max_pow2 = 0;
	w2_u16 output_search_range;

	offset = w2_store32(result, offset, flavor);
	offset = w2_store16(result, offset, num_tables);
	while ((1u << (max_pow2 + 1)) <= (unsigned int) num_tables)
		max_pow2++;
	output_search_range = (w2_u16) ((1u << max_pow2) << 4);
	offset = w2_store16(result, offset, output_search_range);
	offset = w2_store16(result, offset, (w2_u16) max_pow2);
	offset = w2_store16(result, offset,
			(w2_u16) (((int) num_tables << 4) - output_search_range));
	return offset;
}

static size_t w2_store_table_entry(w2_u8 *result, size_t offset, w2_u32 tag)
{
	offset = w2_store32(result, offset, tag);
	offset = w2_store32(result, offset, 0);  /* checksum, patched later */
	offset = w2_store32(result, offset, 0);  /* offset, patched later   */
	offset = w2_store32(result, offset, 0);  /* length, patched later   */
	return offset;
}

static size_t w2_compute_offset_to_first_table(const struct w2_hdr *hdr)
{
	return W2_SFNT_HEADER_SIZE +
			(size_t) W2_SFNT_ENTRY_SIZE * hdr->num_tables;
}

/* table tag -> directory-entry offset, for the patch pass. */
struct w2_tag_off {
	w2_u32 tag;
	size_t off;
};

/* Find the entry offset for a tag (linear - num_tables is small). */
static size_t w2_entry_offset(struct w2_tag_off *entries, size_t n, w2_u32 tag)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (entries[i].tag == tag)
			return entries[i].off;
	return 0;
}

static struct w2_table *w2_find_table(struct w2_hdr *hdr, w2_u32 tag)
{
	size_t i;
	for (i = 0; i < hdr->num_tables; i++)
		if (hdr->tables[i].tag == tag)
			return &hdr->tables[i];
	return NULL;
}

/* Write the sfnt header + 16-byte-per-table directory (sorted by tag, all
 * checksum/offset/length zeroed), and record each tag's entry offset. */
static int w2_write_headers(const struct w2_hdr *hdr, struct w2_out *out,
		struct w2_tag_off *entries, w2_u32 *header_checksum)
{
	struct w2_table *sorted;
	w2_u8 *result;
	size_t offset;
	size_t n;
	size_t i, j;

	n = hdr->num_tables;
	sorted = (struct w2_table *) malloc(n * sizeof(struct w2_table));
	if (sorted == NULL)
		return 0;
	memcpy(sorted, hdr->tables, n * sizeof(struct w2_table));

	/* insertion sort by tag (the output directory must be in OTSpec order) */
	for (i = 1; i < n; i++) {
		struct w2_table key = sorted[i];
		j = i;
		while (j > 0 && sorted[j - 1].tag > key.tag) {
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = key;
	}

	if (!w2_out_need(out, W2_SFNT_HEADER_SIZE + W2_SFNT_ENTRY_SIZE * n)) {
		free(sorted);
		return 0;
	}
	result = out->d + out->len;   /* header is appended first, then patched */
	offset = w2_store_offset_table(result, 0, hdr->flavor, (w2_u16) n);
	for (i = 0; i < n; i++) {
		entries[i].tag = sorted[i].tag;
		entries[i].off = offset;
		offset = w2_store_table_entry(result, offset, sorted[i].tag);
	}
	out->len += offset;
	free(sorted);

	*header_checksum = w2_compute_ulong_sum(result, offset);
	return 1;
}

/* ---- glyf transform reconstruction (google/woff2 woff2_dec.cc) ---------- */

struct w2_point {
	int x;
	int y;
	int on_curve;
};

static int w2_with_sign(int flag, int baseval)
{
	return (flag & 1) ? baseval : -baseval;
}

/* Decode the triplet-encoded point deltas into absolute coordinates. */
static int w2_triplet_decode(const w2_u8 *flags_in, const w2_u8 *in,
		size_t in_size, unsigned int n_points, struct w2_point *result,
		size_t *in_bytes_consumed)
{
	int x = 0;
	int y = 0;
	unsigned int triplet_index = 0;
	unsigned int i;

	if (n_points > in_size)
		return 0;
	for (i = 0; i < n_points; i++) {
		w2_u8 flag = flags_in[i];
		int on_curve = !(flag >> 7);
		unsigned int n_data_bytes;
		int dx, dy;

		flag &= 0x7f;
		if (flag < 84)
			n_data_bytes = 1;
		else if (flag < 120)
			n_data_bytes = 2;
		else if (flag < 124)
			n_data_bytes = 3;
		else
			n_data_bytes = 4;
		if (triplet_index + n_data_bytes > in_size ||
				triplet_index + n_data_bytes < triplet_index)
			return 0;
		if (flag < 10) {
			dx = 0;
			dy = w2_with_sign(flag,
					((flag & 14) << 7) + in[triplet_index]);
		} else if (flag < 20) {
			dx = w2_with_sign(flag,
					(((flag - 10) & 14) << 7) + in[triplet_index]);
			dy = 0;
		} else if (flag < 84) {
			int b0 = flag - 20;
			int b1 = in[triplet_index];
			dx = w2_with_sign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
			dy = w2_with_sign(flag >> 1,
					1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f));
		} else if (flag < 120) {
			int b0 = flag - 84;
			dx = w2_with_sign(flag, 1 + ((b0 / 12) << 8) +
					in[triplet_index]);
			dy = w2_with_sign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) +
					in[triplet_index + 1]);
		} else if (flag < 124) {
			int b2 = in[triplet_index + 1];
			dx = w2_with_sign(flag,
					(in[triplet_index] << 4) + (b2 >> 4));
			dy = w2_with_sign(flag >> 1,
					((b2 & 0x0f) << 8) + in[triplet_index + 2]);
		} else {
			dx = w2_with_sign(flag,
					(in[triplet_index] << 8) + in[triplet_index + 1]);
			dy = w2_with_sign(flag >> 1,
					(in[triplet_index + 2] << 8) +
					in[triplet_index + 3]);
		}
		triplet_index += n_data_bytes;
		if ((x > 0 && dx > 2147483647 - x) || (x < 0 && dx < -2147483647 - x))
			return 0;
		if ((y > 0 && dy > 2147483647 - y) || (y < 0 && dy < -2147483647 - y))
			return 0;
		x += dx;
		y += dy;
		result[i].x = x;
		result[i].y = y;
		result[i].on_curve = on_curve;
	}
	*in_bytes_consumed = triplet_index;
	return 1;
}

/* Re-encode the point deltas into standard glyf flag/x/y format. */
static int w2_store_points(unsigned int n_points, const struct w2_point *points,
		unsigned int n_contours, unsigned int instruction_length,
		w2_u8 *dst, size_t dst_size, size_t *glyph_size)
{
	unsigned int flag_offset = W2_ENDPTS_OFFSET + 2 * n_contours + 2 +
			instruction_length;
	int last_flag = -1;
	int repeat_count = 0;
	int last_x = 0;
	int last_y = 0;
	unsigned int x_bytes = 0;
	unsigned int y_bytes = 0;
	size_t x_offset, y_offset;
	unsigned int i;

	for (i = 0; i < n_points; i++) {
		const struct w2_point *point = &points[i];
		int flag = point->on_curve ? W2_GLYF_ON_CURVE : 0;
		int dx = point->x - last_x;
		int dy = point->y - last_y;

		if (dx == 0) {
			flag |= W2_GLYF_X_SAME;
		} else if (dx > -256 && dx < 256) {
			flag |= W2_GLYF_X_SHORT | (dx > 0 ? W2_GLYF_X_SAME : 0);
			x_bytes += 1;
		} else {
			x_bytes += 2;
		}
		if (dy == 0) {
			flag |= W2_GLYF_Y_SAME;
		} else if (dy > -256 && dy < 256) {
			flag |= W2_GLYF_Y_SHORT | (dy > 0 ? W2_GLYF_Y_SAME : 0);
			y_bytes += 1;
		} else {
			y_bytes += 2;
		}

		if (flag == last_flag && repeat_count != 255) {
			dst[flag_offset - 1] |= W2_GLYF_REPEAT;
			repeat_count++;
		} else {
			if (repeat_count != 0) {
				if (flag_offset >= dst_size)
					return 0;
				dst[flag_offset++] = (w2_u8) repeat_count;
			}
			if (flag_offset >= dst_size)
				return 0;
			dst[flag_offset++] = (w2_u8) flag;
			repeat_count = 0;
		}
		last_x = point->x;
		last_y = point->y;
		last_flag = flag;
	}
	if (repeat_count != 0) {
		if (flag_offset >= dst_size)
			return 0;
		dst[flag_offset++] = (w2_u8) repeat_count;
	}
	if (x_bytes + y_bytes < x_bytes ||
			flag_offset + x_bytes + y_bytes < flag_offset ||
			flag_offset + x_bytes + y_bytes > dst_size)
		return 0;

	x_offset = flag_offset;
	y_offset = flag_offset + x_bytes;
	last_x = 0;
	last_y = 0;
	for (i = 0; i < n_points; i++) {
		int dx = points[i].x - last_x;
		int dy = points[i].y - last_y;
		if (dx == 0) {
			/* pass */
		} else if (dx > -256 && dx < 256) {
			dst[x_offset++] = (w2_u8) (dx < 0 ? -dx : dx);
		} else {
			x_offset = w2_store16(dst, x_offset, dx);
		}
		if (dy == 0) {
			/* pass */
		} else if (dy > -256 && dy < 256) {
			dst[y_offset++] = (w2_u8) (dy < 0 ? -dy : dy);
		} else {
			y_offset = w2_store16(dst, y_offset, dy);
		}
		last_x += dx;
		last_y += dy;
	}
	*glyph_size = y_offset;
	return 1;
}

/* Compute the bounding box into a glyf buffer at offset 2. */
static void w2_compute_bbox(unsigned int n_points,
		const struct w2_point *points, w2_u8 *dst)
{
	int x_min = 0, y_min = 0, x_max = 0, y_max = 0;
	size_t offset = 2;
	unsigned int i;

	if (n_points > 0) {
		x_min = points[0].x;
		x_max = points[0].x;
		y_min = points[0].y;
		y_max = points[0].y;
	}
	for (i = 1; i < n_points; i++) {
		int x = points[i].x;
		int y = points[i].y;
		if (x < x_min) x_min = x;
		if (x > x_max) x_max = x;
		if (y < y_min) y_min = y;
		if (y > y_max) y_max = y;
	}
	offset = w2_store16(dst, offset, x_min);
	offset = w2_store16(dst, offset, y_min);
	offset = w2_store16(dst, offset, x_max);
	offset = w2_store16(dst, offset, y_max);
}

/* Measure a composite glyph's component list (no data copy). Takes the
 * stream BY VALUE, matching the reference decoder: the walk must not
 * advance the caller's stream - the caller reads composite_size bytes of
 * real data afterward, and double-advancing misaligns every later glyph. */
static int w2_size_of_composite(struct w2_buf stream, size_t *size,
		int *have_instructions)
{
	size_t start_offset = stream.off;
	int we_have_instructions = 0;
	w2_u16 flags = W2_MORE_COMPONENTS;

	while (flags & W2_MORE_COMPONENTS) {
		size_t arg_size;
		if (!w2_buf_read_u16(&stream, &flags))
			return 0;
		if (flags & W2_WE_HAVE_INSTRUCTIONS)
			we_have_instructions = 1;
		arg_size = 2;   /* glyph index */
		if (flags & W2_ARG_1_AND_2_ARE_WORDS)
			arg_size += 4;
		else
			arg_size += 2;
		if (flags & W2_WE_HAVE_A_SCALE)
			arg_size += 2;
		else if (flags & W2_WE_HAVE_AN_X_AND_Y_SCALE)
			arg_size += 4;
		else if (flags & W2_WE_HAVE_A_TWO_BY_TWO)
			arg_size += 8;
		if (!w2_buf_read(&stream, NULL, arg_size))
			return 0;
	}
	*size = stream.off - start_offset;
	*have_instructions = we_have_instructions;
	return 1;
}

/* Build the TrueType loca table from glyph offsets. */
static int w2_store_loca(const w2_u32 *loca_values, size_t n_values,
		int index_format, w2_u32 *checksum, struct w2_out *out)
{
	size_t offset_size = index_format ? 4 : 2;
	size_t loca_size = n_values * offset_size;
	w2_u8 *loca_content;
	size_t offset = 0;
	size_t i;

	if (loca_size < n_values)
		return 0;       /* overflow */
	loca_content = (w2_u8 *) malloc(loca_size);
	if (loca_content == NULL)
		return 0;
	for (i = 0; i < n_values; i++) {
		w2_u32 value = loca_values[i];
		if (index_format)
			offset = w2_store32(loca_content, offset, value);
		else
			offset = w2_store16(loca_content, offset,
					(int) (value >> 1));
	}
	*checksum = w2_compute_ulong_sum(loca_content, loca_size);
	if (!w2_out_append(out, loca_content, loca_size)) {
		free(loca_content);
		return 0;
	}
	free(loca_content);
	return 1;
}

/* Reconstruct the entire glyf table from its 7 transformed substreams, and
 * append the derived loca table right after it. */
static int w2_reconstruct_glyf(const w2_u8 *data, struct w2_table *glyf_table,
		w2_u32 *glyf_checksum, struct w2_table *loca_table,
		w2_u32 *loca_checksum, struct w2_glyph_info *info,
		struct w2_out *out)
{
	static const int k_num_substreams = 7;
	struct w2_buf file;
	w2_u32 version;
	struct { const w2_u8 *d; size_t len; size_t off; } sub[7];
	struct w2_buf n_contour_stream, n_points_stream, flag_stream,
			glyph_stream, composite_stream, bbox_stream,
			instruction_stream;
	const size_t glyf_start = out->len;
	w2_u32 *loca_values;
	size_t offset;
	unsigned int i;
	const w2_u8 *bbox_bitmap;
	unsigned int bitmap_length;
	w2_u8 *glyph_buf;
	size_t glyph_buf_size;
	w2_u32 glyf_check = 0;
	w2_u16 n_contours = 0;
	int have_bbox = 0;

	w2_buf_init(&file, data, glyf_table->transform_length);
	if (!w2_buf_read_u32(&file, &version))
		return 0;
	if (!w2_buf_read_u16(&file, &info->num_glyphs) ||
			!w2_buf_read_u16(&file, &info->index_format))
		return 0;

	offset = (2 + k_num_substreams) * 4;
	if (offset > glyf_table->transform_length)
		return 0;
	for (i = 0; i < (unsigned int) k_num_substreams; i++) {
		w2_u32 substream_size;
		if (!w2_buf_read_u32(&file, &substream_size))
			return 0;
		if (substream_size > glyf_table->transform_length - offset)
			return 0;
		sub[i].d = data + offset;
		sub[i].len = substream_size;
		sub[i].off = 0;
		offset += substream_size;
	}
	w2_buf_init(&n_contour_stream, sub[0].d, sub[0].len);
	w2_buf_init(&n_points_stream, sub[1].d, sub[1].len);
	w2_buf_init(&flag_stream, sub[2].d, sub[2].len);
	w2_buf_init(&glyph_stream, sub[3].d, sub[3].len);
	w2_buf_init(&composite_stream, sub[4].d, sub[4].len);
	w2_buf_init(&bbox_stream, sub[5].d, sub[5].len);
	w2_buf_init(&instruction_stream, sub[6].d, sub[6].len);

	loca_values = (w2_u32 *) malloc(((size_t) info->num_glyphs + 1) *
			sizeof(w2_u32));
	if (loca_values == NULL)
		return 0;
	info->x_mins = (w2_s16 *) malloc((size_t) info->num_glyphs *
			sizeof(w2_s16));
	if (info->x_mins == NULL) {
		free(loca_values);
		return 0;
	}

	bbox_bitmap = bbox_stream.d + bbox_stream.off;
	bitmap_length = ((info->num_glyphs + 31) >> 5) << 2;
	if (!w2_buf_read(&bbox_stream, NULL, bitmap_length)) {
		free(loca_values);
		free(info->x_mins);
		info->x_mins = NULL;
		return 0;
	}

	glyph_buf = (w2_u8 *) malloc(W2_DEFAULT_GLYPH_BUF);
	if (glyph_buf == NULL) {
		free(loca_values);
		free(info->x_mins);
		info->x_mins = NULL;
		return 0;
	}
	glyph_buf_size = W2_DEFAULT_GLYPH_BUF;

	for (i = 0; i < info->num_glyphs; i++) {
		size_t glyph_size = 0;
		w2_u32 *n_points_vec = NULL;

		have_bbox = 0;   /* per-glyph, like the reference's loop-local */
		if (bbox_bitmap[i >> 3] & (0x80 >> (i & 7)))
			have_bbox = 1;
		if (!w2_buf_read_u16(&n_contour_stream, &n_contours))
			{ goto glyf_fail; }

		if (n_contours == 0xffff) {
			/* composite glyph */
			int have_instructions = 0;
			unsigned int instruction_size = 0;
			size_t composite_size;
			size_t size_needed;
			w2_u8 *nb;

			if (!have_bbox)
				{ goto glyf_fail; }  /* composites must carry a bbox */
			if (!w2_size_of_composite(composite_stream,
					&composite_size, &have_instructions))
				{ goto glyf_fail; }
			if (have_instructions &&
					!w2_read_255us16(&glyph_stream,
						&instruction_size))
				{ goto glyf_fail; }

			size_needed = 12 + composite_size + instruction_size;
			if (glyph_buf_size < size_needed) {
				nb = (w2_u8 *) realloc(glyph_buf, size_needed);
				if (nb == NULL)
					{ goto glyf_fail; }
				glyph_buf = nb;
				glyph_buf_size = size_needed;
			}
			glyph_size = w2_store16(glyph_buf, glyph_size, n_contours);
			if (!w2_buf_read(&bbox_stream, glyph_buf + glyph_size, 8))
				{ goto glyf_fail; }
			glyph_size += 8;
			if (!w2_buf_read(&composite_stream,
					glyph_buf + glyph_size, composite_size))
				{ goto glyf_fail; }
			glyph_size += composite_size;
			if (have_instructions) {
				glyph_size = w2_store16(glyph_buf, glyph_size,
						(int) instruction_size);
				if (!w2_buf_read(&instruction_stream,
						glyph_buf + glyph_size,
						instruction_size))
					{ goto glyf_fail; }
				glyph_size += instruction_size;
			}
		} else if (n_contours > 0) {
			/* simple glyph */
			unsigned int total_n_points = 0;
			unsigned int n_points_contour;
			unsigned int flag_size;
			const w2_u8 *flags_buf;
			const w2_u8 *triplet_buf;
			size_t triplet_size;
			size_t triplet_bytes_consumed = 0;
			unsigned int instruction_size;
			size_t size_needed;
			struct w2_point *points;
			unsigned int j;

			n_points_vec = (w2_u32 *) malloc((size_t) n_contours *
					sizeof(w2_u32));
			if (n_points_vec == NULL)
				{ goto glyf_fail; }
			for (j = 0; j < n_contours; j++) {
				if (!w2_read_255us16(&n_points_stream,
						&n_points_contour))
					{ goto glyf_fail; }
				n_points_vec[j] = n_points_contour;
				if (total_n_points + n_points_contour <
						total_n_points)
					{ goto glyf_fail; }
				total_n_points += n_points_contour;
			}
			flag_size = total_n_points;
			if (flag_size > flag_stream.len - flag_stream.off)
				{ goto glyf_fail; }
			flags_buf = flag_stream.d + flag_stream.off;
			triplet_buf = glyph_stream.d + glyph_stream.off;
			triplet_size = glyph_stream.len - glyph_stream.off;
			points = (struct w2_point *) malloc(
					(size_t) total_n_points *
					sizeof(struct w2_point));
			if (points == NULL)
				{ goto glyf_fail; }
			if (!w2_triplet_decode(flags_buf, triplet_buf,
					triplet_size, total_n_points, points,
					&triplet_bytes_consumed)) {
				free(points);
				goto glyf_fail;
			}
			if (!w2_buf_read(&flag_stream, NULL, flag_size)) {
				free(points);
				goto glyf_fail;
			}
			if (!w2_buf_read(&glyph_stream, NULL,
					triplet_bytes_consumed)) {
				free(points);
				goto glyf_fail;
			}
			if (!w2_read_255us16(&glyph_stream, &instruction_size)) {
				free(points);
				goto glyf_fail;
			}
			if (total_n_points >= (1u << 27) ||
					instruction_size >= (1u << 30)) {
				free(points);
				goto glyf_fail;
			}
			size_needed = 12 + 2 * n_contours +
					5 * total_n_points + instruction_size;
			if (glyph_buf_size < size_needed) {
				w2_u8 *nb = (w2_u8 *) realloc(glyph_buf,
						size_needed);
				if (nb == NULL) {
					free(points);
					goto glyf_fail;
				}
				glyph_buf = nb;
				glyph_buf_size = size_needed;
			}
			glyph_size = w2_store16(glyph_buf, glyph_size,
					n_contours);
			if (have_bbox) {
				if (!w2_buf_read(&bbox_stream,
						glyph_buf + glyph_size, 8)) {
					free(points);
					goto glyf_fail;
				}
			} else {
				w2_compute_bbox(total_n_points, points, glyph_buf);
			}
			glyph_size = W2_ENDPTS_OFFSET;
			{
				int end_point = -1;
				for (j = 0; j < n_contours; j++) {
					end_point += (int) n_points_vec[j];
					if (end_point >= 65536) {
						free(points);
						goto glyf_fail;
					}
					glyph_size = w2_store16(glyph_buf,
							glyph_size, end_point);
				}
			}
			glyph_size = w2_store16(glyph_buf, glyph_size,
					(int) instruction_size);
			if (!w2_buf_read(&instruction_stream,
					glyph_buf + glyph_size,
					instruction_size)) {
				free(points);
				goto glyf_fail;
			}
			glyph_size += instruction_size;
			if (!w2_store_points(total_n_points, points, n_contours,
					instruction_size, glyph_buf, glyph_buf_size,
					&glyph_size)) {
				free(points);
				goto glyf_fail;
			}
			free(points);
		}
		if (n_points_vec != NULL) {
			free(n_points_vec);
			n_points_vec = NULL;
		}

		loca_values[i] = (w2_u32) (out->len - glyf_start);
		if (!w2_out_append(out, glyph_buf, glyph_size))
			{ goto glyf_fail; }
		if (!w2_out_pad4(out))
			{ goto glyf_fail; }
		glyf_check += w2_compute_ulong_sum(glyph_buf, glyph_size);
		if (n_contours > 0) {
			struct w2_buf x_min_buf;
			w2_s16 x_min;
			w2_buf_init(&x_min_buf, glyph_buf + 2, 2);
			if (!w2_buf_read_s16(&x_min_buf, &x_min))
				{ goto glyf_fail; }
			info->x_mins[i] = x_min;
		}
	}
	free(glyph_buf);

	glyf_table->dst_length = (w2_u32) (out->len - glyf_table->dst_offset);
	loca_table->dst_offset = (w2_u32) out->len;
	loca_values[info->num_glyphs] = glyf_table->dst_length;
	if (!w2_store_loca(loca_values, (size_t) info->num_glyphs + 1,
			info->index_format, loca_checksum, out)) {
		free(loca_values);
		free(info->x_mins);
		info->x_mins = NULL;
		return 0;
	}
	loca_table->dst_length = (w2_u32) (out->len - loca_table->dst_offset);
	free(loca_values);
	*glyf_checksum = glyf_check;
	return 1;

glyf_fail:
	free(glyph_buf);
	free(loca_values);
	free(info->x_mins);
	info->x_mins = NULL;
	return 0;
}

/* Reconstruct the transformed hmtx table (WOFF2 spec 6.2 / hmtx section). */
static int w2_reconstruct_hmtx(const w2_u8 *transformed,
		size_t transformed_size, w2_u16 num_glyphs, w2_u16 num_hmetrics,
		const w2_s16 *x_mins, w2_u32 *checksum, struct w2_out *out)
{
	struct w2_buf in;
	w2_u8 hmtx_flags;
	int has_proportional_lsbs, has_monospace_lsbs;
	w2_u16 *advance_widths;
	w2_s16 *lsbs;
	size_t n_adv, n_lsb, i;
	w2_u32 hmtx_output_size;
	w2_u8 *hmtx_table;
	size_t dst_offset = 0;

	w2_buf_init(&in, transformed, transformed_size);
	if (!w2_buf_read_u8(&in, &hmtx_flags))
		return 0;
	has_proportional_lsbs = (hmtx_flags & 1) == 0;
	has_monospace_lsbs = (hmtx_flags & 2) == 0;
	if (has_proportional_lsbs && has_monospace_lsbs)
		return 0;
	if (num_hmetrics > num_glyphs)
		return 0;
	if (num_hmetrics < 1)
		return 0;

	n_adv = num_hmetrics;
	n_lsb = num_glyphs;
	advance_widths = (w2_u16 *) malloc(n_adv * sizeof(w2_u16));
	lsbs = (w2_s16 *) malloc(n_lsb * sizeof(w2_s16));
	if (advance_widths == NULL || lsbs == NULL) {
		free(advance_widths);
		free(lsbs);
		return 0;
	}
	for (i = 0; i < num_hmetrics; i++) {
		w2_u16 advance_width;
		if (!w2_buf_read_u16(&in, &advance_width))
			goto hmtx_fail;
		advance_widths[i] = advance_width;
	}
	for (i = 0; i < num_hmetrics; i++) {
		w2_s16 lsb;
		if (has_proportional_lsbs) {
			if (!w2_buf_read_s16(&in, &lsb))
				goto hmtx_fail;
		} else {
			lsb = x_mins[i];
		}
		lsbs[i] = lsb;
	}
	for (i = num_hmetrics; i < num_glyphs; i++) {
		w2_s16 lsb;
		if (has_monospace_lsbs) {
			if (!w2_buf_read_s16(&in, &lsb))
				goto hmtx_fail;
		} else {
			lsb = x_mins[i];
		}
		lsbs[i] = lsb;
	}

	hmtx_output_size = 2 * (w2_u32) num_glyphs + 2 * (w2_u32) num_hmetrics;
	hmtx_table = (w2_u8 *) malloc(hmtx_output_size);
	if (hmtx_table == NULL)
		goto hmtx_fail;
	for (i = 0; i < num_glyphs; i++) {
		if (i < num_hmetrics)
			w2_store16_ptr(hmtx_table, &dst_offset,
					(int) advance_widths[i]);
		w2_store16_ptr(hmtx_table, &dst_offset, (int) lsbs[i]);
	}
	*checksum = w2_compute_ulong_sum(hmtx_table, hmtx_output_size);
	if (!w2_out_append(out, hmtx_table, hmtx_output_size)) {
		free(hmtx_table);
		goto hmtx_fail;
	}
	free(hmtx_table);
	free(advance_widths);
	free(lsbs);
	return 1;

hmtx_fail:
	free(advance_widths);
	free(lsbs);
	return 0;
}

/* Read numberOfHMetrics from an (untransformed) hhea copy. */
static int w2_read_num_hmetrics(const w2_u8 *data, size_t data_size,
		w2_u16 *num_hmetrics)
{
	struct w2_buf buffer;
	w2_buf_init(&buffer, data, data_size);
	if (!w2_buf_read(&buffer, NULL, 34))
		return 0;
	if (!w2_buf_read_u16(&buffer, num_hmetrics))
		return 0;
	return 1;
}

/* Reconstruct one font into the output (single font; TTC rejected earlier). */
static int w2_reconstruct_font(const w2_u8 *transformed,
		size_t transformed_size, struct w2_hdr *hdr,
		struct w2_tag_off *entries, w2_u32 header_checksum,
		struct w2_out *out)
{
	struct w2_glyph_info info;
	struct w2_table *loca_table = NULL;
	w2_u32 loca_checksum = 0;
	w2_u32 font_checksum = header_checksum;
	size_t i;
	w2_u8 table_entry[12];

	memset(&info, 0, sizeof(info));

	/* glyf without loca (or vice versa) makes no sense */
	{
		int have_glyf = 0, have_loca = 0;
		for (i = 0; i < hdr->num_tables; i++) {
			if (hdr->tables[i].tag == W2_TAG_GLYF)
				have_glyf = 1;
			if (hdr->tables[i].tag == W2_TAG_LOCA)
				have_loca = 1;
		}
		if (have_glyf != have_loca)
			return 0;
	}

	for (i = 0; i < hdr->num_tables; i++) {
		struct w2_table *table = &hdr->tables[i];
		w2_u32 checksum = 0;
		size_t entry_off;

		if (table->src_offset + table->src_length > transformed_size)
			return 0;

		if (table->tag == W2_TAG_HHEA) {
			if (!w2_read_num_hmetrics(
					transformed + table->src_offset,
					table->src_length, &info.num_hmetrics))
				return 0;
		}

		if ((table->flags & W2_FLAGS_TRANSFORM) != W2_FLAGS_TRANSFORM) {
			if (table->tag == W2_TAG_HEAD) {
				/* zero checkSumAdjustment before checksumming; the
				 * real value is patched in after the whole font */
				static const w2_u8 adj_zero[4] = { 0, 0, 0, 0 };
				if (table->src_length < 12)
					return 0;
				memcpy((w2_u8 *) transformed + table->src_offset +
						W2_CHECKSUM_ADJ_OFFSET, adj_zero, 4);
			}
			table->dst_offset = (w2_u32) out->len;
			checksum = w2_compute_ulong_sum(
					transformed + table->src_offset,
					table->src_length);
			if (!w2_out_append(out, transformed + table->src_offset,
					table->src_length))
				return 0;
		} else {
			if (table->tag == W2_TAG_GLYF) {
				table->dst_offset = (w2_u32) out->len;
				if (loca_table == NULL)
					loca_table = w2_find_table(hdr, W2_TAG_LOCA);
				if (loca_table == NULL)
					return 0;
				if (!w2_reconstruct_glyf(
						transformed + table->src_offset,
						table, &checksum, loca_table,
						&loca_checksum, &info, out))
					return 0;
			} else if (table->tag == W2_TAG_LOCA) {
				/* all the work was done by ReconstructGlyf */
				checksum = loca_checksum;
			} else if (table->tag == W2_TAG_HMTX) {
				table->dst_offset = (w2_u32) out->len;
				if (!w2_reconstruct_hmtx(
						transformed + table->src_offset,
						table->src_length, info.num_glyphs,
						info.num_hmetrics, info.x_mins,
						&checksum, out))
					return 0;
			} else {
				return 0;   /* unknown transform */
			}
		}
		font_checksum += checksum;

		/* patch the directory entry with real values */
		entry_off = w2_entry_offset(entries, hdr->num_tables, table->tag);
		{
			size_t te_off = 0;
			w2_store32_ptr(table_entry, &te_off, checksum);
			w2_store32_ptr(table_entry, &te_off, table->dst_offset);
			w2_store32_ptr(table_entry, &te_off, table->dst_length);
		}
		if (!w2_out_write_at(out, table_entry, entry_off + 4, 12))
			return 0;
		font_checksum += w2_compute_ulong_sum(table_entry, 12);

		if (!w2_out_pad4(out))
			return 0;
		if (table->dst_offset + table->dst_length > out->len)
			return 0;
	}

	/* checkSumAdjustment into 'head' (we zeroed it during the copy) */
	{
		struct w2_table *head = w2_find_table(hdr, W2_TAG_HEAD);
		if (head != NULL) {
			w2_u8 adjustment[4];
			if (head->dst_length < 12)
				return 0;
			w2_store32(adjustment, 0, 0xB1B0AFBAUL - font_checksum);
			if (!w2_out_write_at(out, adjustment,
					head->dst_offset + W2_CHECKSUM_ADJ_OFFSET, 4))
				return 0;
		}
	}
	if (info.x_mins != NULL) {
		free(info.x_mins);
		info.x_mins = NULL;
	}
	return 1;
}

/* ---- public entry ------------------------------------------------------- */

int
macos9_woff2_to_ttf(const unsigned char *src, long src_len,
		unsigned char **out, long *out_len)
{
	struct w2_hdr hdr;
	struct w2_out o;
	struct w2_tag_off *entries;
	w2_u32 header_checksum = 0;
	w2_u8 *uncompressed;
	size_t uncompressed_size;

	*out = NULL;
	*out_len = 0;

	memset(&hdr, 0, sizeof(hdr));
	memset(&o, 0, sizeof(o));

	if (src == NULL || src_len < 48)
		return 0;
	if (!w2_read_woff2_header(src, (size_t) src_len, &hdr))
		return 0;

	/* sanity: a woff2 that decompresses >100x its file size is bogus */
	if (hdr.uncompressed_size < 1 ||
			(double) hdr.uncompressed_size / (double) src_len >
				W2_MAX_PLAUSIBLE_RATIO ||
			hdr.uncompressed_size > W2_MAX_UNCOMPRESSED)
		goto fail;

	uncompressed = (w2_u8 *) malloc(hdr.uncompressed_size);
	if (uncompressed == NULL)
		goto fail;
	uncompressed_size = hdr.uncompressed_size;
	if (BrotliDecoderDecompress(hdr.compressed_length,
			src + hdr.compressed_offset, &uncompressed_size,
			uncompressed) != 1 ||
			uncompressed_size != hdr.uncompressed_size) {
		free(uncompressed);
		goto fail;
	}

	entries = (struct w2_tag_off *) malloc(hdr.num_tables *
			sizeof(struct w2_tag_off));
	if (entries == NULL) {
		free(uncompressed);
		goto fail;
	}
	if (!w2_write_headers(&hdr, &o, entries, &header_checksum)) {
		free(entries);
		free(uncompressed);
		goto fail;
	}
	if (!w2_reconstruct_font(uncompressed, hdr.uncompressed_size,
			&hdr, entries, header_checksum, &o)) {
		free(entries);
		free(uncompressed);
		free(o.d);
		o.d = NULL;
		goto fail;
	}
	free(entries);
	free(uncompressed);
	w2_hdr_destroy(&hdr);

	*out = o.d;
	*out_len = (long) o.len;
	return 1;

fail:
	w2_hdr_destroy(&hdr);
	if (o.d != NULL)
		free(o.d);
	return 0;
}
