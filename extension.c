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

#include <sqlite3.h>
#include <sqlite3ext.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "bspline.h"

#ifdef SQLITE_OMIT_VIRTUALTABLE
#error "this extension require SQLite virtual table support"
#endif

/**
 * -----------------------------------------------------------------------------
 * FITPACK SPLINE - SQLite extension for working with parametric BSplines.
 * -----------------------------------------------------------------------------
 *
 * See readme.md for details.
 */

SQLITE_EXTENSION_INIT1

#define FPK_ARG_MNAME 0   /* Module name arg. */
#define FPK_ARG_TABLE 2   /* Vtable name arg. */
#define FPK_ARG_SPDEG 3   /* Bspline degree arg. */
#define FPK_ARG_TSTEP 4   /* Evaluation delta arg. */
#define FPK_ARG_SFACT 5   /* Smoothing factor. */
#define FPK_ARG_INPUT 6   /* Input table arg. */
#define FPK_ARG_T_COL 7   /* Knots column arg. */
#define FPK_ARG_C_COL 8   /* 1:st coeff. column arg. */

typedef enum fpk_init_flags {
	FPK_INIT_CONNECT = 1, /* Connect to existing table. */
	FPK_INIT_CREATE = 2,  /* Create new table. */
	FPK_INIT_PREDEF = 4,  /* Create from knots and coef. */
	FPK_INIT_SMOOTH = 8,  /* Smoothed spline from samples. */
} fpk_init_flags;

typedef enum fpk_eval_mode {
	FPK_EVAL_EMPTY,       /* Query produces no rows. */
	FPK_EVAL_POINT,       /* Query single point on knots vector. */
	FPK_EVAL_RANGE,       /* Query an interval on knots vector. */
} fpk_eval_mode;

typedef enum fpk_eval_range {
	FPK_RANGE_NONE,       /* No lower or upper bound. */
	FPK_RANGE_LOWER,      /* Lower bound given. */
	FPK_RANGE_UPPER,      /* Upper bound given. */
} fpk_eval_range;

typedef struct fpk_arg {
	const char *key;      /* Argument name. */
	double val;           /* Argument value. */
} fpk_arg;

typedef struct fpk_cursor {
	sqlite3_vtab_cursor p;
	fpk_eval_mode mode;   /* Evaluation mode. */
	fpk_eval_range range; /* Range constraints. */
	float lower;          /* Lower bound. */
	float upper;          /* Upper bound. */
	float *pX;            /* X-values to be evaluated. */
	float *pRow;          /* Evaluated row. */
	int iCurX;            /* Current value. */ 
	int iNumX;            /* Nr of X-values to evaluate. */
	int bEval;            /* Current X-value is evaluated. */
} fpk_cursor;

typedef struct fpk_vtab {
	sqlite3_vtab p;
	sqlite3 *db;
	bspline *pBSpline;    /* BSpline knots and coeff. */
	char *zMetaTab;       /* Metadata table name. */
	char *zDataTab;       /* Spline data table name. */
	double sFact;         /* Smoothing factor. */
	double tMin, tMax;    /* Knot vector limits. */
	double tStep;         /* Evaluation step. */
	int iCreat;           /* Create/connect mode. */
	int iNRows;           /* Virtual row count. */
	int iCdim;            /* Coeff dimension. */
	int iOrder;           /* BSpline order. */
} fpk_vtab;

// ----------------------------------------------------------------------------
static int _fpk_parse_args(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	char *endp = NULL;
	char *zSrcTab = NULL;
	char **pzColNames = NULL;
	int iNumCols = 0;
	sqlite3_stmt *stmt = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);

	pFpkVtab->zMetaTab = sqlite3_mprintf("%s_meta", argv[FPK_ARG_TABLE]);
	pFpkVtab->zDataTab = sqlite3_mprintf("%s_data", argv[FPK_ARG_TABLE]);

	/* Value dimension. */
	if ((pFpkVtab->iCdim = argc - FPK_ARG_C_COL) < 1) {
		*pzErr = sqlite3_mprintf("usage: %s(k, dt, s, data, t, c1...cn))",
			argv[FPK_ARG_MNAME]);
		rc = SQLITE_ERROR;
		goto onError_parse_args;
	}

	/* BSpline order. */
	pFpkVtab->iOrder = strtol(argv[FPK_ARG_SPDEG], &endp, 10) + 1;
	if (pFpkVtab->iOrder < 2 || (endp && *endp)) {
		*pzErr = sqlite3_mprintf("%s: invalid degree '%s'",
			argv[FPK_ARG_MNAME],
			argv[FPK_ARG_SPDEG]);
		rc = SQLITE_ERROR;
		goto onError_parse_args;
	}

	/* Evaluation step. */
	pFpkVtab->tStep = strtod(argv[FPK_ARG_TSTEP], &endp);
	if (pFpkVtab->tStep <= 0 || (endp && *endp)) {
		*pzErr = sqlite3_mprintf("%s: invalid step '%s'",
			argv[FPK_ARG_MNAME],
			argv[FPK_ARG_TSTEP]);
		rc = SQLITE_ERROR;
		goto onError_parse_args;
	}

	/* Not creating a new vtable? Then done here. */
	if (!(pFpkVtab->iCreat & FPK_INIT_CREATE))
		goto onError_parse_args;

	/* Smoothing factor. */
	if (!sqlite3_stricmp(argv[FPK_ARG_SFACT], "NULL")) {
		/* Use predefined knots and coeffs. */
		pFpkVtab->iCreat |= FPK_INIT_PREDEF;
	}
	else {
		/* Approximate samples with spline. */
		pFpkVtab->iCreat |= FPK_INIT_SMOOTH;
		pFpkVtab->sFact = strtod(argv[FPK_ARG_SFACT], &endp);
		if (pFpkVtab->sFact < 0 || (endp && *endp)) {
			*pzErr = sqlite3_mprintf("%s: invalid smoothing factor '%s'",
				argv[FPK_ARG_MNAME],
				argv[FPK_ARG_SFACT]);
			rc = SQLITE_ERROR;
			goto onError_parse_args;
		}
	}

	/* No predef table means done here. */
	if (!sqlite3_stricmp(argv[FPK_ARG_INPUT], "NULL"))
		goto onError_parse_args;

	/* Check predef table exists. */
	sqlite3_str_appendf(sql, "PRAGMA table_info(%s)",
		argv[FPK_ARG_INPUT]);
	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, NULL))) {
		*pzErr = sqlite3_mprintf("%s: table '%s' does not exists",
			argv[FPK_ARG_MNAME],
			argv[FPK_ARG_INPUT]);
		rc = SQLITE_ERROR;
		goto onError_parse_args;
	}

	/* Extract column names from predef table. */
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {

		pzColNames = sqlite3_realloc(pzColNames, (iNumCols + 1) * sizeof(char*));
		pzColNames[iNumCols] = sqlite3_mprintf((char*)sqlite3_column_text(stmt, 1));
		iNumCols++;
	}

	if (rc != SQLITE_DONE)
		goto onError_parse_args;

	/* Compare given predef columns against those found. */
	for (int i = FPK_ARG_T_COL, bFound = 0; i < argc; i++, bFound = 0) {
		for (int j = 0; j < iNumCols; j++) {
			if (!strcmp(argv[i], pzColNames[j])) {
				bFound = 1;
				break;
			}
		}
		if (!bFound) {
			*pzErr = sqlite3_mprintf("%s: table '%s' has no column '%s'",
				argv[FPK_ARG_MNAME],
				argv[FPK_ARG_INPUT],
				argv[i]);
			rc = SQLITE_ERROR;
			goto onError_parse_args;
		}
	}
	rc = SQLITE_OK;

onError_parse_args:
	sqlite3_finalize(stmt);
	sqlite3_free(sqlite3_str_finish(sql));
	if (zSrcTab) sqlite3_free(zSrcTab);
	if (pzColNames) {
		for (int i = 0; i < iNumCols; i++)
			sqlite3_free(pzColNames[i]);
		sqlite3_free(pzColNames);
	}
	return rc;
}

// ----------------------------------------------------------------------------
static int _fpk_create_tables(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	sqlite3_str *sql = sqlite3_str_new(db);

	/* Create backing table for knots and coefficients. */
	sqlite3_str_appendf(sql, "CREATE TABLE %s(%s REAL",
		pFpkVtab->zDataTab,
		argv[FPK_ARG_T_COL]);
	for (int i = FPK_ARG_C_COL; i < argc; i++)
		sqlite3_str_appendf(sql, ", %s REAL", argv[i]);
	sqlite3_str_appendchar(sql, 1, ')');

	if ((rc = sqlite3_exec(db, sqlite3_str_value(sql), NULL, NULL, pzErr)))
		goto onError_create_tables;

	/* Create backing table for metadata. */
	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "CREATE TABLE %s(key TEXT, val REAL)",
		pFpkVtab->zMetaTab);

	if ((rc = sqlite3_exec(db, sqlite3_str_value(sql), NULL, NULL, pzErr)))
		goto onError_create_tables;

onError_create_tables:
	sqlite3_free(sqlite3_str_finish(sql));
	return rc;
}	

// ----------------------------------------------------------------------------
static int _fpk_save_meta(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	assert(!*pzErr);
	int rc = SQLITE_OK;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_stmt *stmt = NULL;
	fpk_arg meta[3] = {
		{"k", pFpkVtab->iOrder},
		{"d", pFpkVtab->tStep},
		{"s", pFpkVtab->sFact},
	};

	/* Save spline metadata. */
	sqlite3_str_appendf(sql, "INSERT INTO %s VALUES (?1, ?2)",
		pFpkVtab->zMetaTab);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, NULL)))
		goto onError_save_meta;

	for (int i = 0; i < sizeof(meta) / sizeof(fpk_arg); i++) {

		if ((rc = sqlite3_bind_text(stmt, 1, meta[i].key, -1, SQLITE_STATIC)))
			goto onError_save_meta;

		if ((rc = sqlite3_bind_double(stmt, 2, meta[i].val)))
			goto onError_save_meta;

		if ((rc = sqlite3_step(stmt)) != SQLITE_DONE)
			goto onError_save_meta;

		if ((rc = sqlite3_reset(stmt)))
			goto onError_save_meta;
	}


onError_save_meta:
	sqlite3_free(sqlite3_str_finish(sql));
	sqlite3_finalize(stmt);

	return rc;
}

// ----------------------------------------------------------------------------
static int _fpk_save_predef(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	assert(!*pzErr);
	int iRow = 0;
	int rc = SQLITE_OK;
	const char *tail = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_str *cols = sqlite3_str_new(db);
	sqlite3_str *vals = sqlite3_str_new(db);
	sqlite3_stmt *stmt1 = NULL;
	sqlite3_stmt *stmt2 = NULL;

	sqlite3_str_appendf(cols, "%s", argv[FPK_ARG_T_COL]);
	sqlite3_str_appendf(vals, "%s", "?");

	for (int i = FPK_ARG_C_COL; i < argc; i++) {
		sqlite3_str_appendf(cols, ",%s", argv[i]);
		sqlite3_str_appendf(vals, ",?");
	}

	/* Fetch given kots and coefficients. */
	sqlite3_str_appendf(sql, "SELECT %s FROM %s",
		sqlite3_str_value(cols),
		argv[FPK_ARG_INPUT]);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt1, &tail)))
		goto onError_save_predef;

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "INSERT INTO %s (%s) VALUES (%s)",
		pFpkVtab->zDataTab,
		sqlite3_str_value(cols),
		sqlite3_str_value(vals));

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt2, NULL)))
		goto onError_save_predef;

	/* Store spline data in private backing table. */
	while ((rc = sqlite3_step(stmt1)) == SQLITE_ROW) {

		for (int i = 0; i <= pFpkVtab->iCdim; i++)
			sqlite3_bind_value(stmt2, i+1, sqlite3_column_value(stmt1, i));

		if ((rc = sqlite3_step(stmt2)) != SQLITE_DONE)
			goto onError_save_predef;

		if ((rc = sqlite3_reset(stmt2)) != SQLITE_OK)
			goto onError_save_predef;
		iRow++;
	}
	assert(rc == SQLITE_DONE);
	rc = SQLITE_OK;

onError_save_predef:
	sqlite3_free(sqlite3_str_finish(sql));
	sqlite3_free(sqlite3_str_finish(cols));
	sqlite3_free(sqlite3_str_finish(vals));

	sqlite3_finalize(stmt1);
	sqlite3_finalize(stmt2);

	return rc;
}

// ----------------------------------------------------------------------------
static int _fpk_declare_vtab(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	sqlite3_str *schema = sqlite3_str_new(db);

	/* Build pseudo-create statement. */
	sqlite3_str_appendall(schema, "CREATE TABLE x(");
	for (int i = FPK_ARG_T_COL; i < argc; i++) {
		if (i > FPK_ARG_T_COL)
			sqlite3_str_appendchar(schema, 1, ',');
		sqlite3_str_appendall(schema, argv[i]);
		sqlite3_str_appendall(schema, " REAL");
		if (i == FPK_ARG_T_COL)
			sqlite3_str_appendall(schema, " PRIMARY KEY");
	}
	sqlite3_str_appendall(schema, ") WITHOUT ROWID");

	/* Declare vtable schema. */
	if ((rc = sqlite3_declare_vtab(db, sqlite3_str_value(schema))))
		goto onError_decl_vtab;

onError_decl_vtab:
	sqlite3_free(sqlite3_str_finish(schema));
	if (pzErr && rc)
		sqlite3_free(*pzErr);
	if (rc)
		*pzErr = sqlite3_mprintf("%s: (%s): %s",
			argv[FPK_ARG_MNAME],
			__func__,
			sqlite3_errstr(rc));
	return rc;
}

// ----------------------------------------------------------------------------
static void _fpk_compute_nrow(
	fpk_vtab *pFpkVtab)
{
	pFpkVtab->tMin = pFpkVtab->pBSpline->t[0];
	pFpkVtab->tMax = pFpkVtab->pBSpline->t[pFpkVtab->pBSpline->n - 1];

	/* Sampled number of rows. */
	pFpkVtab->iNRows = (int)ceil((pFpkVtab->tMax - pFpkVtab->tMin) / pFpkVtab->tStep) + 1;
	pFpkVtab->tStep = (pFpkVtab->tMax - pFpkVtab->tMin) / (double)(pFpkVtab->iNRows - 1);
}

// ----------------------------------------------------------------------------
static int _fpk_approx_bspline(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	int iRow = 0, iNx = 0;
	float *pU = NULL, *pX = NULL;
	const char *tail = NULL;
	int k = pFpkVtab->iOrder - 1;
	int iXdim = pFpkVtab->iCdim;
	float s = pFpkVtab->sFact;
	sqlite3_stmt *stmt = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_str *cols = sqlite3_str_new(db);
	sqlite3_str *vals = sqlite3_str_new(db);

	/* Query for size of input. */
	sqlite3_str_appendf(sql, "SELECT COUNT(*) FROM %s",
		argv[FPK_ARG_INPUT]);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_approx_bspline;

	if ((rc = sqlite3_step(stmt)) != SQLITE_ROW)
		goto onError_approx_bspline;

	rc = SQLITE_OK;
	if (!(iNx = sqlite3_column_int(stmt, 0))) {
		assert(!*pzErr);
		*pzErr = sqlite3_mprintf("%s: to few samples: %d",
			argv[FPK_ARG_MNAME],
			iNx);
		rc = SQLITE_ERROR;
		goto onError_approx_bspline;
	}

	/* Copy u,x from table to memory. */
	pU = sqlite3_malloc(iNx * sizeof(float));
	pX = sqlite3_malloc(iNx * sizeof(float) * iXdim);

	for (int i = FPK_ARG_T_COL; i < argc; i++) {
		if (i > FPK_ARG_T_COL) {
			sqlite3_str_appendchar(cols, 1, ',');
			sqlite3_str_appendchar(vals, 1, ',');
		}
		sqlite3_str_appendf(cols, "%s", argv[i]);
		sqlite3_str_appendf(vals, "?");
	}

	sqlite3_str_reset(sql);
	sqlite3_finalize(stmt);
	stmt = NULL;

	sqlite3_str_appendf(sql, "SELECT %s FROM %s",
		sqlite3_str_value(cols),
		argv[FPK_ARG_INPUT]);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_approx_bspline;

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {

		pU[iRow] = (float)sqlite3_column_double(stmt, 0);
		for (int iCol = 0; iCol < pFpkVtab->iCdim; iCol++) {
			pX[iRow * iXdim + iCol] = (float)sqlite3_column_double(
				stmt, iCol + 1);
		}
		iRow++;
	}
	assert(iRow == iNx);

	/* Approximate data points with BSpline. */
	if ((rc = splprep(&pFpkVtab->pBSpline, pX, iXdim, iNx, pU, iNx, k, s)) > 0) {
		switch (rc) {
		case 1:
		case 2:
		case 3:
			*pzErr = sqlite3_mprintf("%s: smoothing spline failed with error %d, factor too small?",
				argv[FPK_ARG_MNAME], rc);
			break;
		case 10:
			*pzErr = sqlite3_mprintf("%s: smoothing spline consistency check failed with error %d",
				argv[FPK_ARG_MNAME], rc);
			break;
		}
		rc = SQLITE_ERROR;
		goto onError_approx_bspline;
	}
	rc = SQLITE_OK;
	_fpk_compute_nrow(pFpkVtab);

onError_approx_bspline:
	sqlite3_free(sqlite3_str_finish(sql));
	sqlite3_free(sqlite3_str_finish(cols));
	sqlite3_free(sqlite3_str_finish(vals));
	sqlite3_finalize(stmt);
	if (pX) sqlite3_free(pX);
	if (pU) sqlite3_free(pU);
	return rc;
}

// ----------------------------------------------------------------------------
static int _fpk_save_bspline(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	int iRow = 0, iNx = 0;
	const char *tail = NULL;
	int offset = pFpkVtab->iOrder / 2;
	sqlite3_stmt *stmt = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_str *cols = sqlite3_str_new(db);
	sqlite3_str *vals = sqlite3_str_new(db);

	for (int i = FPK_ARG_T_COL; i < argc; i++) {
		if (i > FPK_ARG_T_COL) {
			sqlite3_str_appendchar(cols, 1, ',');
			sqlite3_str_appendchar(vals, 1, ',');
		}
		sqlite3_str_appendf(cols, "%s", argv[i]);
		sqlite3_str_appendf(vals, "?");
	}


	/* Save spline to backing table. */
	sqlite3_str_appendf(sql, "INSERT INTO %s (%s) VALUES (%s)",
		pFpkVtab->zDataTab,
		sqlite3_str_value(cols),
		sqlite3_str_value(vals));

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_save_bspline;

	for (iRow = 0; iRow < pFpkVtab->pBSpline->n; iRow++) {

		sqlite3_bind_double(stmt, 1, pFpkVtab->pBSpline->t[iRow]);

		for (int iCol = 0; iCol < pFpkVtab->iCdim; iCol++) {

			float *c = pFpkVtab->pBSpline->c;
			int nc = pFpkVtab->pBSpline->nc;
			if (iRow >= offset && iRow < nc + offset) {
				sqlite3_bind_double(stmt, iCol + 2, c[iCol * iNx + iRow - offset]);
			}
		}
		if ((rc = sqlite3_step(stmt)) != SQLITE_DONE)
			goto onError_save_bspline;

		if ((rc = sqlite3_reset(stmt)) != SQLITE_OK)
			goto onError_save_bspline;

		if ((rc = sqlite3_clear_bindings(stmt)) != SQLITE_OK)
			goto onError_save_bspline;
	}
	rc = SQLITE_OK;

onError_save_bspline:
	sqlite3_free(sqlite3_str_finish(sql));
	sqlite3_free(sqlite3_str_finish(cols));
	sqlite3_free(sqlite3_str_finish(vals));
	sqlite3_finalize(stmt);
	return rc;
}

// ----------------------------------------------------------------------------
static int _fpk_build_bspline(
	sqlite3 *db,
	fpk_vtab *pFpkVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int iNumCoeff, iNumKnots;
	int rc = SQLITE_OK;
	int iRow = 0;
	int offset = pFpkVtab->iOrder / 2;
	float *t = NULL, *c = NULL;
	const char *tail = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_stmt *stmt = NULL;

	sqlite3_str_appendf(sql, "SELECT COUNT(*) from %s",
		pFpkVtab->zDataTab);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_build_bspline;

	if ((rc = sqlite3_step(stmt)) != SQLITE_ROW)
		goto onError_build_bspline;

	rc = SQLITE_OK;
	if (!(iNumKnots = sqlite3_column_int(stmt, 0))) {
		assert(!*pzErr);
		*pzErr = sqlite3_mprintf("%s: to few knots: %d",
			argv[FPK_ARG_MNAME],
			iNumKnots);
		rc = SQLITE_ERROR;
		goto onError_build_bspline;
	}

	if ((iNumCoeff = iNumKnots - pFpkVtab->iOrder) < 1) {
		*pzErr = sqlite3_mprintf("%s: to few coefficients (%d)",
			argv[FPK_ARG_MNAME],
			iNumCoeff);
		rc = SQLITE_ERROR;
		goto onError_build_bspline;
	};
	t = sqlite3_malloc(iNumKnots * sizeof(float));
	c = sqlite3_malloc(iNumKnots * sizeof(float) * pFpkVtab->iCdim);

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "SELECT * from %s",
		pFpkVtab->zDataTab);

	if ((rc = sqlite3_finalize(stmt)))
		goto onError_build_bspline;

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_build_bspline;

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {

		t[iRow] = sqlite3_column_double(stmt, 0);

		for (int iCol = 0; iCol < pFpkVtab->iCdim; iCol++)
			if (iRow >= offset && iRow < iNumCoeff + offset)
				c[iCol * iNumKnots + iRow - offset] = (float)
					sqlite3_column_double(stmt, iCol + 1);

		iRow++;
	}

	if (rc != SQLITE_DONE) {
		*pzErr = sqlite3_mprintf("%s: %s inconsistent row count",
			argv[FPK_ARG_MNAME],
			pFpkVtab->zDataTab);
		goto onError_build_bspline;
	}
	rc = SQLITE_OK;

	if (splcreate(&pFpkVtab->pBSpline, t, iNumKnots, c, iNumCoeff, pFpkVtab->iCdim, pFpkVtab->iOrder - 1)) {
		rc = SQLITE_ERROR;
		*pzErr = sqlite3_mprintf("%s: %s inconsistent spline data",
			argv[FPK_ARG_MNAME],
			pFpkVtab->zDataTab);
		goto onError_build_bspline;
	}
	_fpk_compute_nrow(pFpkVtab);


onError_build_bspline:

	if (rc && t) sqlite3_free(t);
	if (rc && c) sqlite3_free(c);
	sqlite3_finalize(stmt);
	sqlite3_free(sqlite3_str_finish(sql));
	return rc;
}

// ----------------------------------------------------------------------------
static int _fpk_eval_spline(
	fpk_cursor *pFpkCursor)
{
	int rc;
	fpk_vtab *pFpkVtab = (fpk_vtab*)pFpkCursor->p.pVtab;
	float *pKnot = pFpkCursor->pX + pFpkCursor->iCurX;

	if ((rc = splev(pFpkVtab->pBSpline, pKnot, pFpkCursor->pRow, 1)))
		return SQLITE_ABORT;

	pFpkCursor->bEval = 1;
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static void fpk_eval(
	sqlite3_context *ctx,
	int argc,
	sqlite3_value **argv)
{
	int rc;
	fpk_vtab *pFpkVtab = (fpk_vtab*)sqlite3_user_data(ctx);
	float x = (float)sqlite3_value_double(argv[argc-2]);
	int iCol = sqlite3_value_int(argv[argc-1]);
	float *u = sqlite3_malloc(sizeof(float) * pFpkVtab->iCdim);

	if (iCol < 1 || iCol > pFpkVtab->iCdim) {
		char buf[127];
		snprintf(buf, sizeof(buf), "%s: invalid column %d", __func__, iCol);
		sqlite3_result_error(ctx, buf, -1);
		goto onError_evaluate;
	}
	if (x < pFpkVtab->tMin || x > pFpkVtab->tMax) {
		sqlite3_result_null(ctx);
		goto onError_evaluate;
	}
	if ((rc = splev(pFpkVtab->pBSpline, &x, u, 1))) {
		sqlite3_set_errmsg(pFpkVtab->db, -1, "inconsistent spline");
		goto onError_evaluate;
	}

	sqlite3_result_double(ctx, u[iCol-1]);

onError_evaluate:
	sqlite3_free(u);
}

// ----------------------------------------------------------------------------
static void fpk_derive(
	sqlite3_context *ctx,
	int argc,
	sqlite3_value **argv)
{
	fpk_vtab *pFpkVtab = (fpk_vtab*)sqlite3_user_data(ctx);
	int iCol = sqlite3_value_int(argv[argc-1]);
	float *pAux = (float*)sqlite3_get_auxdata(ctx, argc-2);
	int kd = 1; /* 1:st derivative. */
	int bHasAux = !!pAux;

	if (iCol < 1 || iCol > pFpkVtab->iCdim) {
		char buf[127];
		snprintf(buf, sizeof(buf), "%s: invalid column %d", __func__, iCol);
		sqlite3_result_error(ctx, buf, -1);
		return;
	}
	if (!pAux) {
		int nd = pFpkVtab->iCdim * pFpkVtab->iOrder;
		float u = (float)sqlite3_value_double(argv[argc-2]);
		if (!(pAux = (float*)sqlite3_malloc(nd * sizeof(float)))) {
			char buf[127];
			snprintf(buf, sizeof(buf), "%s: no memory", __func__);
			sqlite3_result_error(ctx, buf, -1);
			return;
		}
		if (splder(pFpkVtab->pBSpline, u, pAux, nd)) {
			char buf[127];
			snprintf(buf, sizeof(buf), "%s: inconsistent spline", __func__);
			sqlite3_result_error(ctx, buf, -1);
			sqlite3_free(pAux);
			return;
		}
	}

	int offset = pFpkVtab->iCdim * kd + iCol - 1;
	sqlite3_result_double(ctx, pAux[offset]);
	if (!bHasAux) { /* Last, as dtor may be invoked during call. */
		sqlite3_set_auxdata(ctx, argc-2, pAux, sqlite3_free);
	}
}


// ----------------------------------------------------------------------------
static int fpk_disconnect(
	sqlite3_vtab *pVTab)
{
	int rc = SQLITE_OK;
	fpk_vtab *pFpkVtab = (fpk_vtab*)pVTab;
	if (pFpkVtab->zMetaTab) sqlite3_free(pFpkVtab->zMetaTab);
	if (pFpkVtab->zDataTab) sqlite3_free(pFpkVtab->zDataTab);
	if (pFpkVtab->pBSpline) spldestroy(&pFpkVtab->pBSpline);
	sqlite3_free(pFpkVtab);
	return rc;
}

// ----------------------------------------------------------------------------
static int fpk_destroy(
	sqlite3_vtab *pVTab)
{
	int rc = SQLITE_OK;
	sqlite3_str *sql = NULL;
	fpk_vtab *pFpkVtab = (fpk_vtab*)pVTab;

	sql = sqlite3_str_new(pFpkVtab->db);
	sqlite3_str_appendf(sql, "DROP TABLE IF EXISTS %s", pFpkVtab->zDataTab);
	if ((rc = sqlite3_exec(pFpkVtab->db, sqlite3_str_value(sql), NULL, NULL, NULL)))
		goto onError_destroy;

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "DROP TABLE IF EXISTS %s", pFpkVtab->zMetaTab);
	if ((rc = sqlite3_exec(pFpkVtab->db, sqlite3_str_value(sql), NULL, NULL, NULL)))
		goto onError_destroy;

	if ((rc = fpk_disconnect(pVTab)))
		goto onError_destroy;

onError_destroy:
	sqlite3_free(sqlite3_str_finish(sql));
	return rc;
}

// ----------------------------------------------------------------------------
static int fpk_create(
	sqlite3 *db,
	void *pAux,
	int argc,
	const char* const argv[],
	sqlite3_vtab **ppVTab,
	char **pzErr)
{
	int rc = SQLITE_OK;
	fpk_vtab *pFpkVtab = pFpkVtab = sqlite3_malloc(sizeof(fpk_vtab));
	memset(pFpkVtab, 0, sizeof(fpk_vtab));
	pFpkVtab->iCreat = FPK_INIT_CREATE;
	pFpkVtab->db = db;
	*ppVTab = (sqlite3_vtab*)pFpkVtab;

	/* Set by subroutines on error. */
	if (*pzErr) {
		sqlite3_free(*pzErr);
		*pzErr = NULL;
	}

	/* Nr of knots and coefficient rows. */
	if ((rc = _fpk_parse_args(db, pFpkVtab, argc, argv, pzErr)))
		goto onError;
 
	/* Declare vtab schema. */
	if ((rc = _fpk_declare_vtab(db, pFpkVtab, argc, argv, pzErr)))
		goto onError;

	/* Create backing tables. */
	if ((rc = _fpk_create_tables(db, pFpkVtab, argc, argv, pzErr)))
		goto onError;

	/* Save metadata to backing tables. */
	if ((rc = _fpk_save_meta(db, pFpkVtab, argc, argv, pzErr)))
		goto onError;

	if (pFpkVtab->iCreat & FPK_INIT_PREDEF) {
		/* Save data to backing tables. */
		if ((rc = _fpk_save_predef(db, pFpkVtab, argc, argv, pzErr)))
			goto onError;
		/* Build BSpline from predefs. */
		if ((rc = _fpk_build_bspline(db, pFpkVtab, argc, argv, pzErr)))
			goto onError;
	}
	else if (pFpkVtab->iCreat & FPK_INIT_SMOOTH) {
		/* Build BSpline from data points. */
		if ((rc = _fpk_approx_bspline(db, pFpkVtab, argc, argv, pzErr)))
			goto onError;
		/* Save spline to backing table. */
		if ((rc = _fpk_save_bspline(db, pFpkVtab, argc, argv, pzErr)))
			goto onError;
	}


onError:
	if (rc) fpk_disconnect(&pFpkVtab->p);
	return rc;
}

// ----------------------------------------------------------------------------
static int fpk_connect(
	sqlite3 *db,
	void *pAux,
	int argc,
	const char* const argv[],
	sqlite3_vtab **ppVTab,
	char **pzErr)
{
	int rc = SQLITE_OK;
	fpk_vtab *pFpkVtab = sqlite3_malloc(sizeof(fpk_vtab));
	memset(pFpkVtab, 0, sizeof(fpk_vtab));
	pFpkVtab->iCreat = FPK_INIT_CONNECT;
	pFpkVtab->db = db;
	*ppVTab = (sqlite3_vtab*)pFpkVtab;

	/* Set by subroutines on error. */
	if (*pzErr) {
		sqlite3_free(*pzErr);
		*pzErr = NULL;
	}

	/* Nr of knots and coefficient rows. */
	if ((rc = _fpk_parse_args(db, pFpkVtab, argc, argv, pzErr)))
		goto onError;
 
	/* Declare vtab schema. */
	if ((rc = _fpk_declare_vtab(db, pFpkVtab, argc, argv, pzErr)))
		goto onError;

	/* Try build BSpline. */
	if ((rc = _fpk_build_bspline(db, pFpkVtab, argc, argv, pzErr)))
		goto onError;

onError:
	if (rc) fpk_disconnect(&pFpkVtab->p);
	return rc;
}

// ----------------------------------------------------------------------------
static int fpk_open(
	sqlite3_vtab *pVtab,
	sqlite3_vtab_cursor **ppCursor)
{
	fpk_cursor *pFpkCursor = sqlite3_malloc(sizeof(fpk_cursor));
	memset(pFpkCursor, 0, sizeof(fpk_cursor));

	fpk_vtab *pFpkVtab = (fpk_vtab*)pVtab;
	pFpkCursor->p.pVtab = pVtab;
	pFpkCursor->pRow = sqlite3_malloc(pFpkVtab->iCdim * sizeof(float));
	*ppCursor = &pFpkCursor->p;

	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int fpk_close(
	sqlite3_vtab_cursor *pCursor)
{
	fpk_cursor *pFpkCursor = (fpk_cursor*)pCursor;

	if (pFpkCursor->pX)
		sqlite3_free(pFpkCursor->pX);
	sqlite3_free(pFpkCursor->pRow);
	sqlite3_free(pFpkCursor);
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int fpk_best_index(
	sqlite3_vtab *pVTab,
	sqlite3_index_info *pIdxInfo)
{
	fpk_vtab *pFpkVtab = (fpk_vtab*)pVTab;
	int iArg = 1;
	unsigned char ops = 0;

	/* No constraints implies a full spline evaluation. */
	pIdxInfo->estimatedCost = (double)pFpkVtab->iNRows;
	pIdxInfo->estimatedRows = pFpkVtab->iNRows;
	pIdxInfo->idxNum = FPK_EVAL_RANGE;

	/* 1:st pass - check operands and column. */
	for (int iC = 0; iC < pIdxInfo->nConstraint; iC++) {

		if (!pIdxInfo->aConstraint[iC].usable)
			continue;

		if (pIdxInfo->aConstraint[iC].iColumn > 0)
			return SQLITE_OK;

		ops |= pIdxInfo->aConstraint[iC].op;
	}

	/*
	 * Test if query can be satisfied by evaluating the spline at one or more
	 * distinct points, eventually overriding the sampling distance.
	 */
	 if (ops == SQLITE_INDEX_CONSTRAINT_EQ) {

		for (int iC = 0; iC < pIdxInfo->nConstraint; iC++)
			if (pIdxInfo->aConstraint[iC].usable)
				pIdxInfo->aConstraintUsage[iC].argvIndex = iArg++;

		pIdxInfo->idxNum = FPK_EVAL_POINT;
		pIdxInfo->estimatedCost = (double)(iArg - 1);
		pIdxInfo->estimatedRows = iArg - 1;
		return SQLITE_OK;
	}
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int fpk_next(
	sqlite3_vtab_cursor *pCursor)
{
	fpk_cursor *pFpkCursor = (fpk_cursor*)pCursor;

	pFpkCursor->bEval = 0;
	pFpkCursor->iCurX++;
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int fpk_column(
	sqlite3_vtab_cursor *pCursor,
	sqlite3_context *ctx,
	int iColIdx)
{
	fpk_cursor *pFpkCursor = (fpk_cursor*)pCursor;
	assert(pFpkCursor->iCurX < pFpkCursor->iNumX);
	if (iColIdx == 0) {
		sqlite3_result_double(ctx, *(pFpkCursor->pX + pFpkCursor->iCurX));
		return SQLITE_OK;
	}
	if (!pFpkCursor->bEval && _fpk_eval_spline(pFpkCursor)) {
		return SQLITE_ABORT;
	}
	sqlite3_result_double(ctx, *(pFpkCursor->pRow + iColIdx - 1));
	return SQLITE_OK;
}

static int fpk_rowid(
	sqlite3_vtab_cursor *pCursor,
	sqlite_int64 *pRowid)
{
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int fpk_eof(
	sqlite3_vtab_cursor *pCursor)
{
	fpk_cursor *pFpkCursor = (fpk_cursor*)pCursor;
	return pFpkCursor->iCurX == pFpkCursor->iNumX;
}

// ----------------------------------------------------------------------------
static int fpk_filter(
	sqlite3_vtab_cursor *pCursor,
	int idxNum,
	const char *idxStrUnused,
	int argc,
	sqlite3_value **argv)
{
	fpk_cursor *pFpkCursor = (fpk_cursor*)pCursor;
	fpk_vtab *pFpkVtab = (fpk_vtab*)pCursor->pVtab;
	pFpkCursor->bEval = 0;
	pFpkCursor->iCurX = 0;

	if (idxNum == FPK_EVAL_POINT) {

		pFpkCursor->iNumX = argc;
		pFpkCursor->pX = sqlite3_realloc(pFpkCursor->pX, pFpkCursor->iNumX * sizeof(float));
		for (int i = 0; i < pFpkCursor->iNumX; i++)
			pFpkCursor->pX[i] = sqlite3_value_double(argv[i]);
	}

	else if (idxNum == FPK_EVAL_RANGE) {

		pFpkCursor->iNumX = pFpkVtab->iNRows;
		pFpkCursor->pX = sqlite3_realloc(pFpkCursor->pX, pFpkCursor->iNumX * sizeof(float));
		for (int i = 0; i < pFpkCursor->iNumX; i++)
			pFpkCursor->pX[i] = pFpkVtab->tMin + i * pFpkVtab->tStep;
	}
	return SQLITE_OK;  
}

// ----------------------------------------------------------------------------
static int fpk_find_func(
  sqlite3_vtab *pVtab,
  int nArg,
  const char *zName,
  void (**pxFunc)(sqlite3_context*,int,sqlite3_value**),
  void **ppArg)
{
	if (!strcmp(zName, "fpk_derive")) {
		*pxFunc = fpk_derive;
		*ppArg = pVtab;
		return 1;
	}
	else if (!strcmp(zName, "fpk_evaluate")) {
		*pxFunc = fpk_eval;
		*ppArg = pVtab;
		return 1;
	}
	return 0;
}

static sqlite3_module s_bzModule = {
	1,                       /* iVersion */
	fpk_create,              /* xCreate */
	fpk_connect,             /* xConnect */
	fpk_best_index,          /* xBestIndex */
	fpk_disconnect,          /* xDisconnect */
	fpk_destroy,             /* xDestroy */
	fpk_open,                /* xOpen */
	fpk_close,               /* xClose */
	fpk_filter,              /* xFilter */
	fpk_next,                /* xNext */
	fpk_eof,                 /* xEof */
	fpk_column,              /* xColumn */
	fpk_rowid,               /* xRowid */
	0,                       /* xUpdate */
	0,                       /* xBegin */
	0,                       /* xSync */
	0,                       /* xCommit */
	0,                       /* xRollback */
	fpk_find_func,           /* xFindMethod */
	0,                       /* xRename */
	0,                       /* xSavepoint */
	0,                       /* xRelease */
	0,                       /* xRollbackTo */
	0,                       /* xShadowName */
#if SQLITE_VERSION_NUMBER >= 3044000
	0,                       /* xIntegrity */
#endif
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int sqlite3_extension_init(
	sqlite3 *db, 
	char **pzErrMsg, 
	const sqlite3_api_routines *pApi)
{
	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);

	/* For WITHOUT ROWID clause. */
	if (sqlite3_libversion_number() < 3014000 && pzErrMsg != 0){
		*pzErrMsg = sqlite3_mprintf(
			"fpk_spline() requires SQLite 3.14.0 or later");
		return SQLITE_ERROR;
	}

	for (int nArg = 2; nArg <= 3; nArg++) {
		if ((rc = sqlite3_overload_function(db, "fpk_derive", nArg))) {
			*pzErrMsg = sqlite3_mprintf("failed to register function");
			return SQLITE_ERROR;
		}
		if ((rc = sqlite3_overload_function(db, "fpk_evaluate", nArg))) {
			*pzErrMsg = sqlite3_mprintf("failed to register function");
			return SQLITE_ERROR;
		}
	}
	rc = sqlite3_create_module(db, "fpk_spline", &s_bzModule, NULL);
	return rc;
}
