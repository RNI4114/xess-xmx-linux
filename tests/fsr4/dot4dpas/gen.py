#!/usr/bin/env python3
"""gen.py: shaders with chains of 8-bit dot products against constant weights (the FSR4 INT8 pattern), their
inputs and the expected outputs (CPU reference). Cases: name C K signed [extra]."""
import random, struct, sys

# Suffixes: i = initial accumulators from the inputs, b = inside a bounds check (non-uniform if), e = with an else
# that stores too (stays dot products), w = workgroups of 40 (partial subgroups) with a bounds check, u = weights
# read from a buffer (binding 2) at uniform addresses instead of constants, n = the K range split over three non-uniform
# ifs that forward the accumulators when not taken (as FSR4's 0058 does per 3x3 neighbor), s = weights that fit in 16 bits
# (a dp4a with a 16-bit immediate is wrong on Xe2; Jay used to emit it).
CASES = [('c64k8', 64, 8, True), ('c32k16', 32, 16, True), ('c16k36', 16, 36, True), ('c13k12', 13, 12, True),
         ('u24k8', 24, 8, False), ('c40k9i', 40, 9, True), ('c32k16b', 32, 16, True), ('c16k8e', 16, 8, True),
         ('c24k12w', 24, 12, True), ('c32k16u', 32, 16, True), ('c13k12bu', 13, 12, True), ('c16k4u', 16, 4, True), ('c16k12n', 16, 12, True), ('c16k8s', 16, 8, True)]
NI = 64 * 32   # invocations
LIMIT = NI - 37


def s8(v):
    return v - 256 if v >= 128 else v


def dot(a, b, signed):
    r = 0
    for i in range(4):
        x, y = (a >> 8 * i) & 255, (b >> 8 * i) & 255
        r += (s8(x) * s8(y)) if signed else x * y
    return r


for name, C, K, signed in CASES:
    local = 40 if name.endswith('w') else 64
    N = NI // local * local
    fromw = name.endswith('u')
    bounds = name.rstrip('u')[-1] in 'bew'
    rnd = random.Random(name)
    W = [[rnd.getrandbits(16 if name.endswith('s') else 32) for _ in range(K)] for _ in range(C)]
    X = [rnd.getrandbits(32) for _ in range(N * K)]
    T = 'int' if signed else 'uint'
    fn = 'dotPacked4x8EXT' if signed else 'dotPacked4x8EXT'
    src = ['#version 460', '#extension GL_EXT_integer_dot_product : require',
           'layout(local_size_x = %d) in;' % local,
           'layout(std430, binding = 0) readonly buffer In { uint x[]; };',
           'layout(std430, binding = 1) writeonly buffer Out { %s o[]; };' % T,
           'layout(std430, binding = 2) readonly buffer Wt { uint wt[]; };',
           'void main() {', '   uint i = gl_GlobalInvocationID.x;']
    if bounds:
        src.append('   if (i < %du) {' % LIMIT)
    for k in range(K):
        src.append('   uint x%d = x[i * %du + %du];' % (k, K, k))
    init_from_x = name.endswith('i')
    for c in range(C):
        init = ('%s(x%d >> 3)' % (T, c % K)) if init_from_x else ('%s(i * %du)' % (T, c + 1))
        src.append('   %s a%d = %s;' % (T, c, init))
    nsplit = name.endswith('n')
    conds = ['(i & 3u) != 1u', '(i % 5u) != 2u', '(i % 7u) != 3u']
    for k in range(K):
        if nsplit and k % (K // 3) == 0:
            if k: src.append('   }')
            src.append('   if (%s) {' % conds[k // (K // 3)])
        for c in range(C):
            wv = ('wt[%du]' % (c * K + k)) if fromw else ('0x%08xu' % W[c][k])
            if signed:
                src.append('   a%d += dotPacked4x8EXT(int(x%d), int(%s));' % (c, k, wv))
            else:
                src.append('   a%d += dotPacked4x8EXT(x%d, %s);' % (c, k, wv))
    if nsplit:
        src.append('   }')
    for c in range(C):
        src.append('   o[i * %du + %du] = a%d;' % (C, c, c))
    if bounds:
        src.append('   }')
        if name.rstrip('u').endswith('e'):
            src.append('   else { o[i * %du] = 7; }' % C)
    src.append('}')
    open(name + '.comp', 'w').write('\n'.join(src) + '\n')
    out = []
    for i in range(N):
        for c in range(C):
            if init_from_x:
                a = X[i * K + c % K] >> 3
                a = a - (1 << 32) if signed and a >= 1 << 31 else a
            else:
                a = i * (c + 1)
            for k in range(K):
                if nsplit:
                    g = k // (K // 3)
                    if (g == 0 and (i & 3) == 1) or (g == 1 and i % 5 == 2) or (g == 2 and i % 7 == 3):
                        continue
                a += dot(X[i * K + k], W[c][k], signed)
            if bounds and i >= LIMIT:
                a = 7 if name.rstrip('u').endswith('e') and c == 0 else 0xcdcdcdcd
            out.append(a & 0xffffffff)
    open(name + '.in', 'wb').write(struct.pack('<%dI' % len(X), *X))
    open(name + '.ref', 'wb').write(struct.pack('<%dI' % len(out), *out))
    open(name + '.w', 'wb').write(struct.pack('<%dI' % (C * K), *[W[c][k] for c in range(C) for k in range(K)]))
    open(name + '.groups', 'w').write('%d\n' % (N // local))
    print(name, C, K, file=sys.stderr)
