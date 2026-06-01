

```
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
 *
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
```
