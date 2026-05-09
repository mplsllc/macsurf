/*
 * Copyright 2016 Michael Drake <tlsa@netsurf-browser.org>
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
 * \file utils/ascii.h
 * \brief Helpers for ASCII string handling.
 *
 * These helpers for string parsing will have the correct effect for parsing
 * ASCII text (as used by most web specs), regardless of system locale.
 */

#ifndef _NETSURF_UTILS_ASCII_H_
#define _NETSURF_UTILS_ASCII_H_

#include <errno.h>
#include <stdlib.h>
#include <limits.h>

/**
 * Test whether a character is a whitespace character.
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is whitespace, else false.
 */
static inline bool ascii_is_space(char c)
{
	return (c == ' '  || c == '\t' ||
	        c == '\n' || c == '\v' ||
	        c == '\f' || c == '\r');
}

/**
 * Test whether a character is lower-case alphabetical.
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is lower-case alphabetical, else false.
 */
static inline bool ascii_is_alpha_lower(char c)
{
	return (c >= 'a' && c <= 'z');
}

/**
 * Test whether a character is upper-case alphabetical.
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is upper-case alphabetical, else false.
 */
static inline bool ascii_is_alpha_upper(char c)
{
	return (c >= 'A' && c <= 'Z');
}

/**
 * Test whether a character is alphabetical (upper or lower case).
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is alphabetical, else false.
 */
static inline bool ascii_is_alpha(char c)
{
	return (ascii_is_alpha_lower(c) || ascii_is_alpha_upper(c));
}

/**
 * Test whether a character is a decimal digit.
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is a decimal digit, else false.
 */
static inline bool ascii_is_digit(char c)
{
	return (c >= '0' && c <= '9');
}

/**
 * Test whether a character is a positive/negative numerical sign.
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is a sign, else false.
 */
static inline bool ascii_is_sign(char c)
{
	return (c == '-' || c == '+');
}

/**
 * Test whether a character is alphanumerical (upper or lower case).
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is alphanumerical, else false.
 */
static inline bool ascii_is_alphanumerical(char c)
{
	return (ascii_is_alpha(c) || ascii_is_digit(c));
}

/**
 * Test whether a character is 'a' to 'f' (lowercase).
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is 'a' to 'f' (lowercase), else false.
 */
static inline bool ascii_is_af_lower(char c)
{
	return (c >= 'a' && c <= 'f');
}

/**
 * Test whether a character is hexadecimal (lower case).
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is hexadecimal, else false.
 */
static inline bool ascii_is_hex_lower(char c)
{
	return (ascii_is_digit(c) || ascii_is_af_lower(c));
}

/**
 * Test whether a character is 'A' to 'F' (uppercase).
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is 'A' to 'F' (uppercase), else false.
 */
static inline bool ascii_is_af_upper(char c)
{
	return (c >= 'A' && c <= 'F');
}

/**
 * Test whether a character is hexadecimal (upper case).
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is hexadecimal, else false.
 */
static inline bool ascii_is_hex_upper(char c)
{
	return (ascii_is_digit(c) || ascii_is_af_upper(c));
}

/**
 * Test whether a character is hexadecimal (upper or lower case).
 *
 * \param[in] c  Character to test.
 * \return true iff `c` is hexadecimal, else false.
 */
static inline bool ascii_is_hex(char c)
{
	return (ascii_is_digit(c) ||
			ascii_is_af_upper(c) ||
			ascii_is_af_lower(c));
}

/**
 * Convert a hexadecimal character to its value.
 *
 * \param[in] c  Character to convert.
 * \return value of character (0-15), or -256 if not a hexadecimal character.
 */
static inline int ascii_hex_to_value(char c)
{
	if (ascii_is_digit(c)) {
		return c - '0';
	} else if (ascii_is_af_lower(c)) {
		return c - 'a' + 10;
	} else if (ascii_is_af_upper(c)) {
		return c - 'A' + 10;
	}

	/* Invalid hex */
	return -256;
}

/**
 * Converts two hexadecimal characters to a single number
 *
 * \param[in] c1  most significant hex digit.
 * \param[in] c2  least significant hex digit.
 * \return the total value of the two digit hex number (0-255),
 *         or -ve if input not hex.
 */
static inline int ascii_hex_to_value_2_chars(char c1, char c2)
{
	return 16 * ascii_hex_to_value(c1) + ascii_hex_to_value(c2);
}

/**
 * Convert an upper case character to lower case.
 *
 * If the given character is not upper case alphabetical, it is returned
 * unchanged.
 *
 * \param[in] c  Character to convert.
 * \return lower case conversion of `c` else `c`.
 */
static inline char ascii_to_lower(char c)
{
	return (ascii_is_alpha_upper(c)) ? (c + 'a' - 'A') : c;
}

/**
 * Convert a lower case character to upper case.
 *
 * If the given character is not lower case alphabetical, it is returned
 * unchanged.
 *
 * \param[in] c  Character to convert.
 * \return upper case conversion of `c` else `c`.
 */
static inline char ascii_to_upper(char c)
{
	return (ascii_is_alpha_lower(c)) ? (c + 'A' - 'a') : c;
}

/**
 * Count consecutive lower case alphabetical characters in string.
 *
 * \param[in] str  String to count characters in.
 * \return number of consecutive lower case characters at start of `str`.
 */
static inline size_t ascii_count_alpha_lower(const char *str)
{
	size_t count = 0;
	while (ascii_is_alpha_lower(*(str++))) {
		count++;
	}
	return count;
}

/**
 * Count consecutive upper case alphabetical characters in string.
 *
 * \param[in] str  String to count characters in.
 * \return number of consecutive upper case characters at start of `str`.
 */
static inline size_t ascii_count_alpha_upper(const char *str)
{
	size_t count = 0;
	while (ascii_is_alpha_upper(*(str++))) {
		count++;
	}
	return count;
}

/**
 * Count consecutive alphabetical characters in string (upper or lower case).
 *
 * \param[in] str  String to count characters in.
 * \return number of consecutive alphabetical characters at start of `str`.
 */
static inline size_t ascii_count_alpha(const char *str)
{
	size_t count = 0;
	while (ascii_is_alpha(*(str++))) {
		count++;
	}
	return count;
}

/**
 * Count consecutive decial digit characters in string.
 *
 * \param[in] str  String to count characters in.
 * \return number of consecutive decimal digit characters at start of `str`.
 */
static inline size_t ascii_count_digit(const char *str)
{
	size_t count = 0;
	while (ascii_is_digit(*(str++))) {
		count++;
	}
	return count;
}

/**
 * Count consecutive characters either decimal digit or colon in string.
 *
 * \param[in] str  String to count characters in.
 * \return number of consecutive decimal or ':' characters at start of `str`.
 */
static inline size_t ascii_count_digit_or_colon(const char *str)
{
	size_t count = 0;
	while (ascii_is_digit(*str) || *str == ':') {
		count++;
		str++;
	}
	return count;
}

/**
 * Test for string equality (case insensitive).
 *
 * \param[in] s1  First string to compare.
 * \param[in] s2  Second string to compare.
 * \return true iff strings are equivalent, else false.
 */
static inline bool ascii_strings_equal_caseless(
		const char *s1, const char *s2)
{
	while (*s1 != '\0') {
		if (ascii_to_lower(*s1) != ascii_to_lower(*s2)) {
			break;
		}
		s1++;
		s2++;
	}
	return (ascii_to_lower(*s1) == ascii_to_lower(*s2));
}

/**
 * Test for string equality (case sensitive).
 *
 * \param[in] s1  First string to compare.
 * \param[in] s2  Second string to compare.
 * \return true iff strings are equal, else false.
 */
static inline bool ascii_strings_equal(
		const char *s1, const char *s2)
{
	while (*s1 != '\0') {
		if (*s1 != *s2) {
			break;
		}
		s1++;
		s2++;
	}
	return (*s1 == *s2);
}

/**
 * Count consecutive equal ascii characters (case insensitive).
 *
 * \param[in] s1  First string to compare.
 * \param[in] s2  Second string to compare.
 * \return number of equivalent characters.
 */
static inline size_t ascii_strings_count_equal_caseless(
		const char *s1, const char *s2)
{
	const char *s = s1;
	while (*s1 != '\0') {
		if (ascii_to_lower(*s1) != ascii_to_lower(*s2)) {
			break;
		}
		s1++;
		s2++;
	}
	return s1 - s;
}

/**
 * Count consecutive equal ascii characters (case sensitive).
 *
 * \param[in] s1  First string to compare.
 * \param[in] s2  Second string to compare.
 * \return number of equal characters.
 */
static inline size_t ascii_strings_count_equal(
		const char *s1, const char *s2)
{
	const char *s = s1;
	while (*s1 != '\0') {
		if (*s1 != *s2) {
			break;
		}
		s1++;
		s2++;
	}
	return s1 - s;
}

/**
 * Parse an int out of a string.
 *
 * \param[in]  str  String to parse integer out of.
 * \param[out] res  Returns parsed integer.
 * \return The number of characters consumed in `str`.
 *         Returning 0 indicates failure to parse an integer out of the string.
 */
static inline size_t ascii_string_to_int(const char *str, int *res)
{
	char *end = NULL;
	long long temp = strtoll(str, &end, 10);

	if (end == str || errno == ERANGE ||
			temp < INT_MIN || temp > INT_MAX) {
		return 0;
	}

	*res = temp;
	return end - str;
}

#endif
