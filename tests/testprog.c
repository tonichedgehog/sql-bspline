#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sqlite3.h>

#include "testdata.c"

typedef void(*bz_test_fn)(sqlite3*,const char*);

struct bz_test_item;
typedef struct bz_test_item {
	bz_test_fn func;
	sqlite3 *db;
	const char *name;
	struct bz_test_item *next;
	char conn[127];
} bz_test_item;

typedef struct bz_exec_result {
	char **pzColNames;
	double **ppRow;
	int nCol;
	int nRow;
} bz_exec_result;

typedef struct bz_test_data {
	float *points;
	int numPts;
	int dimPts;
} bz_test_data;

#define _BZ_ASSERT(cond, func, line) \
	assert_cond((cond), #cond, func, line)

#define BZ_ASSERT(cond) \
	_BZ_ASSERT(cond, __func__, __LINE__)

#define BZ_EXEC_SUCCESS(db, sql) \
	exec_sql(db, NULL, sql, NULL, 0, __func__, __LINE__)

#define BZ_EXEC_FAILURE(db, sql, expect) \
	exec_sql(db, NULL, sql, expect, 1, __func__, __LINE__)

#define BZ_EXEC_RESULT(db, sql, rset) \
	exec_sql(db, rset, sql, NULL, 0, __func__, __LINE__)

#define BZ_PREDEF_DATA(db, table, cols) \
	make_data_table(db, table, 3, cols, (float*)s_Data, sizeof(s_Data)/(3*sizeof(float)))

#define BZ_SAMPLES_DATA(db, table, cols, tb, te) \
	make_samples_table(db, table, 2, cols, (float*)s_Pts, sizeof(s_Pts)/(2*sizeof(float)),tb,te)

#define BZ_PUSH_TEST(node, proc) \
	do { \
		bz_test_item *head = calloc(1, sizeof(bz_test_item)); \
		head->func = &proc; \
		head->name = #proc; \
		head->next = node; \
		node = head; \
	} while (0)

#define BZ_INIT_RSET(rset) \
	do { \
		*rset = malloc(sizeof(bz_exec_result)); \
		memset(*rset, 0, sizeof(bz_exec_result)); \
	} while (0)

#define BZ_CLEAR_RSET(rset) \
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

#define _BZ_COMPARE_RSET(a,b,tol,func,line) \
	do { \
		_BZ_ASSERT(a->nCol == b->nCol, func, line); \
		_BZ_ASSERT(a->nRow == b->nRow, func, line); \
		for (int i = 0; i < a->nRow; i++) \
			for (int j = 0; j < a->nCol; j++) \
				if (fabs(a->ppRow[i][j] - b->ppRow[i][j]) > tol) { \
					char buf[256]; \
					snprintf(buf, sizeof(buf), \
						"fabs(%f - %f) < %f, row=%d col=%d", \
						a->ppRow[i][j], b->ppRow[i][j], tol,i,j); \
					assert_cond(0, buf, func, line); \
				} \
	} while (0)

#define BZ_COMPARE_RSET(a,b,tol) \
	_BZ_COMPARE_RSET(a,b,tol,__func__,__LINE__)

static const int s_Rows = sizeof(s_Data) / (sizeof(double) * 3);
static char *pzFilter = NULL;
static int iNumFail = 0;
static int iSaveDb = 0;

void print_error(const char *what, const char*func, int line) {
	fprintf(stderr, "** %s ERROR at line %d: '%s'\n",
		func, line, what);
	iNumFail += 1;
}

void assert_cond(bool pCond, const char *zCond, const char *zFunc, int line) {
	if (!pCond)
		print_error(zCond, zFunc, line); 
}

bz_test_data make_test_data_pts()
{
	bz_test_data retval = {
		.points = malloc(sizeof(s_Pts)),
		.numPts = sizeof(s_Pts) / sizeof(float) / 2,
		.dimPts = 2,
	};
	memcpy(retval.points, s_Pts, sizeof(s_Pts));
	return retval;
}

int callback(void *udata, int nCol, char **pzColVals, char **pzColNames)
{
	bz_exec_result *rset = (bz_exec_result*)udata;
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
	bz_exec_result *result,
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

void make_data_table(
	sqlite3 *db,
	const char *table,
	int nCol,
	const char* colNames[],
	const float* data,
	int nRow)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	sqlite3_str *sql = sqlite3_str_new(db);
	sqlite3_str *cols = sqlite3_str_new(db);
	sqlite3_str *vals = sqlite3_str_new(db);
	sqlite3_str_appendf(sql, "CREATE TABLE %s (", table);

	for (int i = 0; i < nCol; i++) {

		if (i > 0) {
			sqlite3_str_appendchar(sql, 1, ',');
			sqlite3_str_appendchar(cols, 1, ',');
			sqlite3_str_appendchar(vals, 1, ',');
		}
		sqlite3_str_appendf(sql, "%s REAL", colNames[i]);
		sqlite3_str_appendf(cols, colNames[i]);
		sqlite3_str_appendchar(vals, 1, '?');
	}
	sqlite3_str_appendchar(sql, 1, ')');

	if (rc = sqlite3_exec(db, sqlite3_str_value(sql), NULL, NULL, NULL))
		goto onError;

	sqlite3_str_reset(sql);
	sqlite3_str_appendf(sql, "INSERT INTO %s (%s) VALUES (%s)",
		table,
		sqlite3_str_value(cols),
		sqlite3_str_value(vals));

	if (rc = sqlite3_prepare(db, sqlite3_str_value(sql), -1, &stmt, NULL))
		goto onError;

	for (int i = 0; i < nRow; i++) {

		for (int j = 1; j <= nCol; j++)
			if (rc = sqlite3_bind_double(stmt, j, *data++))
				goto onError;

		if (rc = sqlite3_step(stmt) != SQLITE_DONE)
			goto onError;

		if (rc = sqlite3_reset(stmt))
			goto onError;
	}

onError:
	sqlite3_finalize(stmt);
	sqlite3_free(sqlite3_str_finish(sql));
	sqlite3_free(sqlite3_str_finish(cols));
	sqlite3_free(sqlite3_str_finish(vals));
}

void make_samples_table(
	sqlite3 *db,
	const char *table,
	int nCol,
	const char* colnames[],
	const float* data,
	int nRow,
	double tBegin,
	double tEnd)
{
	float *blob = malloc((nCol + 1) * nRow * sizeof(float));
	float *p = blob;

	double dt = (tEnd - tBegin) / (nRow-1.0);
	for (int i = 0; i < nRow; i++) {

		*p++ = tBegin + (float)(i * dt);
		for (int j = 0; j < nCol; j++)
			*p++ = data[i*nCol+j];
	}
	make_data_table(db, table, nCol+1, colnames, blob, nRow);
	free(blob);

}

sqlite3* connect_db(const char *conn)
{
	int rc;
	sqlite3 *retval = NULL;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_SHAREDCACHE;

	BZ_ASSERT(!(rc = sqlite3_open_v2(conn, &retval, flags, NULL)));
	BZ_ASSERT(!(rc = sqlite3_db_config(retval, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,1,NULL)));
	BZ_ASSERT(!(rc = sqlite3_load_extension(retval, "./fitpack_spline.so", NULL, NULL)));
	return rc ? NULL : retval;
}

void set_up(bz_test_item *item)
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

void tear_down(bz_test_item *item)
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

void parse_args_no_coeff_cols(sqlite3 *db, const char*)
{
	/* Not enough arguments. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,0.01,data,t)";
	BZ_EXEC_FAILURE(db, sql, "usage");
}

void parse_args_invaid_degree(sqlite3 *db, const char*)
{
	/* Invalid spline degree. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"0,0.1,0.01,data,t,c)";
	BZ_EXEC_FAILURE(db, sql, "degree");
}

void parse_args_negative_step(sqlite3 *db, const char*)
{
	/* Invalid evaluation step. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,-0.1,0.01,data,t,c)";
	BZ_EXEC_FAILURE(db, sql, "step");
}

void parse_args_negative_sfactor(sqlite3 *db, const char*)
{
	/* Invalid smoothing factor. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,-0.01,data,t,c)";
	BZ_EXEC_FAILURE(db, sql, "factor");
}

void parse_args_missing_predef_table(sqlite3 *db, const char*)
{
	/* The given predef table does not exist. */
	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,0.01,'foobar',t,c)";
	BZ_EXEC_FAILURE(db, sql, "foobar");
}

void parse_args_missing_predef_column(sqlite3 *db, const char*)
{
	/* Missing column in the given predef table. */
	const char *colnames[] = {"t", "cx", "cy"};
	BZ_PREDEF_DATA(db, "data", colnames);

	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,0.01,data,t,cx,foobar)";
	BZ_EXEC_FAILURE(db, sql, "foobar");
}

void vtab_created_with_column_names(sqlite3 *db, const char*)
{
	bz_exec_result *rset = NULL;
	const char *colnames[] = {"t", "cx", "cy"};
	BZ_PREDEF_DATA(db, "data", colnames);

	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,1,NULL,data,t,cx,cy)";
	BZ_EXEC_SUCCESS(db, sql);

	BZ_INIT_RSET(&rset);
	BZ_EXEC_RESULT(db, "SELECT * FROM asdf", rset);

	BZ_ASSERT(rset->pzColNames);
	BZ_ASSERT(!strcmp(rset->pzColNames[0], "t"));
	BZ_ASSERT(!strcmp(rset->pzColNames[1], "cx"));
	BZ_ASSERT(!strcmp(rset->pzColNames[2], "cy"));

	BZ_CLEAR_RSET(rset);
}

void vtab_step_define_rowcount(sqlite3 *db, const char*)
{
	bz_exec_result *rset = NULL;
	const char *colnames[] = {"t", "c1", "c2"};
	BZ_PREDEF_DATA(db, "data", colnames);

	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,NULL,data,t,c1,c2)";
	BZ_EXEC_SUCCESS(db, sql);

	BZ_INIT_RSET(&rset);
	BZ_EXEC_RESULT(db, "SELECT * FROM asdf", rset);

	BZ_ASSERT(rset->nCol == 3);
	BZ_ASSERT(rset->nRow == 11);

	BZ_CLEAR_RSET(rset);
}

void vtab_connect_existing_table(sqlite3 *db1, const char *connstr)
{
	bz_exec_result *rset = NULL;
	const char *colnames[] = {"t", "c1", "c2"};
	BZ_PREDEF_DATA(db1, "data", colnames);

	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,NULL,data,t,c1,c2)";
	BZ_EXEC_SUCCESS(db1, sql);

	sqlite3 *db2 = connect_db(connstr);
	BZ_INIT_RSET(&rset);
	BZ_EXEC_RESULT(db2, "SELECT * FROM asdf", rset);

	BZ_ASSERT(rset->nCol == 3);
	BZ_ASSERT(rset->nRow == 11);

	BZ_ASSERT(!sqlite3_close(db2));
	BZ_CLEAR_RSET(rset);
}

void vtab_dropped_removes_backing(sqlite3 *db, const char*)
{
	bz_exec_result *rset1 = NULL, *rset2 = NULL;
	const char *colnames[] = {"t", "c1", "c2"};
	BZ_PREDEF_DATA(db, "data", colnames);

	BZ_INIT_RSET(&rset1);
	BZ_INIT_RSET(&rset2);

	BZ_EXEC_SUCCESS(db, "CREATE VIRTUAL TABLE asdf USING fitpack_spline(\
		3,0.1,NULL,data,t,c1,c2)");
	BZ_EXEC_RESULT(db, "SELECT * FROM sqlite_schema WHERE \
		type = 'table' AND name LIKE 'asdf%'", rset1);
	BZ_ASSERT(rset1->nRow > 0);

	BZ_EXEC_SUCCESS(db, "DROP TABLE asdf");
	BZ_EXEC_RESULT(db, "SELECT * FROM sqlite_schema WHERE \
		type = 'table' AND name LIKE 'asdf%'", rset2);
	BZ_ASSERT(rset2->nRow == 0);

	BZ_CLEAR_RSET(rset1);
	BZ_CLEAR_RSET(rset2);
}

void reconnect_after_predef_dropped(sqlite3 *db1, const char *connstr)
{
	const char *colnames[] = {"t", "c1", "c2"};
	BZ_PREDEF_DATA(db1, "predef", colnames);

	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,NULL,predef,t,c1,c2)";
	BZ_EXEC_SUCCESS(db1, sql);
	BZ_EXEC_SUCCESS(db1, "DROP TABLE predef");

	sqlite3 *db2 = connect_db(connstr);
	BZ_EXEC_SUCCESS(db2, "SELECT * FROM asdf");
	BZ_ASSERT(!sqlite3_close(db2));
}

void reconnect_after_samples_dropped(sqlite3 *db1, const char *connstr)
{
	const char *colnames[] = {"t", "c1", "c2"};
	BZ_SAMPLES_DATA(db1, "samples", colnames,0,1);

	const char *sql = "CREATE VIRTUAL TABLE asdf USING fitpack_spline("
		"3,0.1,0.005,samples,t,c1,c2)";
	BZ_EXEC_SUCCESS(db1, sql);
	BZ_EXEC_SUCCESS(db1, "DROP TABLE samples");

	sqlite3 *db2 = connect_db(connstr);
	BZ_EXEC_SUCCESS(db2, "SELECT * FROM asdf");
	BZ_ASSERT(!sqlite3_close(db2));
}

void spline_match_samples_at_endpoints(sqlite3*db, const char*)
{
	bz_exec_result *rset1 = NULL, *rset2 = NULL;
	const char *colnames[] = {"t", "c1", "c2"};
	BZ_SAMPLES_DATA(db, "pts", colnames,0,1);

	BZ_EXEC_SUCCESS(db, "CREATE VIRTUAL TABLE asdf USING fitpack_spline(\
		3,0.1,0.002,pts,t,c1,c2)");

	BZ_INIT_RSET(&rset1);
	BZ_INIT_RSET(&rset2);

	BZ_EXEC_RESULT(db, "SELECT c1,c2 FROM pts WHERE t = 0 \
		UNION ALL SELECT c1,c2 FROM pts WHERE t = 1", rset1);

	BZ_EXEC_RESULT(db, "SELECT c1,c2 FROM asdf WHERE t = 0 \
		UNION ALL SELECT c1,c2 FROM asdf WHERE t = 1", rset2);

	BZ_COMPARE_RSET(rset1, rset2, 0.02);

	BZ_CLEAR_RSET(rset1);
	BZ_CLEAR_RSET(rset2);
}

void left_join_full_spline_range(sqlite3*db, const char*)
{
	bz_exec_result *rset1 = NULL, *rset2 = NULL;
	const char *colnames[] = {"t", "c1", "c2"};
	BZ_SAMPLES_DATA(db, "pts", colnames,0,100);

	BZ_EXEC_SUCCESS(db, "CREATE VIRTUAL TABLE asdf USING fitpack_spline(\
		3,0.1,0.005,pts,t,c1,c2)");

	BZ_INIT_RSET(&rset1);
	BZ_INIT_RSET(&rset2);

	BZ_EXEC_RESULT(db, "SELECT p.t, p.c1, p.c2 FROM pts p", rset1);
	BZ_EXEC_RESULT(db, "SELECT s.t, s.c1, s.c2 FROM pts p \
		LEFT JOIN asdf s ON p.t = s.t", rset2);

	/* FIXME: tolerance should be 0.005. */
	BZ_COMPARE_RSET(rset1, rset2, 0.02);
	BZ_CLEAR_RSET(rset1);
	BZ_CLEAR_RSET(rset2);
}

int main(int argc, char* argv[])
{
	int nFunc = 0;
	sqlite3 *db = NULL;
	bz_test_item *tail = NULL, *test = NULL;

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

	BZ_PUSH_TEST(tail, left_join_full_spline_range);
	BZ_PUSH_TEST(tail, spline_match_samples_at_endpoints);
	BZ_PUSH_TEST(tail, reconnect_after_samples_dropped);
	BZ_PUSH_TEST(tail, reconnect_after_predef_dropped);
	BZ_PUSH_TEST(tail, vtab_dropped_removes_backing);
	BZ_PUSH_TEST(tail, vtab_connect_existing_table);
	BZ_PUSH_TEST(tail, vtab_step_define_rowcount);
	BZ_PUSH_TEST(tail, vtab_created_with_column_names);
	BZ_PUSH_TEST(tail, parse_args_missing_predef_column);
	BZ_PUSH_TEST(tail, parse_args_missing_predef_table);
	BZ_PUSH_TEST(tail, parse_args_negative_sfactor);
	BZ_PUSH_TEST(tail, parse_args_negative_step);
	BZ_PUSH_TEST(tail, parse_args_invaid_degree);
	BZ_PUSH_TEST(tail, parse_args_no_coeff_cols);

	while (tail && !iNumFail) {

		test = tail, tail = tail->next;
		char buf[80];
		if (pzFilter && !strstr(test->name, pzFilter))
			continue;

		nFunc++;
		printf("-- %s\n", test->name);
		set_up(test);
		test->func(test->db, test->conn);
		tear_down(test);
		free(test);
	}
	if (pzFilter) free(pzFilter);
	printf("** %d tests run, %d failed assertions\n", nFunc, iNumFail);
	return iNumFail;
}
