# FITPACK Spline

SQLite extension for working with sampled, N-dimensional parametric data:

 - Approximating sampled parametric data points with a BSpline.
 - Interpolate a parametric BSpline directly from SQL queries.

This extension uses the [FITPACK](https://netlib.org/dierckx/index.html) Fortran library for smoothing splines, which is also used and exemplary documented by the [SciPy](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.splprep.html#scipy.interpolate.splprep) project.

```sql
CREATE VIRTUAL TABLE vtab USING fitpack_spline(
    3,             -- BSpline degree (1, 3 or 5)
    0.1,           -- Default evaluation step
    5e-2,          -- Smoothing factor
    aTable,        -- Input data table
    t,             -- Parameter column in 'aTable'
    c1, ..., cn)   -- Value columns in 'aTable'
```

### Parameter description

All arguments to the `fitpack_spline' virtual table are positional. At least one value (or coefficient) column is required.

 - _BSpline degree_. See https://netlib.org/dierckx/index.html regarding support for other than cubic splines.

 - _Evaluation step_, defines the sampling distance to use on knots vector when evaluating the spline. The step is adjusted so that t0 + N*dt = t1, where N+1 becomes the number of virtual table rows. 
 - _Smoothing factor_, defines how close the resulting spline will follow the data points. When creating a spline from predefined knots and coefficients this parameter must be NULL.
 - _Input data table_, specifying the input table or view to use as source for either data points or knot vector and coefficients. Any updates to the input table after the virtual is created are ignored.
 - _Parameter column_, or _knots_ when using predefined data. Values must be strict ascending. See below for layout of predefined knots and coefficients.
 - _Data point columns_, or _coefficients_ when using predefined data. See below for layout of predefined knots and coefficients.

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

## Building the extension

The following is needed for building the extension:

 - CMake
 - Fortran-77 and C compilers, e.g. fort77 and gcc
 - SQLite3 headers and libraries.
 
