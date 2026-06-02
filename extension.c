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
 * Example 1 - Create a virtual table representing a parametric cubic spline,
 *   from a table with parameter and data columns. Column names from the input
 *   data table are used for naming the virtual table knot and coefficient
 *   columns.
 * 
 *   CREATE VIRTUAL TABLE vtab USING fitpack_spline(
 *       3,             -- BSpline degree
 *       0.1,           -- Evaluation step
 *       5e-2,          -- Smoothing factor
 *       aTable,        -- Input data table
 *       t,             -- Knot vector column in 'aTable'
 *       c1, ..., cn)   -- Coefficient columns in 'aTable'
 *
 * All arguments to the `fitpack_spline' virtual table are positional. At least one
 * value (or coefficient) column is required.
 *
 *   - BSpline degree. See https://netlib.org/dierckx/index.html regarding
 *       support for other than cubic splines.
 *
 *   - Evaluation step, defines the sampling distance to use on knots vector
 *       when evaluating the spline. The step is adjusted so that t0 + N*dt =
 *       t1, where N+1 becomes the number of virtual table rows.
 *
 *   - Smoothing factor, defines how close the resulting spline will follow the
 *       data points. When creating a spline from predefined knots and
 *       coefficients this parameter must be NULL.
 *
 *   - Input data table, specifying the input table or view to use as source for
 *       either data points or knot vector and coefficients. Any updates to the
 *       input table after the virtual is created are ignored.
 *
 *   - Parameter column, or knots when using predefined data. Values must be
 *       strict ascending. See below for layout of predefined knots and
 *       coefficients.
 *
 *   - Data point columns, or coefficients when using predefined data. See below
 *       for layout of predefined knots and coefficients.
 *
 * Example 2 - Create a virtual table from a table or view containing predefined
 *   knots and coefficients.
 *
 *   CREATE VIRTUAL TABLE vtab USING fitpack_spline(
 *       3,             -- BSpline degree
 *       0.1,           -- Evaluation step
 *       NULL,          -- Smoothing factor
 *       aTable,        -- Input data table
 *       t,             -- Knot vector column in 'aTable'
 *       c1, ..., cn)   -- Coefficient columns in 'aTable'
 * 
 * When creating a spline from predefined knots and coefficients, the layout of
 * the input data table is important. For a spline with degree k, n knots and m
 * coefficients, the relation n = m + k + 1 must hold. Hence, coefficient rows
 * are 'vertically centered', as illustraded below. Coefficients for the (k+1)/2
 * first and last rows shall be set to NULL or zero.
 * 
 *   +---------+----------+----------+
 *   | t       | c1       | c2       |
 *   +---------+----------+----------+
 *   | t_{1}   |        - |        - |
 *   | t_{2}   |        - |        - |
 *   | t_{3}   | c1_{1}   | c2_{1}   |
 *   | t_{4}   | c1_{2}   | c2_{2}   |
 *  ----8<-----------------------------
 *   | t_{n-3} | c1_{m-1} | c2_{m-1} |
 *   | t_{n-2} | c1_{m}   | c2_{m}   |
 *   | t_{n-1} |        - |        - |
 *   | t_{n}   |        - |        - |
 *   +---------+----------+----------+
 */

SQLITE_EXTENSION_INIT1

#define BZ_ARG_MNAME 0    /* Module name arg. */
#define BZ_ARG_TABLE 2    /* Vtable name arg. */
#define BZ_ARG_SPDEG 3    /* Bspline degree arg. */
#define BZ_ARG_TSTEP 4    /* Evaluation delta arg. */
#define BZ_ARG_SFACT 5    /* Smoothing factor. */
#define BZ_ARG_INPUT 6    /* Input table arg. */
#define BZ_ARG_T_COL 7    /* Knots column arg. */
#define BZ_ARG_C_COL 8    /* 1:st coeff. column arg. */

typedef enum bz_init_flags {
	BZ_INIT_CONNECT = 1,  /* Connect to existing table. */
	BZ_INIT_CREATE = 2,   /* Create new table. */
	BZ_INIT_PREDEF = 4,   /* Create from knots and coef. */
	BZ_INIT_SMOOTH = 8,   /* Smoothed spline from samples. */
} bz_init_flags;

typedef enum bz_eval_mode {
	BZ_EVAL_EMPTY,        /* Query produces no rows. */
	BZ_EVAL_POINT,        /* Query single point on knots vector. */
	BZ_EVAL_RANGE,        /* Query an interval on knots vector. */
} bz_eval_mode;

typedef enum bz_eval_range {
	BZ_RANGE_NONE,        /* No lower or upper bound. */
	BZ_RANGE_LOWER,       /* Lower bound given. */
	BZ_RANGE_UPPER,       /* Upper bound given. */
} bz_eval_range;

typedef struct bz_arg {
	const char *key;      /* Argument name. */
	double val;           /* Argument value. */
} bz_arg;

typedef struct bz_cursor {
	sqlite3_vtab_cursor p;
	bz_eval_mode mode;    /* Evaluation mode. */
	bz_eval_range range;  /* Range constraints. */
	float lower;          /* Lower bound. */
	float upper;          /* Upper bound. */
	float *pX;            /* X-values to be evaluated. */
	float *pRow;          /* Evaluated row. */
	int iCurX;            /* Current value. */ 
	int iNumX;            /* Nr of X-values to evaluate. */
	int bEval;            /* Current X-value is evaluated. */
} bz_cursor;

typedef struct bz_vtab {
	sqlite3_vtab p;
	sqlite3 *db;
	bspline *pSpline;
	char *zSrcTab;        /* Data source table, if any. */
	char *zMetaTab;       /* Metadata table name. */
	char *zDataTab;       /* Spline data table name. */
	double sFact;         /* Smoothing factor. */
	double tMin, tMax;    /* Knot vector limits. */
	double tStep;         /* Evaluation step. */
	int iCreat;           /* Create/connect mode. */
	int iRowCount;        /* Virtual row count. */
	int k;                /* BSpline degree. */
	int iCdim;            /* Coeff dimension. */
} bz_vtab;

// ----------------------------------------------------------------------------
static char* _bz_unquoted_str(
	const char *str)
{
	char *dst;
	const char *src;
	size_t len = strlen(str) + 1;
	char *ret = sqlite3_malloc(len * sizeof(char));
	memset(ret, 0, len);
	for (src = str, dst = ret; *src; src++) {
		switch (*src) {
		case '\'':
		case '\"':
			break;
		default:
			*dst++ = *src;
		}
	}
	return ret;
}

// ----------------------------------------------------------------------------
static int _bz_parse_args(
	sqlite3 *db,
	bz_vtab *pBzVtab,
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

	pBzVtab->zMetaTab = sqlite3_mprintf("%s_meta", argv[BZ_ARG_TABLE]);
	pBzVtab->zDataTab = sqlite3_mprintf("%s_data", argv[BZ_ARG_TABLE]);

	/* Value dimension. */
	if ((pBzVtab->iCdim = argc - BZ_ARG_C_COL) < 1) {
		*pzErr = sqlite3_mprintf("usage: %s(k, dt, s, data, t, c1...cn))",
			argv[BZ_ARG_MNAME]);
		rc = SQLITE_ERROR;
		goto onError_parse_args;
	}

	/* BSpline degree. */
	pBzVtab->k = strtol(argv[BZ_ARG_SPDEG], &endp, 10);
	if (pBzVtab->k < 1 || (endp && *endp)) {
		*pzErr = sqlite3_mprintf("%s: invalid degree '%s'",
			argv[BZ_ARG_MNAME],
			argv[BZ_ARG_SPDEG]);
		rc = SQLITE_ERROR;
		goto onError_parse_args;
	}

	/* Evaluation step. */
	pBzVtab->tStep = strtod(argv[BZ_ARG_TSTEP], &endp);
	if (pBzVtab->tStep <= 0 || (endp && *endp)) {
		*pzErr = sqlite3_mprintf("%s: invalid step '%s'",
			argv[BZ_ARG_MNAME],
			argv[BZ_ARG_TSTEP]);
		rc = SQLITE_ERROR;
		goto onError_parse_args;
	}

	/* Not creating a new vtable? Then done here. */
	if (!(pBzVtab->iCreat & BZ_INIT_CREATE))
		goto onError_parse_args;

	/* Smoothing factor. */
	if (!sqlite3_stricmp(argv[BZ_ARG_SFACT], "NULL")) {
		/* Use predefined knots and coeffs. */
		pBzVtab->iCreat |= BZ_INIT_PREDEF;
	}
	else {
		/* Approximate samples with spline. */
		pBzVtab->iCreat |= BZ_INIT_SMOOTH;
		pBzVtab->sFact = strtod(argv[BZ_ARG_SFACT], &endp);
		if (pBzVtab->sFact < 0 || (endp && *endp)) {
			*pzErr = sqlite3_mprintf("%s: invalid smoothing factor '%s'",
				argv[BZ_ARG_MNAME],
				argv[BZ_ARG_SFACT]);
			rc = SQLITE_ERROR;
			goto onError_parse_args;
		}
	}

	/* Predefined data table name? */
	zSrcTab = _bz_unquoted_str(argv[BZ_ARG_INPUT]);
	if (zSrcTab && strlen(zSrcTab)) {
		pBzVtab->zSrcTab = zSrcTab;
		zSrcTab = NULL;
	}

	/* No predef table means done here. */
	if (!pBzVtab->zSrcTab)
		goto onError_parse_args;

	/* Check predef table exists. */
	sqlite3_str_appendf(sql, "PRAGMA table_info(%s)",
		pBzVtab->zSrcTab);
	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, NULL))) {
		*pzErr = sqlite3_mprintf("%s: table '%s' does not exists",
			argv[BZ_ARG_MNAME],
			pBzVtab->zSrcTab);
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
	for (int i = BZ_ARG_T_COL, bFound = 0; i < argc; i++, bFound = 0) {
		for (int j = 0; j < iNumCols; j++) {
			if (!strcmp(argv[i], pzColNames[j])) {
				bFound = 1;
				break;
			}
		}
		if (!bFound) {
			*pzErr = sqlite3_mprintf("%s: table '%s' has no column '%s'",
				argv[BZ_ARG_MNAME],
				pBzVtab->zSrcTab,
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
static int _bz_create_tables(
	sqlite3 *db,
	bz_vtab *pBzVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	sqlite3_str *sql = sqlite3_str_new(db);

	/* Create backing table for knots and coefficients. */
	sqlite3_str_appendf(sql, "CREATE TABLE %s(%s REAL",
		pBzVtab->zDataTab,
		argv[BZ_ARG_T_COL]);
	for (int i = BZ_ARG_C_COL; i < argc; i++)
		sqlite3_str_appendf(sql, ", %s REAL", argv[i]);
	sqlite3_str_appendchar(sql, 1, ')');

	if ((rc = sqlite3_exec(db, sqlite3_str_value(sql), NULL, NULL, pzErr)))
		goto onError_create_tables;

	/* Create backing table for metadata. */
	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "CREATE TABLE %s(key TEXT, val REAL)",
		pBzVtab->zMetaTab);

	if ((rc = sqlite3_exec(db, sqlite3_str_value(sql), NULL, NULL, pzErr)))
		goto onError_create_tables;

onError_create_tables:
	sqlite3_free(sqlite3_str_finish(sql));
	return rc;
}	

// ----------------------------------------------------------------------------
static int _bz_save_meta(
	sqlite3 *db,
	bz_vtab *pBzVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	assert(!*pzErr);
	int rc = SQLITE_OK;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_stmt *stmt = NULL;
	bz_arg meta[3] = {
		{"k", pBzVtab->k},
		{"d", pBzVtab->tStep},
		{"s", pBzVtab->sFact},
	};

	/* Save spline metadata. */
	sqlite3_str_appendf(sql, "INSERT INTO %s VALUES (?1, ?2)",
		pBzVtab->zMetaTab);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, NULL)))
		goto onError_save_meta;

	for (int i = 0; i < sizeof(meta) / sizeof(bz_arg); i++) {

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
static int _bz_save_predef(
	sqlite3 *db,
	bz_vtab *pBzVtab,
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

	/* No predef knota and coeff to save? Then skip. */
	if (!pBzVtab->zSrcTab)
		goto onError_save_predef;

	sqlite3_str_appendf(cols, "%s", argv[BZ_ARG_T_COL]);
	sqlite3_str_appendf(vals, "%s", "?");

	for (int i = BZ_ARG_C_COL; i < argc; i++) {
		sqlite3_str_appendf(cols, ",%s", argv[i]);
		sqlite3_str_appendf(vals, ",?");
	}

	/* Fetch given kots and coefficients. */
	sqlite3_str_appendf(sql, "SELECT %s FROM %s",
		sqlite3_str_value(cols),
		argv[BZ_ARG_INPUT]);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt1, &tail)))
		goto onError_save_predef;

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "INSERT INTO %s (%s) VALUES (%s)",
		pBzVtab->zDataTab,
		sqlite3_str_value(cols),
		sqlite3_str_value(vals));

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt2, NULL)))
		goto onError_save_predef;

	/* Store spline data in private backing table. */
	while ((rc = sqlite3_step(stmt1)) == SQLITE_ROW) {

		for (int i = 0; i <= pBzVtab->iCdim; i++)
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
static int _bz_declare_vtab(
	sqlite3 *db,
	bz_vtab *pBzVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	sqlite3_str *schema = sqlite3_str_new(db);

	/* Build pseudo-create statement. */
	sqlite3_str_appendall(schema, "CREATE TABLE x(");
	for (int i = BZ_ARG_T_COL; i < argc; i++) {
		if (i > BZ_ARG_T_COL)
			sqlite3_str_appendchar(schema, 1, ',');
		sqlite3_str_appendall(schema, argv[i]);
		sqlite3_str_appendall(schema, " REAL");
		if (i == BZ_ARG_T_COL)
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
			argv[BZ_ARG_MNAME],
			__func__,
			sqlite3_errstr(rc));
	return rc;
}

// ----------------------------------------------------------------------------
static int _bz_approx_bspline(
	sqlite3 *db,
	bz_vtab *pBzVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	int iRow = 0, iNx = 0;
	float *pU = NULL, *pX = NULL;
	const char *tail = NULL;
	int k = pBzVtab->k;
	int iXdim = pBzVtab->iCdim;
	float s = pBzVtab->sFact;
	sqlite3_stmt *stmt = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_str *cols = sqlite3_str_new(db);
	sqlite3_str *vals = sqlite3_str_new(db);

	/* Query for size of input. */
	sqlite3_str_appendf(sql, "SELECT COUNT(*) FROM %s",
		pBzVtab->zSrcTab);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_approx_bspline;

	if ((rc = sqlite3_step(stmt)) != SQLITE_ROW)
		goto onError_approx_bspline;

	rc = SQLITE_OK;
	if (!(iNx = sqlite3_column_int(stmt, 0))) {
		assert(!*pzErr);
		*pzErr = sqlite3_mprintf("%s: to few samples: %d",
			argv[BZ_ARG_MNAME],
			iNx);
		rc = SQLITE_ERROR;
		goto onError_approx_bspline;
	}

	/* Copy u,x from table to memory. */
	pU = sqlite3_malloc(iNx * sizeof(float));
	pX = sqlite3_malloc(iNx * sizeof(float) * iXdim);

	for (int i = BZ_ARG_T_COL; i < argc; i++) {
		if (i > BZ_ARG_T_COL) {
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
		pBzVtab->zSrcTab);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_approx_bspline;

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {

		pU[iRow] = (float)sqlite3_column_double(stmt, 0);
		for (int iCol = 0; iCol < pBzVtab->iCdim; iCol++) {
			pX[iRow * iXdim + iCol] = (float)sqlite3_column_double(
				stmt, iCol + 1);
		}
		iRow++;
	}
	assert(iRow == iNx);

	/* Approximate data points with BSpline. */
	if ((rc = splprep(&pBzVtab->pSpline, pX, iXdim, iNx, pU, iNx, k, s)) > 0) {
		switch (rc) {
		case 1:
		case 2:
		case 3:
			*pzErr = sqlite3_mprintf("%s: smoothing spline failed with error %d, factor too small?",
				argv[BZ_ARG_MNAME], rc);
			break;
		case 10:
			*pzErr = sqlite3_mprintf("%s: smoothing spline consistency check failed with error %d",
				argv[BZ_ARG_MNAME], rc);
			break;
		}
		rc = SQLITE_ERROR;
		goto onError_approx_bspline;
	}
	rc = SQLITE_OK;

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
static int _bz_save_bspline(
	sqlite3 *db,
	bz_vtab *pBzVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	int iRow = 0, iNx = 0;
	const char *tail = NULL;
	int offset = (pBzVtab->k + 1) / 2;
	sqlite3_stmt *stmt = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_str *cols = sqlite3_str_new(db);
	sqlite3_str *vals = sqlite3_str_new(db);

	for (int i = BZ_ARG_T_COL; i < argc; i++) {
		if (i > BZ_ARG_T_COL) {
			sqlite3_str_appendchar(cols, 1, ',');
			sqlite3_str_appendchar(vals, 1, ',');
		}
		sqlite3_str_appendf(cols, "%s", argv[i]);
		sqlite3_str_appendf(vals, "?");
	}


	/* Save spline to backing table. */
	sqlite3_str_appendf(sql, "INSERT INTO %s (%s) VALUES (%s)",
		pBzVtab->zDataTab,
		sqlite3_str_value(cols),
		sqlite3_str_value(vals));

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_approx_bspline;

	for (iRow = 0; iRow < pBzVtab->pSpline->n; iRow++) {

		sqlite3_bind_double(stmt, 1, pBzVtab->pSpline->t[iRow]);

		for (int iCol = 0; iCol < pBzVtab->iCdim; iCol++) {

			float *c = pBzVtab->pSpline->c;
			int nc = pBzVtab->pSpline->nc;
			if (iRow >= offset && iRow < nc + offset) {
				sqlite3_bind_double(stmt, iCol + 2, c[iCol * iNx + iRow - offset]);
			}
		}
		if ((rc = sqlite3_step(stmt)) != SQLITE_DONE)
			goto onError_approx_bspline;

		if ((rc = sqlite3_reset(stmt)) != SQLITE_OK)
			goto onError_approx_bspline;
	}
	rc = SQLITE_OK;

onError_approx_bspline:
	sqlite3_free(sqlite3_str_finish(sql));
	sqlite3_free(sqlite3_str_finish(cols));
	sqlite3_free(sqlite3_str_finish(vals));
	sqlite3_finalize(stmt);
	return rc;
}

// ----------------------------------------------------------------------------
static int _bz_build_bspline(
	sqlite3 *db,
	bz_vtab *pBzVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int iNumCoeff, iNumKnots;
	int rc = SQLITE_OK;
	int iRow = 0;
	int offset = (pBzVtab->k + 1) / 2;
	float *t = NULL, *c = NULL;
	const char *tail = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_stmt *stmt = NULL;

	sqlite3_str_appendf(sql, "SELECT COUNT(*) from %s",
		pBzVtab->zDataTab);

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_build_bspline;

	if ((rc = sqlite3_step(stmt)) != SQLITE_ROW)
		goto onError_build_bspline;

	rc = SQLITE_OK;
	if (!(iNumKnots = sqlite3_column_int(stmt, 0))) {
		assert(!*pzErr);
		*pzErr = sqlite3_mprintf("%s: to few knots: %d",
			argv[BZ_ARG_MNAME],
			iNumKnots);
		rc = SQLITE_ERROR;
		goto onError_build_bspline;
	}

	if ((iNumCoeff = iNumKnots - pBzVtab->k - 1) < 1) {
		*pzErr = sqlite3_mprintf("%s: to few coefficients (%d)",
			argv[BZ_ARG_MNAME],
			iNumCoeff);
		rc = SQLITE_ERROR;
		goto onError_build_bspline;
	};
	t = sqlite3_malloc(iNumKnots * sizeof(float));
	c = sqlite3_malloc(iNumKnots * sizeof(float) * pBzVtab->iCdim);

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "SELECT * from %s",
		pBzVtab->zDataTab);

	if ((rc = sqlite3_finalize(stmt)))
		goto onError_build_bspline;

	if ((rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, &tail)))
		goto onError_build_bspline;

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {

		t[iRow] = sqlite3_column_double(stmt, 0);

		for (int iCol = 0; iCol < pBzVtab->iCdim; iCol++)
			if (iRow >= offset && iRow < iNumCoeff + offset)
				c[iCol * iNumKnots + iRow - offset] = (float)
					sqlite3_column_double(stmt, iCol + 1);

		iRow++;
	}

	if (rc != SQLITE_DONE) {
		*pzErr = sqlite3_mprintf("%s: %s inconsistent row count",
			argv[BZ_ARG_MNAME],
			pBzVtab->zDataTab);
		goto onError_build_bspline;
	}
	else
		rc = SQLITE_OK;

	if (splcreate(&pBzVtab->pSpline, t, iNumKnots, c, iNumCoeff, pBzVtab->iCdim, pBzVtab->k)) {
		rc = SQLITE_ERROR;
		goto onError_build_bspline;
	}
	pBzVtab->tMin = pBzVtab->pSpline->t[0];
	pBzVtab->tMax = pBzVtab->pSpline->t[pBzVtab->pSpline->n - 1];

	/* Sampled number of rows. */
	pBzVtab->iRowCount = (int)ceil((pBzVtab->tMax - pBzVtab->tMin) / pBzVtab->tStep) + 1;
	pBzVtab->tStep = (pBzVtab->tMax - pBzVtab->tMin) / (double)(pBzVtab->iRowCount - 1);


onError_build_bspline:

	if (rc && t) sqlite3_free(t);
	if (rc && c) sqlite3_free(c);
	sqlite3_finalize(stmt);
	sqlite3_free(sqlite3_str_finish(sql));
	return rc;
}

// ----------------------------------------------------------------------------
static int _bz_eval_spline(
	bz_cursor *pBzCursor)
{
	int rc;
	bz_vtab *pBzVtab = (bz_vtab*)pBzCursor->p.pVtab;
	float *pKnot = pBzCursor->pX + pBzCursor->iCurX;

	if ((rc = splev(pBzVtab->pSpline, pKnot, pBzCursor->pRow, 1)))
		return SQLITE_ABORT;

	pBzCursor->bEval = 1;
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_disconnect(
	sqlite3_vtab *pVTab)
{
	int rc = SQLITE_OK;
	bz_vtab *pBzVtab = (bz_vtab*)pVTab;
	if (pBzVtab->zMetaTab) sqlite3_free(pBzVtab->zMetaTab);
	if (pBzVtab->zDataTab) sqlite3_free(pBzVtab->zDataTab);
	if (pBzVtab->zSrcTab) sqlite3_free(pBzVtab->zSrcTab);
	if (pBzVtab->pSpline) spldestroy(&pBzVtab->pSpline);
	sqlite3_free(pBzVtab);
	return rc;
}

// ----------------------------------------------------------------------------
static int bz_destroy(
	sqlite3_vtab *pVTab)
{
	int rc = SQLITE_OK;
	sqlite3_str *sql = NULL;
	bz_vtab *pBzVtab = (bz_vtab*)pVTab;

	sql = sqlite3_str_new(pBzVtab->db);
	sqlite3_str_appendf(sql, "DROP TABLE IF EXISTS %s", pBzVtab->zDataTab);
	if ((rc = sqlite3_exec(pBzVtab->db, sqlite3_str_value(sql), NULL, NULL, NULL)))
		goto onError_destroy;

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "DROP TABLE IF EXISTS %s", pBzVtab->zMetaTab);
	if ((rc = sqlite3_exec(pBzVtab->db, sqlite3_str_value(sql), NULL, NULL, NULL)))
		goto onError_destroy;

	if ((rc = bz_disconnect(pVTab)))
		goto onError_destroy;

onError_destroy:
	sqlite3_free(sqlite3_str_finish(sql));
	return rc;
}

// ----------------------------------------------------------------------------
static int bz_create(
	sqlite3 *db,
	void *pAux,
	int argc,
	const char* const argv[],
	sqlite3_vtab **ppVTab,
	char **pzErr)
{
	int rc = SQLITE_OK;
	bz_vtab *pBzVtab = pBzVtab = sqlite3_malloc(sizeof(bz_vtab));
	memset(pBzVtab, 0, sizeof(bz_vtab));
	pBzVtab->iCreat = BZ_INIT_CREATE;
	pBzVtab->db = db;
	*ppVTab = (sqlite3_vtab*)pBzVtab;

	/* Set by subroutines on error. */
	if (*pzErr) {
		sqlite3_free(*pzErr);
		*pzErr = NULL;
	}

	/* Nr of knots and coefficient rows. */
	if ((rc = _bz_parse_args(db, pBzVtab, argc, argv, pzErr)))
		goto onError;
 
	/* Declare vtab schema. */
	if ((rc = _bz_declare_vtab(db, pBzVtab, argc, argv, pzErr)))
		goto onError;

	/* Create backing tables. */
	if ((rc = _bz_create_tables(db, pBzVtab, argc, argv, pzErr)))
		goto onError;

	/* Save metadata to backing tables. */
	if ((rc = _bz_save_meta(db, pBzVtab, argc, argv, pzErr)))
		goto onError;

	if (pBzVtab->iCreat & BZ_INIT_PREDEF) {
		/* Save data to backing tables. */
		if ((rc = _bz_save_predef(db, pBzVtab, argc, argv, pzErr)))
			goto onError;
		/* Build BSpline from predefs. */
		if ((rc = _bz_build_bspline(db, pBzVtab, argc, argv, pzErr)))
			goto onError;
	}
	else if (pBzVtab->iCreat & BZ_INIT_SMOOTH) {
		/* Build BSpline from data points. */
		if ((rc = _bz_approx_bspline(db, pBzVtab, argc, argv, pzErr)))
			goto onError;
		/* Save spline to backing table. */
		if ((rc = _bz_save_bspline(db, pBzVtab, argc, argv, pzErr)))
			goto onError;
	}


onError:
	if (rc) bz_disconnect(&pBzVtab->p);
	return rc;
}

// ----------------------------------------------------------------------------
static int bz_connect(
	sqlite3 *db,
	void *pAux,
	int argc,
	const char* const argv[],
	sqlite3_vtab **ppVTab,
	char **pzErr)
{
	int rc = SQLITE_OK;
	bz_vtab *pBzVtab = sqlite3_malloc(sizeof(bz_vtab));
	memset(pBzVtab, 0, sizeof(bz_vtab));
	pBzVtab->iCreat = BZ_INIT_CONNECT;
	pBzVtab->db = db;
	*ppVTab = (sqlite3_vtab*)pBzVtab;

	/* Set by subroutines on error. */
	if (*pzErr) {
		sqlite3_free(*pzErr);
		*pzErr = NULL;
	}

	/* Nr of knots and coefficient rows. */
	if ((rc = _bz_parse_args(db, pBzVtab, argc, argv, pzErr)))
		goto onError;
 
	/* Declare vtab schema. */
	if ((rc = _bz_declare_vtab(db, pBzVtab, argc, argv, pzErr)))
		goto onError;

	/* Try build BSpline. */
	if ((rc = _bz_build_bspline(db, pBzVtab, argc, argv, pzErr)))
		goto onError;

onError:
	if (rc) bz_disconnect(&pBzVtab->p);
	return rc;
}

// ----------------------------------------------------------------------------
static int bz_open(
	sqlite3_vtab *pVtab,
	sqlite3_vtab_cursor **ppCursor)
{
	bz_cursor *pBzCursor = sqlite3_malloc(sizeof(bz_cursor));
	memset(pBzCursor, 0, sizeof(bz_cursor));

	bz_vtab *pBzVtab = (bz_vtab*)pVtab;
	pBzCursor->p.pVtab = pVtab;
	pBzCursor->pRow = sqlite3_malloc(pBzVtab->iCdim * sizeof(float));
	*ppCursor = &pBzCursor->p;

	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_close(
	sqlite3_vtab_cursor *pCursor)
{
	bz_cursor *pBzCursor = (bz_cursor*)pCursor;

	if (pBzCursor->pX)
		sqlite3_free(pBzCursor->pX);
	sqlite3_free(pBzCursor->pRow);
	sqlite3_free(pBzCursor);
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_best_index(
	sqlite3_vtab *pVTab,
	sqlite3_index_info *pIdxInfo)
{
	bz_vtab *pBzVtab = (bz_vtab*)pVTab;
	int iArg = 1;
	unsigned char ops = 0;

	/* No constraints implies a full spline evaluation. */
	pIdxInfo->estimatedCost = (double)pBzVtab->iRowCount;
	pIdxInfo->estimatedRows = pBzVtab->iRowCount;
	pIdxInfo->idxNum = BZ_EVAL_RANGE;

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

		pIdxInfo->idxNum = BZ_EVAL_POINT;
		pIdxInfo->estimatedCost = (double)(iArg - 1);
		pIdxInfo->estimatedRows = iArg - 1;
		return SQLITE_OK;
	}
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_next(
	sqlite3_vtab_cursor *pCursor)
{
	bz_cursor *pBzCursor = (bz_cursor*)pCursor;

	pBzCursor->bEval = 0;
	pBzCursor->iCurX++;
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_column(
	sqlite3_vtab_cursor *pCursor,
	sqlite3_context *ctx,
	int iColIdx)
{
	bz_cursor *pBzCursor = (bz_cursor*)pCursor;
	assert(pBzCursor->iCurX < pBzCursor->iNumX);

	if (iColIdx == 0) {
		sqlite3_result_double(ctx, *(pBzCursor->pX + pBzCursor->iCurX));
		return SQLITE_OK;
	}
	if (!pBzCursor->bEval && _bz_eval_spline(pBzCursor)) {
		return SQLITE_ABORT;
	}
	sqlite3_result_double(ctx, *(pBzCursor->pRow + iColIdx - 1));
	return SQLITE_OK;
}

static int bz_rowid(
	sqlite3_vtab_cursor *pCursor,
	sqlite_int64 *pRowid)
{
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_eof(
	sqlite3_vtab_cursor *pCursor)
{
	bz_cursor *pBzCursor = (bz_cursor*)pCursor;
	return pBzCursor->iCurX == pBzCursor->iNumX;
}

// ----------------------------------------------------------------------------
static int bz_filter(
	sqlite3_vtab_cursor *pCursor,
	int idxNum,
	const char *idxStrUnused,
	int argc,
	sqlite3_value **argv)
{
	bz_cursor *pBzCursor = (bz_cursor*)pCursor;
	bz_vtab *pBzVtab = (bz_vtab*)pCursor->pVtab;

	pBzCursor->bEval = 0;
	pBzCursor->iCurX = 0;

	if (idxNum == BZ_EVAL_POINT) {

		pBzCursor->iNumX = argc;
		pBzCursor->pX = sqlite3_realloc(pBzCursor->pX, pBzCursor->iNumX * sizeof(float));
		for (int i = 0; i < pBzCursor->iNumX; i++)
			pBzCursor->pX[i] = sqlite3_value_double(argv[i]);
	}

	else if (idxNum == BZ_EVAL_RANGE) {

		pBzCursor->iNumX = pBzVtab->iRowCount;
		pBzCursor->pX = sqlite3_realloc(pBzCursor->pX, pBzCursor->iNumX * sizeof(float));
		for (int i = 0; i < pBzCursor->iNumX; i++)
			pBzCursor->pX[i] = pBzVtab->tMin + i * pBzVtab->tStep;
	}
	return SQLITE_OK;  
}

static sqlite3_module s_bzModule = {
	1,                       /* iVersion */
	bz_create,               /* xCreate */
	bz_connect,              /* xConnect */
	bz_best_index,           /* xBestIndex */
	bz_disconnect,           /* xDisconnect */
	bz_destroy,              /* xDestroy */
	bz_open,                 /* xOpen */
	bz_close,                /* xClose */
	bz_filter,               /* xFilter */
	bz_next,                 /* xNext */
	bz_eof,                  /* xEof */
	bz_column,               /* xColumn */
	bz_rowid,                /* xRowid */
	0,                       /* xUpdate */
	0,                       /* xBegin */
	0,                       /* xSync */
	0,                       /* xCommit */
	0,                       /* xRollback */
	0,                       /* xFindMethod */
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
		"fitpack_spline() requires SQLite 3.14.0 or later");
		return SQLITE_ERROR;
	}

	rc = sqlite3_create_module(db, "fitpack_spline", &s_bzModule, NULL);
	return rc;
}
