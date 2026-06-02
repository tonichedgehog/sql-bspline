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

#pragma once

typedef struct bspline {
	float *t; /* Knot vector. */
	float *c; /* Coefficients. */
	int k;    /* Spline degree. */
	int nc;   /* Number of coeff. */
	int n;    /* Number of knots. */
	int idim; /* Coeff. dimension. */
} bspline;

/**
 * Create parametric spline from knot vector and coefficients. Memory layout for
 * coefficient array is column major, where each column is padded with zeroes,
 * (n+k+1)/2 elements at both ends. Ownership of knot and coefficient vectors is
 * transferred to the spline object.
 *
 * Parameters:
 *    - spline: Resulting spline, if successful.
 *    - t:      Knot vector.
 *    - n:      Number of kots.
 *    - c:      Coefficients.
 *    - nc:     Number of coefficients.
 *    - idim:   Coefficient dimension.
 *    - k:      BSpline degree.
 *
 * Return value:
 *    - Zero if successful. 
 */
int splcreate(bspline**, float* t, int n, float*c, int nc, int idim,
	int k);

void spldestroy(bspline**);

/**
 * Create parametric spline fitted to given parametric data points. See
 * dierckx/parcur.f for details.
 *
 * Parameters:
 *   - spline: Resulting spline, if successful.
 *   - x:      Data points, column major format.
 *   - idim:   Dimension of data points.
 *   - u:      Parametrisation vector, may be NULL.
 *   - un:     Length of parametrisation vector.
 *   - k:      Spline degree, use 1, 3 or 5.
 *   - s:      Smoothing factor.
 *
 * Return value:
 *   - The error code obtained from the parcur Fortran routine, rc <= 0 is
 *     considered success.
 */
int splprep(bspline **spline, float *x, int idim, int m,
	float *u, int nu, int k, float s);

/**
 * Evaluate parametric spline at given values for x. See dierckx/curev.f for
 * details.
 *
 * Parameters:
 *   - spline: Resulting spline, if successful.
 *   - x:      Evaluation points.
 *   - y:      Result vector.
 *   - m:      Number of points.
 */
int splev(bspline *spline, float *x, float *y, int m);
