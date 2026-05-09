/*
 * Copyright 2014 Chris Young <chris@unsatisfactorysoftware.co.uk>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * NetSurf international domain name handling implementation.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "netsurf/inttypes.h"

#include "utils/errors.h"
#include "utils/idna.h"
#include "utils/idna_props.h"
#include "utils/log.h"
#include "utils/punycode.h"
#include "utils/utf8.h"
#include "utils/utils.h"


/**
 * Convert punycode status into nserror.
 *
 * \param status The punycode status to convert.
 * \return The corresponding nserror code for the status.
 */
static nserror punycode_status_to_nserror(enum punycode_status status)
{
	nserror ret = NSERROR_NOMEM;

	switch (status) {
	case punycode_success:
		ret = NSERROR_OK;
		break;

	case punycode_bad_input:
		NSLOG(netsurf, INFO, "Bad input");
		ret = NSERROR_BAD_ENCODING;
		break;

	case punycode_big_output:
		NSLOG(netsurf, INFO, "Output too big");
		ret = NSERROR_BAD_SIZE;
		break;

	case punycode_overflow:
		NSLOG(netsurf, INFO, "Overflow");
		ret = NSERROR_NOSPACE;
		break;

	default:
		break;
	}
	return ret;
}


/**
 * Convert a host label in UCS-4 to an ACE version
 *
 * \param ucs4_label UCS-4 NFC string containing host label
 * \param len Length of host label (in characters/codepoints)
 * \param ace_label ASCII-compatible encoded version
 * \param out_len Length of ace_label
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * If return value != NSERROR_OK, output will be left untouched.
 */
static nserror
idna__ucs4_to_ace(int32_t *ucs4_label,
		  size_t len,
		  char **ace_label,
		  size_t *out_len)
{
	char punycode[65]; /* max length of host label + NULL */
	size_t output_length = 60; /* punycode length - 4 - 1 */
	nserror ret;

	punycode[0] = 'x';
	punycode[1] = 'n';
	punycode[2] = '-';
	punycode[3] = '-';

	ret = punycode_status_to_nserror(punycode_encode(len,
			(const punycode_uint *)ucs4_label, NULL,
			&output_length, punycode + 4));
	if (ret != NSERROR_OK) {
		return ret;
	}

	output_length += SLEN("xn--");
	punycode[output_length] = '\0';

	*ace_label = strdup(punycode);
	*out_len = output_length;

	return NSERROR_OK;
}


/**
 * Convert a host label in ACE format to UCS-4
 *
 * \param ace_label ASCII string containing host label
 * \param ace_len Length of host label
 * \param ucs4_label Pointer to hold UCS4 decoded version
 * \param ucs4_len Pointer to hold length of ucs4_label
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * If return value != NSERROR_OK, output will be left untouched.
 */
static nserror
idna__ace_to_ucs4(const char *ace_label,
		  size_t ace_len,
		  int32_t **ucs4_label,
		  size_t *ucs4_len)
{
	int32_t *ucs4;
	nserror ret;
	size_t output_length = ace_len; /* never exceeds input length */

	/* The header should always have been checked before calling */
	assert((ace_label[0] == 'x') && (ace_label[1] == 'n') &&
		(ace_label[2] == '-') && (ace_label[3] == '-'));

	ucs4 = malloc(output_length * 4);
	if (ucs4 == NULL) {
		return NSERROR_NOMEM;
	}

	ret = punycode_status_to_nserror(punycode_decode(ace_len - 4,
		ace_label + 4, &output_length, (punycode_uint *)ucs4, NULL));
	if (ret != NSERROR_OK) {
		free(ucs4);
		return ret;
	}

	ucs4[output_length] = '\0';

	*ucs4_label = ucs4;
	*ucs4_len = output_length;

	return NSERROR_OK;
}


#ifdef WITH_UTF8PROC

#include <utf8proc.h>

int32_t idna_contexto[] = {
	/* CONTEXTO codepoints which have a rule defined */
	0x00b7, 0x0375, 0x05f3, 0x05f4, 0x30fb, 0x0660, 0x0661,
	0x0662, 0x0663, 0x0664, 0x0665, 0x0666, 0x0667, 0x0668,
	0x0669, 0x06f0, 0x06f1, 0x06f2, 0x06f3, 0x06f4, 0x06f5,
	0x06f6, 0x06f7, 0x06f8, 0x06f9, 0
};

/**
 * Find the IDNA property of a UCS-4 codepoint
 *
 * \param cp	Unicode codepoint
 * \return IDNA property
 */
static idna_property idna__cp_property(int32_t cp)
{
	const idna_table *t;

	t = idna_derived;
	while (t->p.property) {
		if ((cp >= t->start) && (cp <= t->end)) {
			return t->p.property;
		}
		t++;
	};

	return IDNA_P_DISALLOWED;
}


/**
 * Find the Joining_Type property of a UCS-4 codepoint
 *
 * \param cp	Unicode codepoint
 * \return JT property
 */
static idna_unicode_jt idna__jt_property(int32_t cp)
{
	const idna_table *t;

	t = idna_joiningtype;
	while (t->p.jt) {
		if ((cp >= t->start) && (cp <= t->end)) {
			return t->p.jt;
		}
		t++;
	};

	return IDNA_UNICODE_JT_U;
}


/**
 * Check if a CONTEXTO codepoint has a rule defined
 *
 * \param cp	Unicode codepoint
 * \return true if a rule is defined
 */
static bool idna__contexto_rule(int32_t cp)
{
	int32_t *t;
	for (t = idna_contexto; *t != 0; t++) {
		if (*t == cp) {
			return true;
		}
	}

	return false;
}


/**
 * Check if a CONTEXTJ codepoint has a rule defined,
 * and conforms to that rule.
 *
 * \param label UCS-4 string
 * \param index	character in the string which is CONTEXTJ
 * \param len The length of the label
 * \return true if conforming
 */
static bool idna__contextj_rule(int32_t *label, int index, size_t len)
{
	const utf8proc_property_t *unicode_props;
	idna_unicode_jt joining_type;
	int i;
	bool match;

	/* These CONTEXTJ rules are defined at
	 * http://www.iana.org/assignments/idna-tables-5.2.0/idna-tables-5.2.0.xml
	 */

	if (label[index] == 0x200c) {
		if (index == 0) {
			return false; /* No previous character */
		}
		unicode_props = utf8proc_get_property(label[index - 1]);
		if (unicode_props->combining_class == IDNA_UNICODE_CCC_VIRAMA) {
			return true;
		}

		match = false;
		for (i = 0; i < (index - 1); i++) {
			joining_type = idna__jt_property(label[i]);
			if (((joining_type == IDNA_UNICODE_JT_L) ||
			     (joining_type == IDNA_UNICODE_JT_D)) &&
			    (idna__jt_property(label[i+1]) == IDNA_UNICODE_JT_T)) {
				match = true;
				break;
			}
		}

		if (match == false) {
			return false;
		}

		if (idna__jt_property(label[index+1]) != IDNA_UNICODE_JT_T) {
			return false;
		}

		for (i = (index + 1); i < (int)len; i++) {
			joining_type = idna__jt_property(label[i]);

			if ((joining_type == IDNA_UNICODE_JT_R) ||
			    (joining_type == IDNA_UNICODE_JT_D)) {
				return true;
			}
		}

		return false;

	} else if (label[index] == 0x200d) {
		if (index == 0) {
			return false; /* No previous character */
		}
		unicode_props = utf8proc_get_property(label[index - 1]);
		if (unicode_props->combining_class == IDNA_UNICODE_CCC_VIRAMA) {
			return true;
		}
		return false;
	}

	/* No rule defined */
	return false;
}


/**
 * Convert a UTF-8 string to UCS-4
 *
 * \param utf8_label	UTF-8 string containing host label
 * \param len	Length of host label (in bytes)
 * \param ucs4_label	Pointer to update with the output
 * \param ucs4_len	Pointer to update with the length
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * If return value != NSERROR_OK, output will be left untouched.
 */
static nserror
idna__utf8_to_ucs4(const char *utf8_label,
		   size_t len,
		   int32_t **ucs4_label,
		   size_t *ucs4_len)
{
	int32_t *nfc_label;
	ssize_t nfc_size;

	nfc_label = malloc(len * 4);
	if (nfc_label == NULL) {
		return NSERROR_NOMEM;
	}

	nfc_size = utf8proc_decompose((const uint8_t *)utf8_label, len,
		nfc_label, len * 4, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	if (nfc_size < 0) {
		return NSERROR_NOMEM;
	}

	nfc_size = utf8proc_normalize_utf32(nfc_label, nfc_size,
		UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	if (nfc_size < 0) {
		return NSERROR_NOMEM;
	}

	*ucs4_label = nfc_label;
	*ucs4_len = nfc_size;

	return NSERROR_OK;
}


/**
 * Convert a UCS-4 string to UTF-8
 *
 * \param ucs4_label	UCS-4 string containing host label
 * \param ucs4_len	Length of host label (in bytes)
 * \param utf8_label	Pointer to update with the output
 * \param utf8_len	Pointer to update with the length
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * If return value != NSERROR_OK, output will be left untouched.
 */
static nserror
idna__ucs4_to_utf8(const int32_t *ucs4_label,
		   size_t ucs4_len,
		   char **utf8_label,
		   size_t *utf8_len)
{
	int32_t *nfc_label;
	ssize_t nfc_size = ucs4_len;

	nfc_label = malloc(1 + ucs4_len * 4);
	if (nfc_label == NULL) {
		return NSERROR_NOMEM;
	}
	memcpy(nfc_label, ucs4_label, ucs4_len * 4);

	nfc_size = utf8proc_reencode(nfc_label, ucs4_len,
		UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	if (nfc_size < 0) {
		return NSERROR_NOMEM;
	}

	*utf8_label = (char *)nfc_label;
	*utf8_len = nfc_size;

	return NSERROR_OK;
}


/**
 * Check if a host label is valid for IDNA2008
 *
 * \param label	Host label to check (UCS-4)
 * \param len	Length of host label (in characters/codepoints)
 * \return true if compliant, false otherwise
 */
static bool idna__is_valid(int32_t *label, size_t len)
{
	const utf8proc_property_t *unicode_props;
	idna_property idna_prop;
	size_t i = 0;

	/* 1. Check that the string is NFC.
	 * This check is skipped as the conversion to Unicode
	 * does normalisation as part of the conversion.
	 */

	/* 2. Check characters 3 and 4 are not '--'. */
	if ((len >= 4) && (label[2] == 0x002d) && (label[3] == 0x002d)) {
		NSLOG(netsurf, INFO,
		      "Check failed: characters 2 and 3 are '--'");
		return false;
	}

	/* 3. Check the first character is not a combining mark */
	unicode_props = utf8proc_get_property(label[0]);

	if ((unicode_props->category == UTF8PROC_CATEGORY_MN) ||
		(unicode_props->category == UTF8PROC_CATEGORY_MC) ||
		(unicode_props->category == UTF8PROC_CATEGORY_ME)) {
		NSLOG(netsurf, INFO,
		      "Check failed: character 0 is a combining mark");
		return false;
	}

	for (i = 0; i < len; i++) {
		idna_prop = idna__cp_property(label[i]);

		/* 4. Check characters not DISALLOWED by RFC5892 */
		if (idna_prop == IDNA_P_DISALLOWED) {
			NSLOG(netsurf, INFO,
			      "Check failed: character %"PRIsizet" (%"PRIx32") is DISALLOWED",
			      i,
			      label[i]);
			return false;
		}

		/* 5. Check CONTEXTJ characters conform to defined rules */
		if (idna_prop == IDNA_P_CONTEXTJ) {
			if (idna__contextj_rule(label, i, len) == false) {
				NSLOG(netsurf, INFO,
				      "Check failed: character %"PRIsizet" (%"PRIx32") does not conform to CONTEXTJ rule",
				      i,
				      label[i]);
				return false;
			}
		}

		/* 6. Check CONTEXTO characters have a rule defined */
		/** \todo optionally we can check conformance to this rule */
		if (idna_prop == IDNA_P_CONTEXTO) {
			if (idna__contexto_rule(label[i]) == false) {
				NSLOG(netsurf, INFO,
				      "Check failed: character %"PRIsizet" (%"PRIx32") has no CONTEXTO rule defined",
				      i,
				      label[i]);
				return false;
			}
		}

		/* 7. Check characters are not UNASSIGNED */
		if (idna_prop == IDNA_P_UNASSIGNED) {
			NSLOG(netsurf, INFO,
			      "Check failed: character %"PRIsizet" (%"PRIx32") is UNASSIGNED",
			      i,
			      label[i]);
			return false;
		}

		/** \todo 8. (optionally) check Bidi compliance */
	}

	return true;
}


/**
 * Verify an ACE label is valid
 *
 * \param label	Host label to check
 * \param len	Length of label
 * \return true if valid, false otherwise
 */
static bool idna__verify(const char *label, size_t len)
{
	nserror error;
	int32_t *ucs4;
	char *ace;
	ssize_t ucs4_len;
	size_t u_ucs4_len, ace_len;

	/* Convert our ACE label back to UCS-4 */
	error = idna__ace_to_ucs4(label, len, &ucs4, &u_ucs4_len);
	if (error != NSERROR_OK) {
		return false;
	}

	/* Perform NFC normalisation */
	ucs4_len = utf8proc_normalize_utf32(ucs4, u_ucs4_len,
		UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	if (ucs4_len < 0) {
		free(ucs4);
		return false;
	}

	/* Convert the UCS-4 label back to ACE */
	error = idna__ucs4_to_ace(ucs4, (size_t)ucs4_len,
				&ace, &ace_len);
	free(ucs4);
	if (error != NSERROR_OK) {
		return false;
	}

	/* Check if it matches the input */
	if ((len == ace_len) && (strncmp(label, ace, len) == 0)) {
		free(ace);
		return true;
	}

	NSLOG(netsurf, INFO, "Re-encoded ACE label %s does not match input",
	      ace);
	free(ace);

	return false;
}


#else /* WITH_UTF8PROC */


/**
 * Convert a UTF-8 string to UCS-4
 *
 * \param utf8_label	UTF-8 string containing host label
 * \param len	Length of host label (in bytes)
 * \param ucs4_label	Pointer to update with the output
 * \param ucs4_len	Pointer to update with the length
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * If return value != NSERROR_OK, output will be left untouched.
 */
static nserror
idna__utf8_to_ucs4(const char *utf8_label,
		   size_t len,
		   int32_t **ucs4_label,
		   size_t *ucs4_len)
{
	return NSERROR_NOT_IMPLEMENTED;
}


/**
 * Convert a UCS-4 string to UTF-8
 *
 * \param ucs4_label	UCS-4 string containing host label
 * \param ucs4_len	Length of host label (in bytes)
 * \param utf8_label	Pointer to update with the output
 * \param utf8_len	Pointer to update with the length
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * If return value != NSERROR_OK, output will be left untouched.
 */
static nserror
idna__ucs4_to_utf8(const int32_t *ucs4_label,
		   size_t ucs4_len,
		   char **utf8_label,
		   size_t *utf8_len)
{
	return NSERROR_NOT_IMPLEMENTED;
}


/**
 * Check if a host label is valid for IDNA2008
 *
 * \param label	Host label to check (UCS-4)
 * \param len	Length of host label (in characters/codepoints)
 * \return true if compliant, false otherwise
 */
static bool idna__is_valid(int32_t *label, size_t len)
{
	return true;
}


/**
 * Verify an ACE label is valid
 *
 * \param label	Host label to check
 * \param len	Length of label
 * \return true if valid, false otherwise
 */
static bool idna__verify(const char *label, size_t len)
{
	return true;
}


#endif /* WITH_UTF8PROC */


/**
 * Find the length of a host label
 *
 * \param host	String containing a host or FQDN
 * \param max_length	Length of host string to search (in bytes)
 * \return Distance to next separator character or end of string
 */
static size_t idna__host_label_length(const char *host, size_t max_length)
{
	const char *p = host;
	size_t length = 0;

	while (length < max_length) {
		if ((*p == '.') || (*p == ':') || (*p == '\0')) {
			break;
		}
		length++;
		p++;
	}

	return length;
}


/**
 * Check if a host label is LDH
 *
 * \param label	Host label to check
 * \param len	Length of host label
 * \return true if LDH compliant, false otherwise
 */
static bool idna__is_ldh(const char *label, size_t len)
{
	const char *p = label;
	size_t i = 0;

	/* Check for leading or trailing hyphens */
	if ((p[0] == '-') || (p[len - 1] == '-'))
		return false;

	/* Check for non-alphanumeric, non-hyphen characters */
	for (i = 0; i < len; p++) {
		i++;
		if (*p == '-') continue;
		if ((*p >= '0') && (*p <= '9')) continue;
		if ((*p >= 'a') && (*p <= 'z')) continue;
		if ((*p >= 'A') && (*p <= 'Z')) continue;

		return false;
	}

	return true;
}


/**
 * Check if a host label appears to be ACE
 *
 * \param label	Host label to check
 * \param len	Length of host label
 * \return true if ACE compliant, false otherwise
 */
static bool idna__is_ace(const char *label, size_t len)
{
	/* Check it is a valid DNS string */
	if (idna__is_ldh(label, len) == false) {
		return false;
	}

	/* Check the ACE prefix is present */
	if ((label[0] == 'x') && (label[1] == 'n') &&
	    (label[2] == '-') && (label[3] == '-')) {
		return true;
	}

	return false;
}

/* This is the maximum length of a full DNS name */
#define FQDN_MAX 256

/* A "no action" action for FQDN_APPEND */
#define NO_ACTION (void)0

#define FQDN_APPEND(s, len, action)                                            \
	do {                                                                   \
		if ((FQDN_MAX - fqdn_len) <= len) {                            \
			/* Not enough room to append this element */           \
			action;                                                \
			return NSERROR_BAD_URL;                                \
		} else {                                                       \
			memcpy(fqdn_p, s, len);                                \
			fqdn_p += len;                                         \
			fqdn_len += len;                                       \
			action;                                                \
		}                                                              \
	} while (0)

/* exported interface documented in idna.h */
nserror
idna_encode(const char *host, size_t len, char **ace_host, size_t *ace_len)
{
	nserror error;
	int32_t *ucs4_host;
	size_t label_len, output_len, ucs4_len, fqdn_len = 0;
	char fqdn[FQDN_MAX];
	char *output, *fqdn_p = fqdn;

	label_len = idna__host_label_length(host, len);
	if (label_len == 0) {
		return NSERROR_BAD_URL;
	}

	while (label_len != 0) {
		if (idna__is_ldh(host, label_len) == false) {
			/* This string is IDN or invalid */

			/* Convert to Unicode */
			error = idna__utf8_to_ucs4(host, label_len,
						   &ucs4_host, &ucs4_len);
			if (error != NSERROR_OK) {
				return error;
			}

			/* Check this is valid for conversion */
			if (idna__is_valid(ucs4_host, ucs4_len) == false) {
				free(ucs4_host);
				return NSERROR_BAD_URL;
			}

			/* Convert to ACE */
			error = idna__ucs4_to_ace(ucs4_host, ucs4_len,
						&output, &output_len);
			free(ucs4_host);
			if (error != NSERROR_OK) {
				return error;
			}
			FQDN_APPEND(output, output_len, free(output));
		} else {
			/* This is already a DNS-valid ASCII string */
			if ((idna__is_ace(host, label_len) == true) &&
			    (idna__verify(host, label_len) == false)) {
				NSLOG(netsurf, INFO,
				      "Cannot verify ACE label %s", host);
				return NSERROR_BAD_URL;
			}
			FQDN_APPEND(host, label_len, NO_ACTION);
		}

		FQDN_APPEND(".", 1, NO_ACTION);

		host += label_len;
		if ((*host == '\0') || (*host == ':')) {
			break;
		}
		host++;
		len = len - label_len - 1;

		label_len = idna__host_label_length(host, len);
	}

	fqdn_p--;
	*fqdn_p = '\0';
	*ace_host = strdup(fqdn);
	*ace_len = fqdn_len - 1; /* last character is NULL */

	return NSERROR_OK;
}


/* exported interface documented in idna.h */
nserror
idna_decode(const char *ace_host, size_t ace_len, char **host, size_t *host_len)
{
	nserror error;
	int32_t *ucs4_host;
	size_t label_len, output_len, ucs4_len, fqdn_len = 0;
	char fqdn[FQDN_MAX];
	char *output, *fqdn_p = fqdn;

	label_len = idna__host_label_length(ace_host, ace_len);
	if (label_len == 0) {
		return NSERROR_BAD_URL;
	}

	while (label_len != 0) {
		if (idna__is_ace(ace_host, label_len) == true) {
			/* This string is DNS-valid and (probably) encoded */

			/* Decode to Unicode */
			error = idna__ace_to_ucs4(ace_host, label_len,
						  &ucs4_host, &ucs4_len);
			if (error != NSERROR_OK) {
				return error;
			}

			/* Convert to UTF-8 */
			error = idna__ucs4_to_utf8(ucs4_host, ucs4_len,
						   &output, &output_len);
			free(ucs4_host);
			if (error != NSERROR_OK) {
				return error;
			}
			FQDN_APPEND(output, output_len, free(output));
		} else {
			/* Not ACE */
			FQDN_APPEND(ace_host, label_len, NO_ACTION);
		}

		FQDN_APPEND(".", 1, NO_ACTION);

		ace_host += label_len;
		if ((*ace_host == '\0') || (*ace_host == ':')) {
			break;
		}
		ace_host++;
		ace_len = ace_len - label_len - 1;

		label_len = idna__host_label_length(ace_host, ace_len);
	}

	fqdn_p--;
	*fqdn_p = '\0';
	*host = strdup(fqdn);
	*host_len = fqdn_len - 1; /* last character is NULL */

	return NSERROR_OK;
}
