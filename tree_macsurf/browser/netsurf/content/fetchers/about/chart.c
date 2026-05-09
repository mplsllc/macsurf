/*
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf.
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
 * content generator for the about scheme chart page
 *
 * A chart consists of the figure area in which a chart a title and a
 * key are placed.
 *
 *
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "utils/config.h"
#include "netsurf/inttypes.h"
#include "utils/config.h"
#include "utils/utils.h"
#include "utils/errors.h"
#include "utils/nsurl.h"

#include "private.h"
#include "chart.h"

/** minimum figure dimension */
#define FIGURE_MIN_WIDTH 150
#define FIGURE_MIN_HEIGHT 100

enum chart_type {
		 CHART_TYPE_UNKNOWN,
		 CHART_TYPE_PIE,
};

/* type of chart key */
enum key_type {
	       CHART_KEY_UNSET,
	       CHART_KEY_NONE,
	       CHART_KEY_LEFT,
	       CHART_KEY_RIGHT,
	       CHART_KEY_TOP,
	       CHART_KEY_BOT,
	       CHART_KEY_END
};


struct chart_label {
	char *title; /* label title */
	unsigned int colour; /* colour */
};

struct chart_series {
	unsigned int len; /* number of values in the series */
	float *value; /* array of values */
};

#define MAX_SERIES 4

struct chart_data {
	unsigned int series_len;
	struct chart_series series[MAX_SERIES];

	unsigned int label_len; /* number of labels */
	struct chart_label *label;

};

/**
 * parameters for a chart figure
 */
struct chart_param {
	enum chart_type type;
	enum key_type key; /* what type of key to use */
	unsigned int width; /* width of figure */
	unsigned int height; /* height of figure */
	char *title; /* title */
	struct {
		unsigned int x;
		unsigned int y;
		unsigned int width;
		unsigned int height;
	} area; /* chart area within figure */
	struct chart_data data;
};

#define DEF_COLOUR_NUM 8
/** default colour series */
static unsigned int colour_series[DEF_COLOUR_NUM] =
	{
	 0x00ff00, /* green */
	 0x0000ff, /* blue */
	 0xff0000, /* red */
	 0xffff00, /* yellow */
	 0x00ffff, /* cyan */
	 0xff00ff, /* pink */
	 0x777777, /* grey */
	 0x000000, /* black */
	};


/* ensures there are labels present for every value */
static nserror ensure_label_count(struct chart_param *chart, unsigned int count)
{
	unsigned int lidx;
	int deltac;
	struct chart_label *nlabels;

	deltac = count - chart->data.label_len;
	if (deltac <= 0) {
		/* there are enough labels */
		return NSERROR_OK;
	}

	nlabels = realloc(chart->data.label,
			  count * sizeof(struct chart_label));
	if (nlabels == NULL) {
		return NSERROR_NOMEM;
	}
	chart->data.label = nlabels;

	for (lidx = chart->data.label_len; lidx < count; lidx++) {
		chart->data.label[lidx].title = calloc(1, 20);
		snprintf(chart->data.label[lidx].title, 19, "item %d", lidx + 1);
		chart->data.label[lidx].colour = colour_series[lidx % DEF_COLOUR_NUM];
	}

	chart->data.label_len = count;

	return NSERROR_OK;
}

/**
 * extract values for a series
 */
static nserror
extract_series_values(struct chart_param *chart,
		      unsigned int series_num,
		      const char *valstr,
		      size_t valstrlen)
{
	nserror res;
	unsigned int valcur;
	size_t valstart;/* value start in valstr */
	size_t vallen; /* value end in valstr */
	struct chart_series *series;

	series = chart->data.series + series_num;

	/* ensure we do not leak any data in this series */
	if (series->value != NULL) {
		free(series->value);
	}

	/* count how many values present */
	for (series->len = 1, valstart=0; valstart < valstrlen; valstart++) {
		if (valstr[valstart] == ',') {
			series->len++;
		}
	}

	/* allocate storage for values */
	series->value = calloc(series->len, sizeof(float));
	if (series->value == NULL) {
		return NSERROR_NOMEM;
	}

	/* extract values from query string */
	for (valcur = 0, vallen = 0, valstart = 0;
	     (valstart < valstrlen) && (valcur < series->len);
	     valstart += vallen, valcur++) {
		/* get query section length */
		vallen = 0;
		while (((valstart + vallen) < valstrlen) &&
		       (valstr[valstart + vallen] != ',')) {
			vallen++;
		}

		series->value[valcur] = strtof(valstr + valstart, NULL);
		vallen++; /* account for , separator */
	}

	res = ensure_label_count(chart, series->len);

	return res;
}


/**
 * extract values for next series
 */
static nserror
extract_next_series_values(struct chart_param *chart,
		      const char *valstr,
		      size_t valstrlen)
{
	nserror res;

	if (chart->data.series_len >= MAX_SERIES) {
		return NSERROR_NOSPACE;
	}

	res = extract_series_values(chart,
				    chart->data.series_len,
				    valstr,
				    valstrlen);
	if (res == NSERROR_OK) {
		chart->data.series_len++;
	}

	return res;
}


/**
 * extract label title
 */
static nserror
extract_series_labels(struct chart_param *chart,
		      const char *valstr,
		      size_t valstrlen)
{
	nserror res;
	unsigned int valcount; /* count of values in valstr */
	unsigned int valcur;
	size_t valstart;/* value start in valstr */
	size_t vallen; /* value end in valstr */

	for (valcount = 1, valstart=0; valstart < valstrlen; valstart++) {
		if (valstr[valstart] == ',') {
			valcount++;
		}
	}

	res = ensure_label_count(chart, valcount);
	if (res != NSERROR_OK) {
		return res;
	}


	for (valcur = 0, vallen = 0, valstart = 0;
	     (valstart < valstrlen) && (valcur < chart->data.label_len);
	     valstart += vallen, valcur++) {
		/* get query section length */
		vallen = 0;
		while (((valstart + vallen) < valstrlen) &&
		       (valstr[valstart + vallen] != ',')) {
			vallen++;
		}

		chart->data.label[valcur].title = strndup(valstr + valstart, vallen);
		vallen++; /* account for , separator */
	}
	return NSERROR_OK;
}


/**
 * extract labels colour
 */
static nserror
extract_series_colours(struct chart_param *chart,
		      const char *valstr,
		      size_t valstrlen)
{
	return NSERROR_OK;
}

/**
 * process a part of a query
 */
static nserror
process_query_section(const char *str, size_t len, struct chart_param *chart)
{
	nserror res = NSERROR_OK;

	if ((len > 6) &&
	    (strncmp(str, "width=", 6) == 0)) {
		/* figure width */
		chart->width = strtoul(str + 6, NULL, 10);
	} else if ((len > 7) &&
	    (strncmp(str, "height=", 7) == 0)) {
		/* figure height */
		chart->height = strtoul(str + 7, NULL, 10);
	} else if ((len > 8) &&
	    (strncmp(str, "cawidth=", 8) == 0)) {
		/* chart area width */
		chart->area.width = strtoul(str + 8, NULL, 10);
	} else if ((len > 9) &&
	    (strncmp(str, "caheight=", 9) == 0)) {
		/* chart area height */
		chart->area.height = strtoul(str + 9, NULL, 10);
	} else if ((len > 4) &&
	    (strncmp(str, "key=", 4) == 0)) {
		/* figure has key */
		chart->key = strtoul(str + 4, NULL, 10);
	} else if ((len > 6) &&
	    (strncmp(str, "title=", 6) == 0)) {
		chart->title = strndup(str + 6, len - 6);
	} else if ((len > 5) &&
	    (strncmp(str, "type=", 5) == 0)) {
		if (strncmp(str + 5, "pie", len - 5) == 0) {
			chart->type = CHART_TYPE_PIE;
		} else {
			chart->type = CHART_TYPE_UNKNOWN;
		}
	} else if ((len > 7) &&
	    (strncmp(str, "values=", 7) == 0)) {
		res = extract_next_series_values(chart, str + 7, len - 7);
	} else if ((len > 7) &&
	    (strncmp(str, "labels=", 7) == 0)) {
		res = extract_series_labels(chart, str + 7, len - 7);
	} else if ((len > 8) &&
	    (strncmp(str, "colours=", 8) == 0)) {
		res = extract_series_colours(chart, str + 8, len - 8);
	}

	return res;
}



static nserror
chart_from_query(struct nsurl *url, struct chart_param *chart)
{
	nserror res;
	char *querystr;
	size_t querylen;
	size_t kvstart;/* key value start */
	size_t kvlen; /* key value end */

	res = nsurl_get(url, NSURL_QUERY, &querystr, &querylen);
	if (res != NSERROR_OK) {
		return res;
	}

	for (kvlen = 0, kvstart = 0; kvstart < querylen; kvstart += kvlen) {
		/* get query section length */
		kvlen = 0;
		while (((kvstart + kvlen) < querylen) &&
		       (querystr[kvstart + kvlen] != '&')) {
			kvlen++;
		}

		res = process_query_section(querystr + kvstart, kvlen, chart);
		if (res != NSERROR_OK) {
			break;
		}
		kvlen++; /* account for & separator */
	}
	free(querystr);

	/* sanity check dimensions */
	if (chart->width < FIGURE_MIN_WIDTH) {
		/* bad width - check height */
		if (chart->height < FIGURE_MIN_HEIGHT) {
			/* both bad set to defaults */
			chart->width = FIGURE_MIN_WIDTH;
			chart->height = FIGURE_MIN_HEIGHT;
		} else {
			/* base width on valid height */
			chart->width = (chart->height * 3) / 2;
		}
	} else {
		/* good width check height */
		if (chart->height < FIGURE_MIN_HEIGHT) {
			/* base height on valid width */
			chart->height = (chart->width * 2) / 3;
		}
	}

	/* ensure legend type correct */
	if ((chart->key == CHART_KEY_UNSET) ||
	    (chart->key >= CHART_KEY_END )) {
		/* default to putting key on right */
		chart->key = CHART_KEY_RIGHT;
	}

	return NSERROR_OK;
}


static nserror
output_pie_legend(struct fetch_about_context *ctx, struct chart_param *chart)
{
	nserror res;
	unsigned int lblidx;
	unsigned int legend_width;
	unsigned int legend_height;
	unsigned int vertical_spacing;

	switch (chart->key) {

	case CHART_KEY_NONE:
		break;
	case CHART_KEY_RIGHT:
		legend_width = chart->width - chart->area.width - chart->area.x;
		legend_width -= 10; /* margin */
		legend_height = chart->height;
		vertical_spacing = legend_height / (chart->data.label_len + 1);

		for(lblidx = 0; lblidx < chart->data.label_len ; lblidx++) {
			res = fetch_about_ssenddataf(ctx,
				"<rect  x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"#%06x\" />",
				chart->width - legend_width,
				(vertical_spacing * lblidx) + (vertical_spacing/2),
				vertical_spacing * 2 / 3,
				vertical_spacing * 2 / 3,
				chart->data.label[lblidx].colour);
			if (res != NSERROR_OK) {
				return res;
			}
			res = fetch_about_ssenddataf(ctx,
				"<text x=\"%d\" y=\"%d\" fill=\"#%06x\" >%s</text>",
				chart->width - legend_width + vertical_spacing,
				vertical_spacing * (lblidx+1),
			     chart->data.label[lblidx].colour,
				chart->data.label[lblidx].title);
			if (res != NSERROR_OK) {
				return res;
			}
		}
		break;
	default:
		break;
	}

	return NSERROR_OK;
}

static float
compute_series_total(struct chart_param *chart, unsigned int series)
{
	float total;
	unsigned int curdata;

	for (total = 0, curdata = 0;
	     curdata < chart->data.series[series].len;
	     curdata++) {
		total += chart->data.series[series].value[curdata];
	}
	return total;
}

/**
 * render the data as a pie chart svg
 */
static bool
pie_chart(struct fetch_about_context *ctx, struct chart_param *chart)
{
	nserror res;
	float ra; /* pie a radius */
	float rb; /* pie b radius */
	float series_total;
	unsigned int curdata; /* current data point index */
	float last_x, last_y;
	float end_x, end_y;
	float start;
	float extent;
	bool large;
	float circle_centre_x, circle_centre_y;

	/* ensure there is data to render */
	if ((chart->data.series_len < 1) || (chart->data.series[0].len < 2)) {
		return NSERROR_BAD_PARAMETER;
	}

	/* get the first series total value */
	series_total = compute_series_total(chart, 0);
	if (series_total == 0) {
		/* dividing by zero is embarasing */
		return NSERROR_BAD_PARAMETER;
	}

	/*
	 * need to ensure the chart area is setup correctly
	 *
	 * this is left to each chart type as different charts
	 *  have differnt requirements
	 */
	if ((chart->area.width == 0) || (chart->area.height == 0)) {
		/*
		 * pie chart defaults to square of smaller of figure
		 *  width and height
		 */
		if (chart->width > chart->height) {
			chart->area.width = chart->area.height = (chart->height - chart->area.x);
		} else {
			chart->area.width = chart->area.height = (chart->width - chart->area.y);
		}
	}

	/* content is going to return ok */
	fetch_about_set_http_code(ctx, 200);

	/* content type */
	if (fetch_about_send_header(ctx,
			"Content-Type: image/svg; charset=utf-8")) {
		goto aborted;
	}

	/* get the pie charts elipse radii */
	ra = chart->area.width / 2;
	rb = chart->area.height / 2;

	/* get the offset to the circle centre */
	circle_centre_x = chart->area.x + ra;
	circle_centre_y = chart->area.y + rb;


	/* svg header */
	res = fetch_about_ssenddataf(ctx,
			"<svg width=\"%u\" height=\"%u\" "
			"xmlns=\"http://www.w3.org/2000/svg\">\n",
			chart->width, chart->height);
	if (res != NSERROR_OK) {
		goto aborted;
	}

	/* generate the legend */
	res = output_pie_legend(ctx, chart);
	if (res != NSERROR_OK) {
		goto aborted;
	}

	/* plot the arcs */
	start = -M_PI_2;
	last_x = (ra * cos(start));
	last_y = (rb * sin(start));

	/* iterate over each data point creating a slice o pie */
	for (curdata=0; curdata < chart->data.series[0].len; curdata++) {
		extent = ((chart->data.series[0].value[curdata] / series_total) * 2 * M_PI);
		end_x = (ra * cos(start + extent));
		end_y = (rb * sin(start + extent));

		if (extent > M_PI) {
			large = true;
		} else {
			large = false;
		}

		res = fetch_about_ssenddataf(
			ctx,
			"<path d=\"M %g %g\n"
			"A %g %g 0 %d 1 %g %g\n"
			"L %g %g Z\" fill=\"#%06x\" />\n",
			circle_centre_x + last_x,
			circle_centre_y + last_y,
			ra, rb, large?1:0,
			circle_centre_x + end_x,
			circle_centre_y + end_y,
			circle_centre_x,
			circle_centre_y,
			chart->data.label[curdata].colour);
		if (res != NSERROR_OK) {
			goto aborted;
		}
		last_x = end_x;
		last_y = end_y;
		start +=extent;
	}

	res = fetch_about_ssenddataf(ctx, "</svg>\n");
	if (res != NSERROR_OK) {
		goto aborted;
	}

	fetch_about_send_finished(ctx);

	return true;

 aborted:

	return false;

}

/**
 * Handler to generate about scheme chart page.
 *
 * generates an svg chart
 *
 * \param ctx The fetcher context.
 * \return true if handled false if aborted.
 */
bool fetch_about_chart_handler(struct fetch_about_context *ctx)
{
	nserror res;
	struct chart_param chart;
	memset(&chart, 0, sizeof(struct chart_param));

	res = chart_from_query(fetch_about_get_url(ctx), &chart);
	if (res != NSERROR_OK) {
		goto aborted;
	}

	switch (chart.type) {
	case CHART_TYPE_PIE:
		return pie_chart(ctx, &chart);


	default:
		break;
	}

aborted:

	return false;

}
