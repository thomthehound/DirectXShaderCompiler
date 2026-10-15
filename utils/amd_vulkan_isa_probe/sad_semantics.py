#!/usr/bin/env python3
"""Verify the APUSR optical-flow SAD/QSad semantic contracts on the CPU.

The important distinction is intentional:
* ordinary packed SAD includes zero-valued bytes;
* HLSL msad4 masks a difference when the reference byte is zero;
* FidelityFX clamps optical-flow luma bytes to at least 1 on the msad4 path,
  so the two forms are equivalent for the inputs that reach that path.
"""

from __future__ import annotations

import argparse
import random

U32_MASK = 0xFFFFFFFF


def u32(value: int) -> int:
    return value & U32_MASK


def unpack_u8x4(value: int) -> tuple[int, int, int, int]:
    return tuple((value >> (8 * i)) & 0xFF for i in range(4))  # type: ignore[return-value]


def pack_u8(values: tuple[int, ...] | list[int]) -> int:
    result = 0
    for i, value in enumerate(values):
        result |= (value & 0xFF) << (8 * i)
    return u32(result)


def scalar_sad4(a: int, b: int) -> int:
    return sum(abs(x - y) for x, y in zip(unpack_u8x4(a), unpack_u8x4(b)))


def apusr_packed_absdiff(a: int, b: int) -> int:
    """Bit-for-bit model of ApusrPackedByteAbsDiff."""
    high = 0x80808080
    low = 0x7F7F7F7F
    xor_value = u32(a ^ b)
    a_high = a & high
    high_diff = xor_value & high
    low_ge = u32(((a & low) | high) - (b & low)) & high
    ge_bit = low_ge ^ (high_diff & (low_ge ^ a_high))
    ge_mask = u32((ge_bit >> 7) * 0xFF)
    swap = xor_value & ge_mask
    max_value = u32(b ^ swap)
    min_value = u32(a ^ swap)
    return u32(max_value - min_value)


def apusr_packed_sad(a: int, b: int) -> int:
    return sum(unpack_u8x4(apusr_packed_absdiff(a, b)))


def q_sad_fallback(source0: int, source1: int, reference: int) -> tuple[int, int, int, int]:
    """Model the rolling-window QSad fallback used by APUSR/FFX."""
    a0 = source0
    a1 = source1
    result: list[int] = []
    for lane in range(4):
        result.append(apusr_packed_sad(a0, reference))
        if lane != 3:
            a0 = u32((a0 >> 8) | ((a1 & 0xFF) << 24))
            a1 >>= 8
    return tuple(result)  # type: ignore[return-value]


def q_sad_oracle(source0: int, source1: int, reference: int) -> tuple[int, int, int, int]:
    source = list(unpack_u8x4(source0)) + list(unpack_u8x4(source1))
    ref = unpack_u8x4(reference)
    return tuple(sum(abs(ref[i] - source[lane + i]) for i in range(4)) for lane in range(4))  # type: ignore[return-value]


def msad4_oracle(reference: int, source0: int, source1: int,
                 accum: tuple[int, int, int, int] = (0, 0, 0, 0)) -> tuple[int, int, int, int]:
    source = list(unpack_u8x4(source0)) + list(unpack_u8x4(source1))
    ref = unpack_u8x4(reference)
    result = []
    for lane in range(4):
        total = accum[lane]
        for i, ref_byte in enumerate(ref):
            if ref_byte != 0:
                total += abs(ref_byte - source[lane + i])
        result.append(total)
    return tuple(result)  # type: ignore[return-value]


def clamp_nonzero_luma(value: int) -> int:
    return pack_u8(tuple(max(1, byte) for byte in unpack_u8x4(value)))


def random_packed(rng: random.Random, *, nonzero: bool) -> int:
    lo = 1 if nonzero else 0
    return pack_u8(tuple(rng.randint(lo, 255) for _ in range(4)))


def apusr_probe_result(
    source0: int, source1: int, source2: int,
    reference0: int, reference1: int, salt: int, *, masked: bool
) -> tuple[int, int, int, int]:
    """Model the complete eight-row workload in both ISA comparison shaders."""
    perturb = u32(salt)  # The probe's dispatched invocation has x == 0.
    a0 = u32(source0 ^ perturb)
    a1 = u32(source1 + perturb)
    a2 = u32(source2 ^ u32(perturb << 8))
    p0 = reference0
    p1 = reference1
    sad = (0, 0, 0, 0)

    for _ in range(8):
        if masked:
            sad = msad4_oracle(p0, a0, a1, sad)
            sad = msad4_oracle(p1, a1, a2, sad)
        else:
            left = q_sad_fallback(a0, a1, p0)
            right = q_sad_fallback(a1, a2, p1)
            sad = tuple(sad[i] + left[i] + right[i] for i in range(4))

        a0 = u32((a0 >> 1) | (a0 << 31))
        a1 = u32(a1 ^ 0x9E3779B9)
        a2 = u32(a2 + 0x7F4A7C15)
        # Full-byte rotations cannot introduce a zero reference byte.
        p0 = u32((p0 >> 8) | (p0 << 24))
        p1 = u32((p1 >> 24) | (p1 << 8))

    return sad



def verify(samples: int, seed: int) -> None:
    rng = random.Random(seed)

    edges = (
        0x00000000, 0xFFFFFFFF, 0x01010101, 0x80808080,
        0x7F7F7F7F, 0x00FF0080, 0xFF00FF7F, 0x01234567,
        0x89ABCDEF,
    )
    for a in edges:
        for b in edges:
            packed = apusr_packed_sad(a, b)
            scalar = scalar_sad4(a, b)
            if packed != scalar:
                raise AssertionError(
                    f"packed SAD mismatch a=0x{a:08x} b=0x{b:08x}: {packed} != {scalar}"
                )

    for _ in range(samples):
        a = random_packed(rng, nonzero=False)
        b = random_packed(rng, nonzero=False)
        packed = apusr_packed_sad(a, b)
        scalar = scalar_sad4(a, b)
        if packed != scalar:
            raise AssertionError(
                f"random packed SAD mismatch a=0x{a:08x} b=0x{b:08x}: {packed} != {scalar}"
            )

        source0 = random_packed(rng, nonzero=False)
        source1 = random_packed(rng, nonzero=False)
        reference = random_packed(rng, nonzero=False)
        fallback = q_sad_fallback(source0, source1, reference)
        oracle = q_sad_oracle(source0, source1, reference)
        if fallback != oracle:
            raise AssertionError(
                "QSad rolling-window mismatch: "
                f"src0=0x{source0:08x} src1=0x{source1:08x} ref=0x{reference:08x}: "
                f"{fallback} != {oracle}"
            )

        # This is the real fast-path contract. FFX clamps every luma byte to at
        # least one before packing whenever FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION
        # is active, so msad4's zero-reference-byte mask cannot alter the result.
        source0_nz = clamp_nonzero_luma(source0)
        source1_nz = clamp_nonzero_luma(source1)
        reference_nz = clamp_nonzero_luma(reference)
        fallback_nz = q_sad_fallback(source0_nz, source1_nz, reference_nz)
        native_shape = msad4_oracle(reference_nz, source0_nz, source1_nz)
        if fallback_nz != native_shape:
            raise AssertionError(
                "nonzero-luma msad4/QSad mismatch: "
                f"src0=0x{source0_nz:08x} src1=0x{source1_nz:08x} ref=0x{reference_nz:08x}: "
                f"{fallback_nz} != {native_shape}"
            )

        # Verify the entire shader recurrence, not just one QSad call. The
        # previous probe changed reference bytes with XOR/add, allowing zeros
        # even when the supplied luma was preclamped.
        if _ % 32 == 0:
            source2_nz = clamp_nonzero_luma(random_packed(rng, nonzero=False))
            reference1_nz = clamp_nonzero_luma(random_packed(rng, nonzero=False))
            salt = rng.getrandbits(32)
            params = (
                source0_nz, source1_nz, source2_nz,
                reference_nz, reference1_nz, salt,
            )
            full_swar = apusr_probe_result(*params, masked=False)
            full_msad = apusr_probe_result(*params, masked=True)
            if full_swar != full_msad:
                raise AssertionError(
                    "eight-row APUSR ISA-probe mismatch: "
                    f"inputs={params!r} swar={full_swar} msad={full_msad}"
                )

    # Guard the reason for the nonzero-luma invariant itself. Without the clamp,
    # ordinary SAD and msad4 are deliberately not equivalent when a reference
    # byte is zero.
    reference = 0x04030001
    source0 = 0x08070605
    source1 = 0x0C0B0A09
    ordinary = q_sad_oracle(source0, source1, reference)
    masked = msad4_oracle(reference, source0, source1)
    if ordinary == masked:
        raise AssertionError("zero-reference control did not expose msad4 masking")

    probe_args = (0x01010101, 0x02020202, 0x03030303,
                  0x00000000, 0x04040404, 0)
    if (apusr_probe_result(*probe_args, masked=False) ==
            apusr_probe_result(*probe_args, masked=True)):
        raise AssertionError("eight-row zero-reference control did not expose masking")

    print(f"PASS packed SWAR SAD == scalar SAD ({samples} random cases + edge corpus)")
    print(f"PASS rolling QSad == byte-window oracle ({samples} random cases)")
    print(f"PASS clamped-nonzero QSad == msad4 semantics ({samples} random cases)")
    print(f"PASS eight-row APUSR MSAD/SWAR equivalence ({(samples + 31) // 32} random cases)")
    print("PASS zero-reference control proves the luma clamp is semantically required")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=100_000)
    parser.add_argument("--seed", type=lambda x: int(x, 0), default=0xA5AD4)
    args = parser.parse_args()
    verify(args.samples, args.seed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
