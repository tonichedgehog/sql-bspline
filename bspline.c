/*
 * Copyright 2026 Tonic Hedgehog
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sqlite3.h>
#include "dierckx.h"
#include "bspline.h"

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

int splcreate(bspline **spline, float* t, int n, float*c, int nc, int idim,
	int k)
{
	(*spline) = sqlite3_malloc(sizeof(bspline));
	(*spline)->t = t;
	(*spline)->c = c;
	(*spline)->n = n;
	(*spline)->nc = nc;
	(*spline)->idim = idim;
	(*spline)->k = k;

	return 0;
}

void spldestroy(bspline **spline)
{
	if (!*spline) return;
	if ((*spline)->t) sqlite3_free((*spline)->t);
	if ((*spline)->c) sqlite3_free((*spline)->c);
	sqlite3_free(*spline);
	*spline = NULL;
}

int
splprep(bspline **spline, float *x, int idim, int m,
	float *u, int nu, int k, float s)
{
	int iopt = 0; /* Smoothing spline. */
	int nest = MAX(m+k+1, 2*k+3);
	int mx = m * idim;
	int ipar = 0;

	// Allocate weighting vector
	float *w = sqlite3_malloc(m * sizeof(float));
	for (int i = 0; i < m; i++)
		w[i] = 1.0f;

	int n = 0;
	int nc = nest * idim;

	// Allocate memory for knots and coefficients
	float *t = sqlite3_malloc(nest * sizeof(float)); // Knots
	float *c = sqlite3_malloc(nc * sizeof(float)); // Coefficients
	float fp = 0.0f; // Weighted sum of squared residuals

	float ub = 0.0f;
	float ue = 1.0f;
	if (nu) {
		ub = u[0], ue = u[nu-1];
		ipar = 1;
	}
	else {
		u = calloc(m, sizeof(float));
	}

	// Allocate working memory required by curfit
	int lwrk = m * (k + 1) + nest * (6 + idim + 3 * k);
	float *wrk = sqlite3_malloc(lwrk * sizeof(float));

	int *iwrk = sqlite3_malloc(nest * sizeof(int));
	int ier = 0;

	parcur_(&iopt, &ipar, &idim, &m, u, &mx, x, w, &ub, &ue, &k, &s,
		&nest, &n, t, &nc, c, &fp, wrk, &lwrk, iwrk, &ier);

	(*spline) = sqlite3_malloc(sizeof(bspline));
	(*spline)->t = t;
	(*spline)->c = c;
	(*spline)->n = n;
	(*spline)->nc = n - k - 1;
	(*spline)->idim = idim;
	(*spline)->k = k;

	if (!nu) sqlite3_free(u);
	sqlite3_free(w);
	sqlite3_free(wrk);
	sqlite3_free(iwrk);

	/*
	 * Note: coefficient layout is column major, where each column is padded
	 *   with zeroes, (n+k+1)/2 elements at both ends. Hence, _knot_ count is
	 *   the column offset for _coefficient_ columns.
	 */
	return ier;
}

int splev(bspline *spline, float *x, float *y, int m)
{
	int ierr = 0;
	int mx = spline->nc * spline->idim;

	curev_(&spline->idim, spline->t, &spline->n, spline->c, &spline->nc, 
		&spline->k, x, &m, y, &mx, &ierr);
	return ierr;
}
