# pyre-ignore-all-errors
head = "aa%s\n%s\n%s\n" % (str(2), 2, 3)
head = "aa%s\n%s\n%s\n" % (
    str(2),
    2,
)
head = "aa%s\n%s\n%s\n" % (str(2), 2, 3, 4)
head = "%s\n%s\n%s\n" % (str(2), 2, 3, 4)
head = "%s\n%s\n%s" % (str(2), 2, 3, 4)
head = "%3s\n%s\n%s" % (str(2), 2, 3, 4)
head = "%-3s\n%s\n%s" % (str(2), 2, 3, 4)
head = "%+3s\n%#s\n%0s" % (str(2), 2, 3, 4)
if x < 0:
    pass
else:
    head = "aaaa%+3s\n%#s\n%0s" % (str(2), 2, 3, 4)
head = "%d" % (str(2),)
