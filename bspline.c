#include <tinyspline.h>
#include <sqlite3.h>
#include <sqlite3ext.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#ifdef SQLITE_OMIT_VIRTUALTABLE
#error "this extension require VIRTUALTABLE"
#endif

/**
 * -----------------------------------------------------------------------------
 * BSPLINE - SQLite extension for evaluating BSpline.
 * -----------------------------------------------------------------------------
 *
 * CREATE VIRTUAL TABLE vtab USING bspline(
 *     3,           -- BSpline degree
 *     0.1,         -- Evaluation step
 *     aTable,      -- Input table
 *     t,           -- Knot vector column
 *     c1, ..., cn) -- Coefficient columns
 */

/**
 * -----------------------------------------------------------------------------
 * PARAMETERS
 * -----------------------------------------------------------------------------
 * 
 *   - degree:  BSpline degree, with 3 for cubic spline. See documentation for
 *              https://github.com/msteinbeck/tinyspline regarding support for
 *              other than cubic splines.
 *
 *   - step     The sampling distance to use on knots vector when evaluating the
 *              spline. The step is adjusted so that t0 + N*dt = t1, where N+1
 *              becomes the number of virtual table rows.
 *
 *   - table:   Input table or view to use as source for knot vector and
 *              coefficients. Any updates to the input table after vtab is
 *              created are ignored. Use a trigger and re-create vtab if needed.
 *
 *   - t        The knots vector column of the input table. The M first and last
 *              knots are extrapolated, so that knots and coefficients can be
 *              given in the same table, where M = (degree + 1) / 2. Spline is
 *              assumed to be open, with TS_CLAMPED endpoints.
 *
 *   - c1...cn: One or more coefficient columns of the input table or view. All
 *              values must be convertible to REAL, no NULLs.
 */

SQLITE_EXTENSION_INIT1

#define BZ_ARG_MNAME 0  /* Module name arg. */
#define BZ_ARG_TNAME 2  /* Vtable name arg. */
#define BZ_ARG_SPLDG 3  /* Bspline degree arg. */
#define BZ_ARG_TDIFF 4  /* Evaluation delta arg. */
#define BZ_ARG_TABLE 5  /* Input table arg. */
#define BZ_ARG_T_COL 6  /* Knots column arg. */
#define BZ_ARG_C_COL 7  /* 1:st coeff. column arg. */

typedef enum bz_eval_mode {
	BZ_MODE_EMPTY,        /* Query produces no rows. */
	BZ_MODE_POINT,        /* Query single point on knots vector. */
	BZ_MODE_RANGE,        /* Query an interval on knots vector. */
} bz_eval_mode;

typedef enum bz_eval_range {
	BZ_RANGE_NONE,        /* No lower or upper bound. */
	BZ_RANGE_LOWER,       /* Lower bound given. */
	BZ_RANGE_UPPER,       /* Upper bound given. */
} bz_eval_range;

typedef struct bz_cursor {
	sqlite3_vtab_cursor p;
	tsDeBoorNet net;      /* De Boor's net for eval. */
	bz_eval_mode mode;    /* Evaluation mode. */
	bz_eval_range range;  /* Range constraints. */
	double lower;         /* Lower bound. */
	double upper;         /* Upper bound. */
	double *pKnot;        /* Knots to be evaluated. */
	int iCurKnot;         /* Current knot. */ 
	int iNumKnots;        /* Nr of knots to evaluate. */
	int bEvaluated;       /* Current knot is evaluated. */
} bz_cursor;

typedef struct bz_vtab {
	sqlite3_vtab p;
	tsBSpline spline;
	double tMin, tMax;    /* Knot vector limits. */
	double tStep;         /* Evaluation step. */
	int iRowCount;        /* Virtual row count. */
	int k;                /* BSpline degree. */
	int t_len;            /* Knots vector length. */
	int c_len;            /* Coeff vector length. */
	int c_dim;            /* Coeff dimension. */
} bz_vtab;

// ----------------------------------------------------------------------------
static int _bz_count_dims(
	sqlite3 *db,
	bz_vtab *pBzVtab,
	int argc,
	const char* const argv[],
	char **pzErr)
{
	int rc = SQLITE_OK;
	sqlite3_stmt *stmt = NULL;
	char *endp = NULL;
	const char *errstr = NULL;
	char* sql = sqlite3_mprintf("SELECT COUNT(*) FROM %s", argv[BZ_ARG_TABLE]);

	/* BSpline degree. */
	pBzVtab->k = strtol(argv[BZ_ARG_SPLDG], &endp, 10);
	if (pBzVtab->k <= 0 || (endp && *endp))
		errstr = "invalid degree";
	if (errstr)
		goto onError_count_dims;

	/* Evaluation step. */
	pBzVtab->tStep = strtod(argv[BZ_ARG_TDIFF], &endp);
	if (pBzVtab->tStep <= 0 || (endp && *endp))
		errstr = "invalid step";
	if (errstr)
		goto onError_count_dims;

	/* Row count. */
	if (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL))
		goto onError_count_dims;
	if (rc = sqlite3_step(stmt) != SQLITE_ROW)
		goto onError_count_dims;

	rc = SQLITE_OK;
	pBzVtab->c_dim = argc - BZ_ARG_C_COL;
	pBzVtab->c_len = sqlite3_column_int(stmt, 0);
	pBzVtab->t_len = pBzVtab->c_len + pBzVtab->k + 1;

onError_count_dims:
	if (*pzErr && (rc || errstr))
		sqlite3_free(*pzErr);
	if (sql)
		sqlite3_free(sql);
	if (stmt)
		sqlite3_finalize(stmt);
	if (errstr) {
		*pzErr = sqlite3_mprintf("** %s (%s): %s",
			argv[BZ_ARG_MNAME],
			__func__,
			errstr);
		rc = SQLITE_MISMATCH;
	}
	else if (rc) {
		*pzErr = sqlite3_mprintf("** %s (%s): %s",
			argv[BZ_ARG_MNAME],
			__func__,
			sqlite3_errstr(rc));
	}
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
	if (rc = sqlite3_declare_vtab(db, sqlite3_str_value(schema)))
		goto onError_decl_vtab;

onError_decl_vtab:
	sqlite3_str_reset(schema);
	sqlite3_free(schema);
	if (pzErr && rc)
		sqlite3_free(*pzErr);
	if (rc)
		*pzErr = sqlite3_mprintf("** %s (%s): %s",
			argv[BZ_ARG_MNAME],
			__func__,
			sqlite3_errstr(rc));
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
	int nCol;
	int rc = SQLITE_OK;
	tsError err = TS_SUCCESS;
	tsStatus status = {0,0};
	sqlite3_str *sql = NULL;
	sqlite3_stmt *stmt = NULL;

	int tOffset = (pBzVtab->t_len - pBzVtab->c_len) / 2;
	double *t = calloc(sizeof(double), pBzVtab->t_len);
	double *c = calloc(sizeof(double), pBzVtab->c_len * pBzVtab->c_dim);

	double *pT = t + tOffset;
	double *pC = c;

	/* Build select statement. */
	sql = sqlite3_str_new(db);
	sqlite3_str_appendall(sql, "SELECT ");
	for (int i = BZ_ARG_T_COL; i < argc; i++) {
		if (i > BZ_ARG_T_COL)
			sqlite3_str_appendchar(sql, 1, ',');
		sqlite3_str_appendall(sql, argv[i]);
	}
	sqlite3_str_appendf(sql, " FROM %s", argv[BZ_ARG_TABLE]);

	/* Fetch input data. */
	if (rc = sqlite3_prepare_v2(db, sqlite3_str_value(sql), -1, &stmt, NULL))
		goto onError_build_vtab;

	nCol = sqlite3_column_count(stmt);
	while (rc = sqlite3_step(stmt) == SQLITE_ROW) {

		*pT++ = sqlite3_column_double(stmt, 0);
		for (int i = 1; i < nCol; i++)
			*pC++ = sqlite3_column_double(stmt, i);
	}

	/* Clamp knot vector boundaries. */
	pBzVtab->tMin = t[tOffset];
	for (int i = 0; i < tOffset; i++)
		t[i] = pBzVtab->tMin;

	pBzVtab->tMax = t[pBzVtab->t_len - tOffset - 1];
	for (int i = 1; i <= tOffset; i++)
		t[pBzVtab->t_len - i] = pBzVtab->tMax;

	/* Sampled number of rows. */
	pBzVtab->iRowCount = (int)ceil((pBzVtab->tMax - pBzVtab->tMin) / pBzVtab->tStep) + 1;
	pBzVtab->tStep = (pBzVtab->tMax - pBzVtab->tMin) / (double)(pBzVtab->iRowCount - 1);

	err = ts_bspline_new(
		pBzVtab->c_len,
		pBzVtab->c_dim,
		pBzVtab->k,
		TS_CLAMPED,
		&pBzVtab->spline,
		&status);
	if (err)
		goto onError_build_vtab;

	if (err = ts_bspline_set_control_points(&pBzVtab->spline, c, &status))
		goto onError_build_vtab;

	if (err = ts_bspline_set_knots(&pBzVtab->spline, t, &status))
		goto onError_build_vtab;

onError_build_vtab:
	if (t) free(t);
	if (c) free(c);
	if (sql) {
		sqlite3_str_reset(sql);
		sqlite3_free(sql);
	}
	if (*pzErr && (rc || err))
		sqlite3_free(*pzErr);
	if (rc) {
		*pzErr = sqlite3_mprintf("** %s (%s): %s",
			argv[BZ_ARG_MNAME],
			__func__,
			sqlite3_errstr(rc));
	}
	if (err) {
		*pzErr = sqlite3_mprintf("** %s (%s): %s",
			argv[BZ_ARG_MNAME],
			__func__,
			status.message);
		rc = SQLITE_MISMATCH;
		ts_bspline_free(&pBzVtab->spline);
	}
	return rc;
}

// ----------------------------------------------------------------------------
static int _bz_eval_spline(
	bz_cursor *pBzCursor,
	tsStatus *status)
{
	int len;
	tsError err;
	bz_vtab *pBzVtab = (bz_vtab*)pBzCursor->p.pVtab;
	double *pKnot = pBzCursor->pKnot + pBzCursor->iCurKnot;

	if (err = ts_bspline_eval(&pBzVtab->spline, *pKnot, &pBzCursor->net, status)) {
		return SQLITE_ABORT;
	}
	if ((len = ts_deboornet_num_result(&pBzCursor->net)) != 1) {
		snprintf(status->message, sizeof(status->message), "result count: %d (%d)",
			len, 1);
		return SQLITE_ABORT;
	}
	if ((len = ts_deboornet_len_result(&pBzCursor->net)) != pBzVtab->c_dim) {
		snprintf(status->message, sizeof(status->message), "result length: %d (%d)",
			len, pBzVtab->c_dim);
		return SQLITE_ABORT;
	}
	pBzCursor->bEvaluated = 1;
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_init(
	sqlite3 *db,
	void *pAux,
	int argc,
	const char* const argv[],
	sqlite3_vtab **ppVTab,
	char **pzErr)
{
	int rc = SQLITE_OK;
	bz_vtab *pBzVtab = NULL;
	const char* ext_name = argv[0];

	if (argc < 8)
		goto onUsage;

	pBzVtab = (bz_vtab*)malloc(sizeof(bz_vtab));
	*ppVTab = (sqlite3_vtab*)pBzVtab;

	/* Nr of knots and coefficient rows. */
	if (rc = _bz_count_dims(db, pBzVtab, argc, argv, pzErr))
		goto onError;
 
	/* Declare vtab schema. */
	if (rc = _bz_declare_vtab(db, pBzVtab, argc, argv, pzErr))
		goto onError;

	/* Try build BSpline. */
	if (rc = _bz_build_bspline(db, pBzVtab, argc, argv, pzErr))
		goto onError;

onError:
	if (rc) free (pBzVtab);
	return rc;

onUsage:
	*pzErr = sqlite3_mprintf("usage: %s(degree, step, table, t, c1...cn))", argv[0]);
	return SQLITE_ERROR;
}

// ----------------------------------------------------------------------------
static int bz_final(
	sqlite3_vtab *pVTab)
{
	int rc = SQLITE_OK;
	bz_vtab *pBzVtab = (bz_vtab*)pVTab;
	ts_bspline_free(&pBzVtab->spline);
	free(pBzVtab);
	return rc;
}

// ----------------------------------------------------------------------------
static int bz_open(
	sqlite3_vtab *pUnused,
	sqlite3_vtab_cursor **ppCursor)
{
	bz_cursor *pBzCursor = calloc(sizeof(bz_cursor), 1);
	bz_vtab *pBzVtab = (bz_vtab*)pBzCursor->p.pVtab;
	*ppCursor = &pBzCursor->p;

	pBzCursor->net = ts_deboornet_init();
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_close(
	sqlite3_vtab_cursor *pCursor)
{
	bz_cursor *pBzCursor = (bz_cursor*)pCursor;

	ts_deboornet_free(&pBzCursor->net);
	if (pBzCursor->pKnot)
		free(pBzCursor->pKnot);
	free(pBzCursor);
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
	pIdxInfo->idxNum = BZ_MODE_RANGE;

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

		pIdxInfo->idxNum = BZ_MODE_POINT;
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

	pBzCursor->bEvaluated = 0;
	pBzCursor->iCurKnot++;
	return SQLITE_OK;
}

// ----------------------------------------------------------------------------
static int bz_column(
	sqlite3_vtab_cursor *pCursor,
	sqlite3_context *ctx,
	int iColIdx)
{
	tsStatus status;
	bz_cursor *pBzCursor = (bz_cursor*)pCursor;
	bz_vtab *pBzVtab = (bz_vtab*)pCursor->pVtab;

	assert(pBzCursor->iCurKnot < pBzCursor->iNumKnots);

	if (iColIdx == 0) {
		sqlite3_result_double(ctx, *(pBzCursor->pKnot + pBzCursor->iCurKnot));
		return SQLITE_OK;
	}
	if (!pBzCursor->bEvaluated && _bz_eval_spline(pBzCursor, &status)) {
		sqlite3_result_text(ctx, status.message, -1, NULL);
		return SQLITE_ABORT;
	}
	const double* pResult = ts_deboornet_result_ptr(&pBzCursor->net);
	sqlite3_result_double(ctx, *(pResult + iColIdx - 1));
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
	return pBzCursor->iCurKnot == pBzCursor->iNumKnots;
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

	assert(!pBzCursor->iCurKnot);
	assert(!pBzCursor->pKnot);

	if (idxNum == BZ_MODE_POINT) {

		pBzCursor->iNumKnots = argc;
		pBzCursor->pKnot = malloc(pBzCursor->iNumKnots * sizeof(double));
		for (int i = 0; i < pBzCursor->iNumKnots; i++)
			pBzCursor->pKnot[i] = sqlite3_value_double(argv[i]);
	}

	else if (idxNum == BZ_MODE_RANGE) {

		pBzCursor->iNumKnots = pBzVtab->iRowCount;	
		pBzCursor->pKnot = malloc(pBzCursor->iNumKnots * sizeof(double));
		for (int i = 0; i < pBzCursor->iNumKnots; i++)
			pBzCursor->pKnot[i] = pBzVtab->tMin + i * pBzVtab->tStep;
	}
	return SQLITE_OK;  
}

static sqlite3_module s_bzModule = {
	0,                       /* iVersion */
	bz_init,                 /* xCreate */
	bz_init,                 /* xConnect */
	bz_best_index,           /* xBestIndex */
	bz_final,                /* xDisconnect */
	bz_final,                /* xDestroy */
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
int sqlite3_bspline_init(
	sqlite3 *db, 
	char **pzErrMsg, 
	const sqlite3_api_routines *pApi)
{
	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);

	if (sqlite3_libversion_number() < 3008012 && pzErrMsg != 0){
		*pzErrMsg = sqlite3_mprintf(
		"bspline() requires SQLite 3.8.12 or later");
		return SQLITE_ERROR;
	}

	rc = sqlite3_create_module(db, "bspline", &s_bzModule, 0);
	return rc;
}
