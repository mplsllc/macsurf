/* Native WebP content handler for the Classic MacSurf frontend. */
#ifndef MACOS9_WEBP_H
#define MACOS9_WEBP_H

#include "utils/ns_errors.h"

nserror macos9_webp_init(void);
void macos9_webp_purge_decoded_images(void);

#endif
