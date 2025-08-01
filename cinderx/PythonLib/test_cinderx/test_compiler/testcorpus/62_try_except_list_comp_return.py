# pyre-ignore-all-errors
def foo():
    try:
        [x for x in abc]
    except OSError:
        pass

    return
