# Copyright (c) Meta Platforms, Inc. and affiliates.

"""
N-body gravitational simulation benchmark.

Simulates the Sun-Jupiter-Saturn-Uranus-Neptune system using a simple
symplectic integrator. Exercises tight numerical loops with floating-point
arithmetic, repeated attribute access on simple objects, and list iteration.

Based on the classic "nbody" benchmark from the Computer Language
Benchmarks Game.
"""

import sys

import cinderx.jit


PI = 3.14159265358979323846
SOLAR_MASS = 4 * PI * PI
DAYS_PER_YEAR = 365.24


class Body(object):
    def __init__(self, x, y, z, vx, vy, vz, mass):
        self.x = x
        self.y = y
        self.z = z
        self.vx = vx
        self.vy = vy
        self.vz = vz
        self.mass = mass


def make_sun():
    return Body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS)


def make_jupiter():
    return Body(
        4.84143144246472090e00,
        -1.16032004402742839e00,
        -1.03622044471123109e-01,
        1.66007664274403694e-03 * DAYS_PER_YEAR,
        7.69901118419740425e-03 * DAYS_PER_YEAR,
        -6.90460016972063023e-05 * DAYS_PER_YEAR,
        9.54791938424326609e-04 * SOLAR_MASS,
    )


def make_saturn():
    return Body(
        8.34336671824457987e00,
        4.12479856412430479e00,
        -4.03523417114321381e-01,
        -2.76742510726862411e-03 * DAYS_PER_YEAR,
        4.99852801234917238e-03 * DAYS_PER_YEAR,
        2.30417297573763929e-05 * DAYS_PER_YEAR,
        2.85885980666130812e-04 * SOLAR_MASS,
    )


def make_uranus():
    return Body(
        1.28943695621391310e01,
        -1.51111514016986312e01,
        -2.23307578892655734e-01,
        2.96460137564761618e-03 * DAYS_PER_YEAR,
        2.37847173959480950e-03 * DAYS_PER_YEAR,
        -2.96589568540237556e-05 * DAYS_PER_YEAR,
        4.36624404335156298e-05 * SOLAR_MASS,
    )


def make_neptune():
    return Body(
        1.53796971148509165e01,
        -2.59193146099879641e01,
        1.79258772950371181e-01,
        2.68067772490389322e-03 * DAYS_PER_YEAR,
        1.62824170038242295e-03 * DAYS_PER_YEAR,
        -9.51592254519715870e-05 * DAYS_PER_YEAR,
        5.15138902046611451e-05 * SOLAR_MASS,
    )


def offset_momentum(bodies):
    px = 0.0
    py = 0.0
    pz = 0.0
    for body in bodies:
        px -= body.vx * body.mass
        py -= body.vy * body.mass
        pz -= body.vz * body.mass
    sun = bodies[0]
    sun.vx = px / SOLAR_MASS
    sun.vy = py / SOLAR_MASS
    sun.vz = pz / SOLAR_MASS


def advance(bodies, dt, n_bodies):
    for i in range(n_bodies):
        bi = bodies[i]
        bi_x = bi.x
        bi_y = bi.y
        bi_z = bi.z
        bi_mass = bi.mass
        bi_vx = bi.vx
        bi_vy = bi.vy
        bi_vz = bi.vz
        for j in range(i + 1, n_bodies):
            bj = bodies[j]
            dx = bi_x - bj.x
            dy = bi_y - bj.y
            dz = bi_z - bj.z
            dist_sq = dx * dx + dy * dy + dz * dz
            dist = dist_sq**0.5
            mag = dt / (dist_sq * dist)
            bj_mass = bj.mass
            bi_vx -= dx * bj_mass * mag
            bi_vy -= dy * bj_mass * mag
            bi_vz -= dz * bj_mass * mag
            bj.vx += dx * bi_mass * mag
            bj.vy += dy * bi_mass * mag
            bj.vz += dz * bi_mass * mag
        bi.vx = bi_vx
        bi.vy = bi_vy
        bi.vz = bi_vz
    for i in range(n_bodies):
        bi = bodies[i]
        bi.x += dt * bi.vx
        bi.y += dt * bi.vy
        bi.z += dt * bi.vz


def energy(bodies, n_bodies):
    e = 0.0
    for i in range(n_bodies):
        bi = bodies[i]
        e += 0.5 * bi.mass * (bi.vx * bi.vx + bi.vy * bi.vy + bi.vz * bi.vz)
        for j in range(i + 1, n_bodies):
            bj = bodies[j]
            dx = bi.x - bj.x
            dy = bi.y - bj.y
            dz = bi.z - bj.z
            dist = (dx * dx + dy * dy + dz * dz) ** 0.5
            e -= (bi.mass * bj.mass) / dist
    return e


class NBody(object):
    def run(self, iterations):
        for _ in range(iterations):
            bodies = [
                make_sun(),
                make_jupiter(),
                make_saturn(),
                make_uranus(),
                make_neptune(),
            ]
            offset_momentum(bodies)
            n_bodies = len(bodies)
            n_steps = 2000000
            dt = 0.01
            for _ in range(n_steps):
                advance(bodies, dt, n_bodies)
            final_energy = energy(bodies, n_bodies)
            # After 2000000 steps, the energy should be conserved to within
            # a known tolerance. The expected value was determined empirically.
            if abs(final_energy - (-0.16902628585296134)) > 1e-8:
                return False
        return True


if __name__ == "__main__":
    cinderx.jit.auto()

    num_iterations = 1
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    NBody().run(num_iterations)
