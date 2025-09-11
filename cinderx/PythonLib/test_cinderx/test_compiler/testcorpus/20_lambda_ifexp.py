lambda oparg, jmp: ((
            2 if oparg[0] in ("1", "2") else 1
        )
        - 3)
