try:
    import cinderx.jit
    HAS_CINDERX = cinderx.jit.is_enabled()
except ImportError:
    HAS_CINDERX = False

if HAS_CINDERX:
    print("Using simple_web_framework_static")
    from cinderx.compiler.strict import loader as static_python_loader
    static_python_loader.install()
    from simple_web_framework_static import *
else:
    print("Using simple_web_framework_py")
    from simple_web_framework_py import *
