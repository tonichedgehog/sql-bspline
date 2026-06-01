import pytest
import math

def test_insufficient_number_of_args(conn):
    conn.make_table()
    with pytest.raises(Exception, match=r'usage: bspline\('):
        conn.dbh.execute('''CREATE VIRTUAL TABLE vtab USING bspline(3, 0.1,path1, t)''')

def test_invalid_degree_arg(conn):
    conn.make_table()
    for d in ('asdf', 0, -1, 2.7):
        with pytest.raises(Exception, match=r'invalid degree'):
            conn.dbh.execute(f'''CREATE VIRTUAL TABLE vtab USING bspline({d},0.1,path1, t, cx, cy)''')

def test_invalid_step_arg(conn):
    conn.make_table()
    for d in ('asdf', 0, -1):
        with pytest.raises(Exception, match=r'invalid step'):
            conn.dbh.execute(f'''CREATE VIRTUAL TABLE vtab USING bspline(3,{d},path1, t, cx, cy)''')

def test_compare_scipy_eval_at_points(conn):
    conn.make_vtable()
    spline = conn.make_spline()
    cursor = conn.dbh.cursor()
    for t in (0, 2.4468, 2.0, 7.5, 12.3333, 22.5, 28.872, 30):
        rs = cursor.execute('SELECT cx,cy FROM vtab WHERE t = ?', [t])
        for given,expect in zip(list(rs)[0], spline(t)):
            assert math.fabs(given - expect) < 1e-5

def test_count_asterisk_sampled_rows(conn):
    conn.make_vtable(dt=1)
    cursor = conn.dbh.cursor()
    rs = cursor.execute('SELECT COUNT(*) FROM vtab', [])
    assert list(rs)[0][0] == 31

def test_compare_scipy_sampled_rows(conn):
    conn.make_vtable(dt=1)
    spline = conn.make_spline()
    cursor = conn.dbh.cursor()
    rs = cursor.execute('SELECT cx,cy FROM vtab', [])
    for given,t in zip(rs, range(0,31,1)):
        for a,b in zip(given, spline(t)):
            assert math.fabs(a - b) < 1e-5
