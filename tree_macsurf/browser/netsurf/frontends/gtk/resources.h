/*
 * Copyright 2006 Daniel Silverstone <dsilvers@digital-scurf.org>
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
 * Interface to gtk builtin resource handling.
 *
 * This presents a unified interface to the rest of the codebase to
 * obtain resources. Note this is not anything to do with the resource
 * scheme handling beyond possibly providing the underlying data.
 *
 */

#ifndef NETSURF_GTK_RESOURCES_H
#define NETSURF_GTK_RESOURCES_H 1

/**
 * Initialise GTK resources handling.
 *
 * Must be called before attempting to retrieve any resources but
 * after logging is initialised as it logs.
 *
 * \param respath A string vector of paths to search for resources.
 * \return NSERROR_OK if all resources were located else an
 *         appropriate error code.
 */
nserror nsgtk_init_resources(char **respath);

/**
 * Creates a menu cursor from internal resources.
 *
 * \return Cursor object or NULL on error.
 */
GdkCursor *nsgtk_create_menu_cursor(void);

/**
 * Create gtk builder object for the named ui resource.
 *
 * Creating gtk builder objects from a named resource requires the
 * source xml resource to be parsed.
 *
 * This creates a gtk builder instance using an identifier name which
 * is mapped to the ui_resource table which must be initialised with
 * nsgtk_init_resources()
 *
 * \param resname The resource name to construct for
 * \param builder_out The builder result
 * \return NSERROR_OK and builder_out updated or appropriate error code
 */
nserror nsgtk_builder_new_from_resname(const char *resname, GtkBuilder **builder_out);


/**
 * Create gdk pixbuf for the named ui resource.
 *
 * This creates a pixbuf using an identifier name which is mapped to
 * the ui_resource table which must be initialised with
 * nsgtk_init_resources()
 *
 * \param resname The resource name to construct for
 * \param pixbuf_out The pixbuf result
 * \return NSERROR_OK and pixbuf_out updated or appropriate error code
 */
nserror nsgdk_pixbuf_new_from_resname(const char *resname, GdkPixbuf **pixbuf_out);

/**
 * Get direct pointer to resource data.
 *
 * For a named resource this obtains a direct acesss pointer to the
 * data and its length.
 *
 * The data is read only through this pointer and remains valid until
 * program exit.
 *
 * \param resname The resource name to obtain data for.
 * \param data_out The resulting data.
 * \param data_size_out The resulting data size.
 * \return NSERROR_OK and data_out updated or appropriate error code.
 */
nserror nsgtk_data_from_resname(const char *resname, const uint8_t **data_out, size_t *data_size_out);

/**
 * Get path to resource data.
 *
 * For a named resource this obtains the on-disc path to that resource.
 *
 * The path is read only and remains valid untill program exit.
 * \param resname The resource name to obtain path for.
 * \param path_out The resulting data.
 * \return NSERROR_OK and path_out updated or appropriate error code.
 */
nserror nsgtk_path_from_resname(const char *resname, const char **path_out);

#endif
