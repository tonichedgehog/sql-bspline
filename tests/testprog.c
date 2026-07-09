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
#include <math.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sqlite3.h>

typedef void(*fpk_test_fn)(sqlite3*,const char*);
typedef double(*fpk_data_fn)(double);

struct fpk_test_item;
typedef struct fpk_test_item {
	fpk_test_fn func;
	sqlite3 *db;
	const char *name;
	struct fpk_test_item *next;
	char conn[127];
} fpk_test_item;

typedef struct fpk_exec_result {
	char **pzColNames;
	double **ppRow;
	int nCol;
	int nRow;
} fpk_exec_result;

typedef struct fpk_test_data {
	double *pPts;
	int nRows;
	int nCols;
	char **ppCols;
	fpk_data_fn *pFuncs;
} fpk_test_data;

typedef struct bz_smooth_axis {
	const char *name;
	fpk_data_fn func;
} bz_smooth_axis;

typedef struct fpk_predef_axis {
	const char *name;
	double vals[8];
} fpk_predef_axis;

static char *pzFilter = NULL;
static int iNumFail = 0;
static int iSaveDb = 0;

#define _FPK_ASSERT(cond, func, line) \
	assert_cond((cond), #cond, func, line)

/* Assert condition. */
#define FPK_ASSERT(cond) \
	_FPK_ASSERT(cond, __func__, __LINE__)

/* Execute SQL statement, expect success.*/
#define FPK_EXEC_SUCCESS(db, sql) \
	exec_sql(db, NULL, sql, NULL, 0, __func__, __LINE__)

/* Execute SQL statement, expect failure.*/
#define FPK_EXEC_FAILURE(db, sql, expect) \
	exec_sql(db, NULL, sql, expect, 1, __func__, __LINE__)

/* Execute SQL statement and fetch result.*/
#define FPK_EXEC_RESULT(db, sql, rset) \
	exec_sql(db, rset, sql, NULL, 0, __func__, __LINE__)

#define _FPK_FPEQUAL(a,b,tol,func,line) \
	do { \
		if (isnan(a) || isnan(b)) { \
			if (!isnan(a) || !isnan(b)) { \
				char buf[256]; \
				snprintf(buf, sizeof(buf), \
					"%f == %f",a,b); \
				assert_cond(0, buf, func, line); \
			} \
		} \
		else if (fabs(a - b) > tol) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), \
				"fabs(%f - %f) < %f",a,b,tol); \
			assert_cond(0, buf, func, line); \
		} \
	} while (0)

/* Assert floating point equality. */
#define FPK_FPEQUAL(a,b,tol) \
	_FPK_FPEQUAL(a,b,tol,__func__,__LINE__)

#define BZ_CLEAR_POINTSET(pts) \
	do { \
		for (int i = 0; i < pts->nCols; i++) \
			free(pts->ppCols[i]); \
		free(pts->pPts); \
		free(pts->pFuncs); \
		free(pts->ppCols); \
		free(pts); \
	} while (0)

/* Append test to suite. */
#define FPK_PUSH_TEST(head, tail, proc) \
	do { \
		fpk_test_item *node = calloc(1, sizeof(fpk_test_item)); \
		if (!head) { \
			head = tail = node; \
		} \
		else { \
			tail->next = node; \
			tail = node; \
		} \
		node->func = &proc; \
		node->name = #proc; \
	} while (0)

/* Initialize result set. */
#define FPK_INIT_RSET(rset) \
	do { \
		*rset = malloc(sizeof(fpk_exec_result)); \
		memset(*rset, 0, sizeof(fpk_exec_result)); \
	} while (0)

/* Clear and destroy result set. */
#define FPK_CLEAR_RSET(rset) \
	do { \
		for (int i = 0; i < rset->nCol; i++) \
			free(rset->pzColNames[i]); \
		for (int i = 0; i < rset->nRow; i++) \
			free(rset->ppRow[i]); \
		if (rset->nRow) \
			free(rset->ppRow); \
		if (rset->nCol) \
			free(rset->pzColNames); \
		free(rset); \
	} while (0)

#define FPK_PRINT_RSET(rset) \
	do { \
		printf("=== %s ==================================\n", #rset); \
		for (int i = 0; i < rset->nRow; i++) { \
			for (int j = 0; j < rset->nCol; j++) { \
				printf("  %8.4f", rset->ppRow[i][j]); \
			} \
			printf("\n"); \
		} \
	} while (0)

#define _FPK_COMPARE_RSET(a,b,tol,func,line) \
	do { \
		_FPK_ASSERT(a->nCol == b->nCol, func, line); \
		_FPK_ASSERT(a->nRow == b->nRow, func, line); \
		for (int i = 0; i < a->nRow && !iNumFail; i++) { \
			for (int j = 0; j < a->nCol && !iNumFail; j++) { \
				_FPK_FPEQUAL(a->ppRow[i][j], b->ppRow[i][j], tol, func, line); \
				if (iNumFail) \
					printf("** %s: rset diff at row %d col %d\n",__func__,i,j); \
			} \
		} \
	} while (0)

/* Compare two result sets. */
#define FPK_COMPARE_RSET(a,b,tol) \
	_FPK_COMPARE_RSET(a,b,tol,__func__,__LINE__)

/* Create smoothing spline. */
#define FPK_MAKE_SMOOTH_SPLINE(db, nsamp, sfact, nrow, t0, t1, name, ...) \
	make_smooth_spline(db, nsamp, sfact, nrow, t0, t1, name, __VA_ARGS__)

/* create predefined spline. */
#define FPK_MAKE_PREDEF_SPLINE(db, nknot, nrow, t0, t1, name, ...) \
	make_predef_spline(db, nknot, nrow, t0, t1, name, __VA_ARGS__)

void print_error(const char *what, const char*func, int line) {
	fprintf(stderr, "** %s ERROR at line %d: '%s'\n",
		func, line, what);
	iNumFail += 1;
}

void assert_cond(bool pCond, const char *zCond, const char *zFunc, int line) {
	if (!pCond)
		print_error(zCond, zFunc, line); 
}

int callback(void *udata, int nCol, char **pzColVals, char **pzColNames)
{
	fpk_exec_result *rset = (fpk_exec_result*)udata;
	if (!rset->nRow) {
		rset->nCol = nCol;
		rset->pzColNames = malloc(nCol * sizeof(char*));

		for (int i = 0; i < nCol; i++)
			rset->pzColNames[i] = strdup(pzColNames[i]);
	}
	rset->ppRow = realloc(rset->ppRow, (rset->nRow + 1) * sizeof(double*));
	rset->ppRow[rset->nRow] = malloc(nCol * sizeof(double));
	for (int iCol = 0; iCol < nCol; iCol++)
		if (pzColVals[iCol])
			rset->ppRow[rset->nRow][iCol] = strtod(pzColVals[iCol], NULL);
		else
			rset->ppRow[rset->nRow][iCol] = NAN;
	rset->nRow += 1;
	return 0;
}

void exec_sql(
	sqlite3 *db,
	fpk_exec_result *result,
	const char *sql,
	const char *expect,
	int fail,
	const char *func,
	int line)
{
	int rc;
	char *msg = NULL;

	if (result)
		rc = sqlite3_exec(db, sql, &callback, result, &msg);
	else
		rc = sqlite3_exec(db, sql, NULL, NULL, &msg);

	switch (rc) {
	case SQLITE_OK:
	case SQLITE_DONE:
		if (fail)
			print_error("expected failure", func, line);
		break;
	default:
		if (!fail) {
			char buf[1024];
			snprintf(buf, sizeof(buf), "exec fail with: rc=%d, msg=%s, sql=%s", rc, msg, sql);
			print_error(msg, func, line);
		}
		else if (expect && !strstr(msg, expect)) {
			char buf[1024];
			snprintf(buf, sizeof(buf), "'%s' not in '%s'", expect, msg);
			print_error(buf, func, line);
		}
	}
	if (msg) sqlite3_free(msg);
}

void make_dbtable(sqlite3 *db, fpk_test_data *pts, const char *table)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_str *cols = sqlite3_str_new(db);
	sqlite3_str *vals = sqlite3_str_new(db);
	sqlite3_str_appendf(sql, "CREATE TABLE %s (", table);

	for (int i = 0; i < pts->nCols; i++) {

		if (i > 0) {
			sqlite3_str_appendchar(sql, 1, ',');
			sqlite3_str_appendchar(cols, 1, ',');
			sqlite3_str_appendchar(vals, 1, ',');
		}
		sqlite3_str_appendf(sql, "%s REAL", pts->ppCols[i]);
		sqlite3_str_appendf(cols, pts->ppCols[i]);
		sqlite3_str_appendchar(vals, 1, '?');
	}
	sqlite3_str_appendchar(sql, 1, ')');

	if (rc = sqlite3_exec(db, sqlite3_str_value(sql), NULL, NULL, NULL))
		goto onError;

	if (rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL))
		goto onError;

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "INSERT INTO %s (%s) VALUES (%s)",
		table,
		sqlite3_str_value(cols),
		sqlite3_str_value(vals));

	if (rc = sqlite3_prepare(db, sqlite3_str_value(sql), -1, &stmt, NULL))
		goto onError;

	double *data = pts->pPts;
	for (int i = 0; i < pts->nRows; i++) {

		for (int j = 1; j <= pts->nCols; j++) {
			if (isnan(*data)) {
				if (rc = sqlite3_bind_null(stmt, j))
					goto onError;
				data++;
			}
			else {
				if (rc = sqlite3_bind_double(stmt, j, *data++))
					goto onError;
			}
		}
		if (rc = sqlite3_step(stmt) != SQLITE_DONE)
			goto onError;

		if (rc = sqlite3_reset(stmt))
			goto onError;
	}
	if (rc = sqlite3_exec(db, "END", NULL, NULL, NULL))
		goto onError;

onError:
	sqlite3_finalize(stmt);
	sqlite3_free(sqlite3_str_finish(sql));
	sqlite3_free(sqlite3_str_finish(cols));
	sqlite3_free(sqlite3_str_finish(vals));
}

void make_smooth_spline(
	sqlite3 *db,
	int nSamp,
	float sFact,
	int nRows,
	double tBegin,
	double tEnd,
	const char *table,
	...)
{
	va_list args;
	va_start(args, table);
	bz_smooth_axis *pAxes = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	fpk_test_data *d = calloc(1, sizeof(fpk_test_data));
	double step = (tEnd - tBegin) / (nRows - 1);
	const char *zSamplesTab = "samples";

	do {
		bz_smooth_axis ax = va_arg(args, bz_smooth_axis);
		if (!ax.name || !ax.func)
			break;

		d->nCols++;

		d->pFuncs = (fpk_data_fn*)realloc(d->pFuncs, d->nCols * sizeof(fpk_data_fn));
		d->pFuncs[d->nCols-1] = ax.func;

		d->ppCols = realloc(d->ppCols, d->nCols * sizeof(char*));
		d->ppCols[d->nCols-1] = strdup(ax.name);

	} while (1);
	va_end(args);

	d->nRows = nRows;
	d->pPts = malloc(nRows * d->nCols * sizeof(double));
	double dt = (tEnd - tBegin) / (d->nRows-1.0);
	for (int i = 0; i < d->nRows; i++) {

		double t = tBegin + (i * dt);
		d->pPts[i * d->nCols] = d->pFuncs[0](t);

		for (int j = 1; j < d->nCols; j++) {
			d->pPts[i * d->nCols + j] = d->pFuncs[j](t);
		}
	}
	make_dbtable(db, d, zSamplesTab);

	sqlite3_str_appendf(sql, "CREATE VIRTUAL TABLE %s USING fpk_spline(%d,%f,%f,%s",
		table, 3, step, sFact, zSamplesTab);

	for (int i = 0; i < d->nCols; i++)
		sqlite3_str_appendf(sql, ",%s", d->ppCols[i]);
	sqlite3_str_appendchar(sql, 1, ')');

	exec_sql(db, NULL, sqlite3_str_value(sql), NULL, 0, __func__, __LINE__);
	sqlite3_free(sqlite3_str_finish(sql));
	BZ_CLEAR_POINTSET(d);
}

void make_predef_spline(
	sqlite3 *db,
	int nKnots,
	int nVtabRows,
	double tStart,
	double tStop,
	const char *zVtab,
	...)
{
	fpk_predef_axis *pAxes = NULL;
	double *pCols = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	fpk_test_data *d = calloc(1, sizeof(fpk_test_data));
	double step = (tStop - tStart) / (nVtabRows - 1);
	const char *zPredefTab = "samples";


	va_list args;
	va_start(args, zVtab);

	do {
		fpk_predef_axis ax = va_arg(args, fpk_predef_axis);
		if (!ax.name)
			break;

		d->nCols++;

		d->ppCols = realloc(d->ppCols, d->nCols * sizeof(char*));
		d->ppCols[d->nCols-1] = strdup(ax.name);

		pCols = (double*)realloc(pCols, d->nCols * nKnots * sizeof(double));
		for (int i = 0; i < nKnots; i++)
			pCols[nKnots * (d->nCols - 1) + i] = ax.vals[i];

	} while (1);
	va_end(args);

	d->nRows = nKnots;
	d->pPts = malloc(nKnots * d->nCols * sizeof(double));

	// transpose
	for (int iRow = 0; iRow < nKnots; iRow++)
		for (int iCol = 0; iCol < d->nCols; iCol++)
			d->pPts[iRow * d->nCols + iCol] = pCols[iCol * nKnots + iRow];

	make_dbtable(db, d, zPredefTab);

	sqlite3_str_appendf(sql, "CREATE VIRTUAL TABLE %s USING fpk_spline(%d,%f,NULL,%s",
		zVtab, 3, step, zPredefTab);

	for (int i = 0; i < d->nCols; i++)
		sqlite3_str_appendf(sql, ",%s", d->ppCols[i]);
	sqlite3_str_appendchar(sql, 1, ')');

	exec_sql(db, NULL, sqlite3_str_value(sql), NULL, 0, __func__, __LINE__);

	sqlite3_free(sqlite3_str_finish(sql));
	free(pCols);
	BZ_CLEAR_POINTSET(d);
}


sqlite3* connect_db(const char *conn)
{
	int rc;
	sqlite3 *retval = NULL;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_SHAREDCACHE;

	FPK_ASSERT(!(rc = sqlite3_open_v2(conn, &retval, flags, NULL)));
	FPK_ASSERT(!(rc = sqlite3_db_config(retval, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,1,NULL)));
	FPK_ASSERT(!(rc = sqlite3_load_extension(retval, FPK_EXTENSION, NULL, NULL)));
	return rc ? NULL : retval;
}


double fpk_identity(double t) { return t; }
double fpk_cosinus(double t) { return cos(t); }
double fpk_sinus(double t) { return sin(t); }

double fpk_walk_u(double t) {
    return sin(3.2*t) + 0.2 * cos(28*t) - exp(-t);
}

double fpk_walk_v(double t) {
    return cos(15*(t+0.05)) * (1-0.5*t) + 0.3*t;
}


void set_up(fpk_test_item *item)
{
	iNumFail = 0;
	if (iSaveDb) {
		snprintf(item->conn, sizeof(item->conn), "%s.db", item->name);
	}
	else {
		strncpy(item->conn, "file::memory:?cache=shared", sizeof(item->conn));
	}
	item->db = connect_db(item->conn);
}

void tear_down(fpk_test_item *item)
{
	/* Have SQLite free pzErr, for valgrind. */
	sqlite3_errmsg(item->db);

	int rc;
	if (rc = sqlite3_close(item->db)) {
		char buf[255];
		snprintf(buf, sizeof(buf), "failed to close db (rc=%d)", rc);
		print_error(buf, __func__, __LINE__);
	}
	item->db = NULL;
}

void parse_args_missing_coeff_column(sqlite3 *db, const char*)
{
	/* Not enough arguments. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fpk_spline("
		"3,0.1,0.01,data,t)";
	FPK_EXEC_FAILURE(db, sql, "usage");
}

void parse_args_invaid_bspline_degree(sqlite3 *db, const char*)
{
	/* Invalid spline degree. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fpk_spline("
		"0,0.1,0.01,data,t,c)";
	FPK_EXEC_FAILURE(db, sql, "degree");
}

void parse_args_negative_step(sqlite3 *db, const char*)
{
	/* Invalid evaluation step. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fpk_spline("
		"3,-0.1,0.01,data,t,c)";
	FPK_EXEC_FAILURE(db, sql, "step");
}

void parse_args_negative_smooth_factor(sqlite3 *db, const char*)
{
	/* Invalid smoothing factor. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fpk_spline("
		"3,0.1,-0.01,data,t,c)";
	FPK_EXEC_FAILURE(db, sql, "factor");
}

void parse_args_missing_predef_table(sqlite3 *db, const char*)
{
	/* The given predef table does not exist. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fpk_spline("
		"3,0.1,0.01,'foobar',t,c)";
	FPK_EXEC_FAILURE(db, sql, "foobar");
}

void parse_args_missing_predef_column(sqlite3 *db, const char*)
{
	/* Missing column in the given predef table. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE data ("
		"t REAL, cx REAL, cy REAL)");

	FPK_EXEC_FAILURE(db, "CREATE VIRTUAL TABLE asdf USING fpk_spline("
		"3,0.1,0.01,data,t,cx,foobar)", "foobar");
}

void predef_spline_too_few_coefficients(sqlite3 *db, const char*)
{
	/* Refuse to create predef spline < k + 1 coefficients. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE data ("
		"t REAL, cx REAL, cy REAL)");

	FPK_EXEC_SUCCESS(db, "INSERT INTO data (t,cx,cy) VALUES"
		"(0, NULL, NULL),"
		"(0, NULL, NULL),"
		"(0.0, 0.0, 0.0),"
		"(0.3, 0.3, 0.3),"
		"(1.0, 1.0, 1.0),"
		"(1, NULL, NULL),"
		"(1, NULL, NULL)");

	FPK_EXEC_FAILURE(db, "CREATE VIRTUAL TABLE test USING fpk_spline("
		"3,0.1,NULL,data,t,cx,cy)", "spline");
}

void smooth_spline_too_few_data_points(sqlite3 *db, const char*)
{
	/* Refuse to create smooth spline < k + 1 data points. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE data ("
		"t REAL, cx REAL, cy REAL)");

	FPK_EXEC_SUCCESS(db, "INSERT INTO data (t,cx,cy) VALUES"
		"(0.0, 0.0, 0.0),"
		"(0.5, 0.5, 0.5),"
		"(1.0, 1.0, 1.0)");

	FPK_EXEC_FAILURE(db, "CREATE VIRTUAL TABLE test USING fpk_spline("
		"3,0.1,0.01,data,t,cx,cy)", "spline");
}

void vtab_created_with_column_names(sqlite3 *db, const char*)
{
	/* Spline created with given column names. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 2, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"xx", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){"yy", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){"zz", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	fpk_exec_result *rset = NULL;
	FPK_INIT_RSET(&rset);

	FPK_EXEC_RESULT(db, "SELECT * FROM test", rset);

	FPK_ASSERT(rset->nCol == 4);
	FPK_ASSERT(!strcmp(rset->pzColNames[0], "t"));
	FPK_ASSERT(!strcmp(rset->pzColNames[1], "xx"));
	FPK_ASSERT(!strcmp(rset->pzColNames[2], "yy"));
	FPK_ASSERT(!strcmp(rset->pzColNames[3], "zz"));

	FPK_CLEAR_RSET(rset);
}

void vtab_step_define_rowcount_predef(sqlite3 *db, const char*)
{
	FPK_MAKE_PREDEF_SPLINE(db, 8, 11, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"c1", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){"c2", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	fpk_exec_result *rset = NULL;
	FPK_INIT_RSET(&rset);

	FPK_EXEC_RESULT(db, "SELECT * FROM test", rset);

	FPK_ASSERT(rset->nCol == 3);
	FPK_ASSERT(rset->nRow == 11);

	FPK_CLEAR_RSET(rset);
}

void vtab_step_define_rowcount_smooth(sqlite3 *db, const char*)
{
	FPK_MAKE_SMOOTH_SPLINE(db, 500, 0.005, 11, 0, 1, "test",
		(bz_smooth_axis){"t",  fpk_identity},
		(bz_smooth_axis){"c1", fpk_walk_u},
		(bz_smooth_axis){"c2", fpk_walk_v},
		(bz_smooth_axis){NULL, NULL});

	fpk_exec_result *rset = NULL;
	FPK_INIT_RSET(&rset);

	FPK_EXEC_RESULT(db, "SELECT * FROM test", rset);

	FPK_ASSERT(rset->nCol == 3);
	FPK_ASSERT(rset->nRow == 11);

	FPK_CLEAR_RSET(rset);
}

void vtab_connect_existing_table(sqlite3 *db1, const char *connstr)
{
	FPK_MAKE_PREDEF_SPLINE(db1, 8, 11, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	sqlite3 *db2 = connect_db(connstr);
	fpk_exec_result *rset = NULL;

	FPK_INIT_RSET(&rset);
	FPK_EXEC_RESULT(db2, "SELECT * FROM test", rset);

	FPK_ASSERT(rset->nCol == 2);
	FPK_ASSERT(rset->nRow == 11);

	FPK_ASSERT(!sqlite3_close(db2));
	FPK_CLEAR_RSET(rset);
}

void vtab_dropped_removes_backing(sqlite3 *db, const char*)
{
	FPK_MAKE_PREDEF_SPLINE(db, 8, 11, 0, 1, "asdf",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.3, 0.7, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	fpk_exec_result *rset1 = NULL, *rset2 = NULL;
	FPK_INIT_RSET(&rset1);
	FPK_INIT_RSET(&rset2);

	FPK_EXEC_RESULT(db, "SELECT * FROM sqlite_schema WHERE \
		type = 'table' AND name LIKE 'asdf%'", rset1);
	FPK_ASSERT(rset1->nRow > 0);

	FPK_EXEC_SUCCESS(db, "DROP TABLE asdf");
	FPK_EXEC_RESULT(db, "SELECT * FROM sqlite_schema WHERE \
		type = 'table' AND name LIKE 'asdf%'", rset2);
	FPK_ASSERT(rset2->nRow == 0);

	FPK_CLEAR_RSET(rset1);
	FPK_CLEAR_RSET(rset2);
}

void reconnect_after_predef_dropped(sqlite3 *db1, const char *connstr)
{
	/* Can connect and access vtab after drop predef table. */
	FPK_MAKE_PREDEF_SPLINE(db1, 8, 5, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.3, 0.7, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	sqlite3 *db2 = connect_db(connstr);
	FPK_EXEC_SUCCESS(db2, "SELECT * FROM test");
	FPK_ASSERT(!sqlite3_close(db2));
}

void reconnect_after_samples_dropped(sqlite3 *db1, const char *connstr)
{
	/* Can connect and access vtab after drop samples table. */
	FPK_MAKE_SMOOTH_SPLINE(db1, 100, 0.005, 11, 0, 1, "test",
		(bz_smooth_axis){"t",  fpk_identity},
		(bz_smooth_axis){"c1", fpk_walk_u},
		(bz_smooth_axis){"c2", fpk_walk_v},
		(bz_smooth_axis){NULL, NULL});

	sqlite3 *db2 = connect_db(connstr);
	FPK_EXEC_SUCCESS(db2, "SELECT * FROM test");
	FPK_ASSERT(!sqlite3_close(db2));
}

void warn_attempt_eval_parameter_column(sqlite3 *db, const char*)
{
	/* Spline with x = t, t in range [0,1] with 5 rows. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 5, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0,  0.0,  1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 1./3, 2./3, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN,  NAN,  NAN, NAN, NAN, NAN}});

	/* Raise error, message contains the faulty column. */
	FPK_EXEC_FAILURE(db, "SELECT fpk_evaluate(t, 0) as dx FROM test LIMIT 1",
		"column 0");
}

void warn_eval_past_last_value_column(sqlite3 *db, const char*)
{
	/* Spline with x = t, t in range [0,1] with 5 rows. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 5, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0,  0.0,  1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 1./3, 2./3, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN,  NAN,  NAN, NAN, NAN, NAN}});

	/* Raise error, message contains the faulty column. */
	FPK_EXEC_FAILURE(db, "SELECT fpk_evaluate(t, 2) as dx FROM test LIMIT 1",
		"column 2");
}

void eval_outside_range_returns_null(sqlite3 *db, const char*)
{
	fpk_exec_result *rset1 = NULL, *rset2 = NULL;
	FPK_INIT_RSET(&rset1);
	FPK_INIT_RSET(&rset2);

	/* Spline with x = t, t in range [0,1] with 2 rows. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 2, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0,  0.0,  1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 1./3, 2./3, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN,  NAN,  NAN, NAN, NAN, NAN}});

	/* Positions to be evaluated in left join. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE t_ref (t REAL)");
	FPK_EXEC_SUCCESS(db, "INSERT INTO t_ref (t) VALUES "
		"(-2),(-1),(0),(1),(2)");

	FPK_EXEC_RESULT(db, "SELECT fpk_evaluate(test.t, t_ref.t, 1) as dx "
		"FROM t_ref LEFT JOIN test USING (t)", rset1);

	/* Expected result from left join. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE expect (t REAL)");
	FPK_EXEC_SUCCESS(db, "INSERT INTO expect (t) VALUES "
		"(NULL),(NULL),(0),(1),(NULL)");
	FPK_EXEC_RESULT(db, "SELECT t from expect", rset2);

	FPK_COMPARE_RSET(rset1, rset2, 1e-5);
	FPK_CLEAR_RSET(rset1);
	FPK_CLEAR_RSET(rset2);
}

void eval_between_spline_rows_is_valid(sqlite3 *db, const char*)
{
	fpk_exec_result *rset1 = NULL, *rset2 = NULL;
	FPK_INIT_RSET(&rset1);
	FPK_INIT_RSET(&rset2);

	/* Spline with x = t, t in range [0,1] with 2 rows. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 2, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0,  0.0,  1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 1./3, 2./3, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN,  NAN,  NAN, NAN, NAN, NAN}});

	/* Positions to be evaluated in left join. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE t_ref (t REAL)");
	FPK_EXEC_SUCCESS(db, "INSERT INTO t_ref (t) VALUES "
		"(0.0),(0.1),(0.2)");

	FPK_EXEC_RESULT(db, "SELECT fpk_evaluate(test.t, t_ref.t, 1) as dx "
		"FROM t_ref LEFT JOIN test USING (t)", rset1);

	/* Expected result from left join. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE expect (t REAL)");
	FPK_EXEC_SUCCESS(db, "INSERT INTO expect (t) VALUES "
		"(0.0),(0.1),(0.2)");
	FPK_EXEC_RESULT(db, "SELECT t from expect", rset2);

	FPK_COMPARE_RSET(rset1, rset2, 1e-5);
	FPK_CLEAR_RSET(rset1);
	FPK_CLEAR_RSET(rset2);
}

void left_join_eval_spline_at_refpos(sqlite3 *db, const char *connstr)
{
	/* Spline can be left joined on any t in interval [t0,t1]. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 5, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0,  0.0,  1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"x",  {NAN, NAN, 0.0, 1./3, 2./3, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN,  NAN,  NAN, NAN, NAN, NAN}});

	fpk_exec_result *rset1 = NULL, *rset2 = NULL;
	FPK_INIT_RSET(&rset1);
	FPK_INIT_RSET(&rset2);

	FPK_EXEC_SUCCESS(db, "CREATE TABLE ref (t REAL, x REAL)");
	FPK_EXEC_SUCCESS(db, "INSERT INTO ref (t,x) VALUES"
		"(0.0, 0.0),"
		"(0.3, 0.3),"
		"(0.7, 0.7),"
		"(0.9, 0.9),"
		"(1.0, 1.0)");

	FPK_EXEC_RESULT(db, "SELECT "
		"ref.t as t_ref, "
		"fpk_evaluate(test.t, ref.t, 1) as x FROM ref "
		"LEFT JOIN test USING (t) ", rset1);
	FPK_EXEC_RESULT(db, "SELECT * FROM ref", rset2);

	FPK_COMPARE_RSET(rset1, rset2, 1e-5);
	FPK_CLEAR_RSET(rset1);
	FPK_CLEAR_RSET(rset2);
}

void smoothing_spline_approx_data_points(sqlite3*db, const char*)
{
	FPK_MAKE_SMOOTH_SPLINE(db, 400, 0.0001, 200, 0, 1, "test",
		(bz_smooth_axis){"t", fpk_identity},
		(bz_smooth_axis){"x", fpk_walk_u},
		(bz_smooth_axis){"y", fpk_walk_v},
		(bz_smooth_axis){NULL, NULL});

	FPK_EXEC_SUCCESS(db, "CREATE TABLE ref (t REAL, x REAL, y REAL)");
	for (int i = 0; i <= 100; i++) {
		char buf[127];
		double t = 0.01 * i;
		snprintf(buf, sizeof(buf), "INSERT INTO ref (t,x,y) VALUES "
			"(%f,%f,%f)", t, fpk_walk_u(t), fpk_walk_v(t));
		FPK_EXEC_SUCCESS(db, buf);
	}

	fpk_exec_result *rset1 = NULL, *rset2 = NULL;
	FPK_INIT_RSET(&rset1);
	FPK_INIT_RSET(&rset2);

	FPK_EXEC_RESULT(db, "SELECT ref.t, ref.x, ref.y FROM ref",
		rset1);
	FPK_EXEC_RESULT(db, "SELECT ref.t AS t, "
		"fpk_evaluate(test.t, ref.t, 1) AS x, "
		"fpk_evaluate(test.t, ref.t, 2) AS y "
		"FROM ref LEFT JOIN test USING (t)",
		rset2);

	FPK_COMPARE_RSET(rset1, rset2, 0.005);
	FPK_CLEAR_RSET(rset1);
	FPK_CLEAR_RSET(rset2);
}

void warn_attempt_derive_parameter_column(sqlite3 *db, const char*)
{
	/* Spline with x = t, t in range [0,1] with 5 rows. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 5, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"xx", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	/* Raise error, message contains the faulty column. */
	FPK_EXEC_FAILURE(db, "SELECT fpk_derive(t, 0) as dx FROM test LIMIT 1",
		"column 0");
}

void warn_derive_past_last_value_column(sqlite3 *db, const char*)
{
	/* Spline with x = t, t in range [0,1] with 5 rows. */
	FPK_MAKE_PREDEF_SPLINE(db, 8, 5, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"xx", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	/* Raise error, message contains the faulty column. */
	FPK_EXEC_FAILURE(db, "SELECT fpk_derive(t, 2) as dx FROM test LIMIT 1",
		"column 2");
}

void warn_attempt_derive_invalid_spline(sqlite3 *db, const char*)
{
	/* Raise error on attempt to derive an inconsistent spline .*/
	FPK_MAKE_PREDEF_SPLINE(db, 8, 5, 0, 1, "test",
		(fpk_predef_axis){"t",  {0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0}},
		(fpk_predef_axis){"xx", {NAN, NAN, 0.0, 0.3, 0.7, 1.0, NAN, NAN}},
		(fpk_predef_axis){NULL, {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}});

	FPK_EXEC_FAILURE(db, "SELECT fpk_derive(t, 1) as dx FROM test "
		"WHERE t = 0", "spline");
}

void linear_var_derive_to_constant(sqlite3 *db, const char*)
{
	FPK_MAKE_SMOOTH_SPLINE(db, 400, 0.005, 11, 0, 6, "test",
		(bz_smooth_axis){"t", fpk_identity},
		(bz_smooth_axis){"x", fpk_identity},
		(bz_smooth_axis){NULL, NULL});

	FPK_EXEC_SUCCESS(db, "CREATE TABLE ref ("
		"tr REAL, dx REAL)");
	FPK_EXEC_SUCCESS(db, "INSERT INTO ref (tr,dx) VALUES"
		"(0.0, 1.0),"
		"(0.3, 1.0),"
		"(0.7, 1.0),"
		"(1.0, 1.0)");

	fpk_exec_result *rset1 = NULL, *rset2 = NULL;
	FPK_INIT_RSET(&rset1);
	FPK_INIT_RSET(&rset2);

	FPK_EXEC_RESULT(db, "SELECT ref.tr, fpk_derive(test.t, ref.tr, 1) AS dx FROM ref "
		"LEFT JOIN test ON tr = t ORDER BY ref.tr", rset1);
	FPK_ASSERT(rset1->nRow == 4);

	FPK_EXEC_RESULT(db, "SELECT tr, dx FROM ref ORDER BY tr", rset2);
	FPK_COMPARE_RSET(rset1, rset2, 1e-4);

	FPK_CLEAR_RSET(rset1);
	FPK_CLEAR_RSET(rset2);
}

void sinus_curve_derive_to_cosinus(sqlite3 *db, const char *connstr)
{
	FPK_MAKE_SMOOTH_SPLINE(db, 400, 0.001, 11, 0, 6, "test",
		(bz_smooth_axis){"t", fpk_identity},
		(bz_smooth_axis){"x", fpk_sinus},
		(bz_smooth_axis){"y", fpk_cosinus},
		(bz_smooth_axis){NULL, NULL});

	fpk_exec_result *rset1 = NULL, *rset2 = NULL;
	FPK_INIT_RSET(&rset1);
	FPK_INIT_RSET(&rset2);

	/* Less accurate derivative at endpoints. */
	FPK_EXEC_SUCCESS(db, "CREATE TABLE ref ("
		"t REAL, dx REAL, dy REAL)");
	FPK_EXEC_SUCCESS(db, "INSERT INTO ref (t) VALUES"
		"(0.1)/*,(1),(2),(3),(4),(5),(5.9)*/");
	FPK_EXEC_SUCCESS(db, "UPDATE ref SET dx = cos(t), dy = -sin(t)");

	FPK_EXEC_RESULT(db, "SELECT ref.t, "
		"fpk_derive(test.t, ref.t, 1) as dx, "
		"fpk_derive(test.t, ref.t, 2) as dy "
		"FROM ref LEFT JOIN test USING (t)",
		rset1);

	FPK_EXEC_RESULT(db, "SELECT t, dx, dy FROM ref", rset2);
	FPK_COMPARE_RSET(rset1, rset2, 0.05);

	FPK_CLEAR_RSET(rset1);
	FPK_CLEAR_RSET(rset2);
}

int main(int argc, char* argv[])
{
	int nFunc = 0;
	sqlite3 *db = NULL;
	fpk_test_item *head = NULL, *tail = NULL, *test = NULL;

	FPK_PUSH_TEST(head, tail, parse_args_missing_coeff_column);
	FPK_PUSH_TEST(head, tail, parse_args_invaid_bspline_degree);
	FPK_PUSH_TEST(head, tail, parse_args_negative_step);
	FPK_PUSH_TEST(head, tail, parse_args_negative_smooth_factor);
	FPK_PUSH_TEST(head, tail, parse_args_missing_predef_table);
	FPK_PUSH_TEST(head, tail, parse_args_missing_predef_column);
	FPK_PUSH_TEST(head, tail, predef_spline_too_few_coefficients);
	FPK_PUSH_TEST(head, tail, smooth_spline_too_few_data_points);
	FPK_PUSH_TEST(head, tail, vtab_created_with_column_names);
	FPK_PUSH_TEST(head, tail, vtab_step_define_rowcount_predef);
	FPK_PUSH_TEST(head, tail, vtab_step_define_rowcount_smooth);
	FPK_PUSH_TEST(head, tail, vtab_connect_existing_table);
	FPK_PUSH_TEST(head, tail, vtab_dropped_removes_backing);
	FPK_PUSH_TEST(head, tail, reconnect_after_predef_dropped);
	FPK_PUSH_TEST(head, tail, reconnect_after_samples_dropped);
	FPK_PUSH_TEST(head, tail, warn_attempt_eval_parameter_column);
	FPK_PUSH_TEST(head, tail, warn_eval_past_last_value_column);
	FPK_PUSH_TEST(head, tail, eval_outside_range_returns_null);
	FPK_PUSH_TEST(head, tail, eval_between_spline_rows_is_valid);
	FPK_PUSH_TEST(head, tail, left_join_eval_spline_at_refpos);
	FPK_PUSH_TEST(head, tail, smoothing_spline_approx_data_points);
	FPK_PUSH_TEST(head, tail, warn_attempt_derive_parameter_column);
	FPK_PUSH_TEST(head, tail, warn_derive_past_last_value_column);
	FPK_PUSH_TEST(head, tail, warn_attempt_derive_invalid_spline);
	FPK_PUSH_TEST(head, tail, linear_var_derive_to_constant);
	FPK_PUSH_TEST(head, tail, sinus_curve_derive_to_cosinus);

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--save-db")) {
			iSaveDb = 1;
			continue;
		}
		if (!strcmp(argv[i], "--run-only") && argc > i) {
			pzFilter = strdup(argv[i+1]);
			i += 1;
			continue;
		}
	}

	while (head) {

		test = head, head = head->next;

		if (!iNumFail && (!pzFilter || strstr(test->name, pzFilter))) {

			nFunc++;
			printf("-- %s\n", test->name);
			set_up(test);
			test->func(test->db, test->conn);
			tear_down(test);
		}

		free(test);
	}
	if (pzFilter) free(pzFilter);
	printf("** %d tests run, %d failed assertions\n", nFunc, iNumFail);
	return iNumFail;
}
