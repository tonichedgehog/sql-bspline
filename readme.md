# FITPACK Spline

SQLite extension for working with sampled, N-dimensional parametric data:

 - Approximating sampled parametric data points with a BSpline.
 - Evaluate and derive a parametric BSpline directly from SQL queries.

This extension uses the venerable [FITPACK](https://netlib.org/dierckx/index.html) Fortran library for smoothing splines, which is also used and exemplary documented by the [SciPy](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.splprep.html#scipy.interpolate.splprep) project.

## Fit spline from data points

```sql
CREATE VIRTUAL TABLE vtab USING fpk_spline(
    3,             -- BSpline degree (1, 3 or 5)
    0.1,           -- Default evaluation step
    5e-2,          -- Smoothing factor
    aTable,        -- Input data table
    t,             -- Parameter column in 'aTable'
    c1, ..., cn)   -- Value columns in 'aTable'
```

### Parameter description

All arguments to the `fpk_spline' virtual table are positional. At least one value column is required.

 - _BSpline degree_. See https://netlib.org/dierckx/index.html regarding support for other than cubic splines.

 - _Evaluation step_, defines the sampling distance to use on knots vector when evaluating the spline. The step is adjusted so that t0 + N*dt = t1, where N+1 becomes the number of virtual table rows. 
 - _Smoothing factor_, defines how close the resulting spline will follow the data points. Smaller values gives a closer fit.
 - _Input data table_, specifying the input table or view to use as source for data points. Any updates to the input table after the virtual is created are ignored.
 - _Parameter column_, or _knots_ when using predefined data. Values must be strict ascending.
 - _Data point columns_, one column per dimension. At least one column is required, max 10.

## Spline from knots and coefficients

```sql
CREATE VIRTUAL TABLE vtab USING fpk_spline(
    3,             -- BSpline degree (1, 3 or 5)
    0.1,           -- Default evaluation step
    NULL,          -- Smoothing factor
    aTable,        -- Input data table
    t,             -- Knot vector column in 'aTable'
    c1, ..., cn)   -- Coefficient columns in 'aTable'
```

### Parameter description

 - _Smoothing factor_, this parameter must be NULL when creating a spline from predefined knots and coefficients .
 - _Input data table_, specifying the input table or view to use as source for knot vector and coefficients. Any updates to the input table after the virtual is created are ignored.
 - _Knot vector column_, when using predefined data. Values must be strict ascending. See below for layout of predefined knots and coefficients.
 - _Coefficient columns_, when using predefined data. See below for layout of predefined knots and coefficients.

If the two parameter column arguments seem redundant, it is because they are. SQLite require the 1:st argument of a table-valued function to be a column in the associated virtual table. Only the two last arguments are used.

### Spline data table layout

When creating a spline from predefined knots and coefficients, the layout of the input data table is important. For a spline with degree k, n knots and m coefficients, the relation n = m + k + 1 must hold. Hence, coefficient rows are vertically centered, as illustraded below. Coefficients for the (k+1)/2 first and last rows shall be set to NULL or zero.

```
 +---------+----------+----------+
 | t       | c1       | c2       |
 +---------+----------+----------+
 | t_{1}   |        - |        - |
 | t_{2}   |        - |        - |
 | t_{3}   | c1_{1}   | c2_{1}   |
 | t_{4}   | c1_{2}   | c2_{2}   |
----8<-----------------------------
 | t_{n-3} | c1_{m-1} | c2_{m-1} |
 | t_{n-2} | c1_{m}   | c2_{m}   |
 | t_{n-1} |        - |        - |
 | t_{n}   |        - |        - |
 +---------+----------+----------+
```

# Evaluate and derive at arbitrary position

This extension provides two table-valued functions for evaluation (`fpk_evaluate`) and derivation (`fpk_derive`) of a BSpline at arbitrary parameter positions.

```sql
CREATE TABLE refpt (t REAL)
INSERT INTO refpt (t) VALUES (0.0),(0.3),(0.7),(1.0)

SELECT
    refpt.t AS t,
    fpk_evaluate(vtab.t, ref.t, 1) AS x
    fpk_derive(vtab.t, ref.t, 1) AS dx
FROM ref
LEFT JOIN test USING (t)
```

### Parameter description

 - _vtab.t_ The knots column in the BSpline virtual table.
 - _ref.t_ The column containing the points to be evaluated or derived.
 - _N_ The n:th coefficient column in the BSpline virtual table to evaluate or derive.

## Building the extension

The following is needed for building the extension:

 - CMake
 - Fortran-77 and C compilers, e.g. fort77 and gcc
 - SQLite3 headers and libraries.
 
