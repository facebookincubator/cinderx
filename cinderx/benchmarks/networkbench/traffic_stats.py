try:
    import cinderx.jit
    HAS_CINDERX = cinderx.jit.is_enabled()
except ImportError:
    HAS_CINDERX = False

if HAS_CINDERX:
    print("Using traffic_stats_static")
    from cinderx.compiler.strict import loader as static_python_loader
    static_python_loader.install()
    from traffic_stats_static import *
else:
    print("Using traffic_stats_py")
    from traffic_stats_py import *
