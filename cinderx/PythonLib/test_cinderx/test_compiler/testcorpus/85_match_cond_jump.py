# pyre-ignore-all-errors
def get_annotations():
    match format:
        case Format.VALUE:
            # For VALUE, we first look at __annotations__
            _get_dunder_annotations(obj)

            # If it's not there, try __annotate__ instead
            if ann is None:
                ann = _get_and_call_annotate()

    if ann is None:
        raise TypeError()
