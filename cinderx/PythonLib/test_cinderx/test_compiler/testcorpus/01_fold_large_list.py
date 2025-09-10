# pyre-ignore-all-errors
OPCODES = _makecodes(
    # failure=0 success=1 (just because it looks better that way :-)
    'FAILURE', 'SUCCESS',

    'ANY', 'ANY_ALL',
    'ASSERT', 'ASSERT_NOT',
    'AT',
    'BRANCH',
    'CATEGORY',
    'CHARSET', 'BIGCHARSET',
    'GROUPREF', 'GROUPREF_EXISTS',
    'IN',
    'INFO',
    'JUMP',
    'LITERAL',
    'MARK',
    'MAX_UNTIL',
    'MIN_UNTIL',
    'NOT_LITERAL',
    'NEGATE',
    'RANGE',
    'REPEAT',
    'REPEAT_ONE',
    'SUBPATTERN',
    'MIN_REPEAT_ONE',
    'ATOMIC_GROUP',
    'POSSESSIVE_REPEAT',
    'POSSESSIVE_REPEAT_ONE',

    'GROUPREF_IGNORE',
    'IN_IGNORE',
    'LITERAL_IGNORE',
    'NOT_LITERAL_IGNORE',

    'GROUPREF_LOC_IGNORE',
    'IN_LOC_IGNORE',
    'LITERAL_LOC_IGNORE',
    'NOT_LITERAL_LOC_IGNORE',

    'GROUPREF_UNI_IGNORE',
    'IN_UNI_IGNORE',
    'LITERAL_UNI_IGNORE',
    'NOT_LITERAL_UNI_IGNORE',
    'RANGE_UNI_IGNORE',

    # The following opcodes are only occurred in the parser output,
    # but not in the compiled code.
    'MIN_REPEAT', 'MAX_REPEAT',
)
