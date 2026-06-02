#pragma once

int parcur_(int *iopt, int *ipar, int *idim, int *m,
	float *u, int *mx, float *x, float *w, float *ub,
	float *ue, int *k, float *s, int *nest, int *n, float *t,
	int *nc, float *c, float *fp, float *wrk,
	int *lwrk, int *iwrk, int *ier);

int curev_(int *idim, float *t, int *n, float *c, int *nc,
	int *k, float *u, int *m, float *x, int *mx, int *ier);

int cualde_(int *idim, float *t, int *n, float *c, 
	int *nc, int *k1, float *u, float *d, int *nd, int *ier);
