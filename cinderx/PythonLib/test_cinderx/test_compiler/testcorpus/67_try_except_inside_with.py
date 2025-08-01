with lock:
    try:
        x[ident] += 1
    except KeyError as ke:
        pass
