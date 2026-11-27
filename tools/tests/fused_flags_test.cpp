// Checks the recompiler's fused condition codes (aotrt::fc_*) and flag-free
// forms against the interpreter's lazy-flag semantics (cpu::alu/inc_dec/shift +
// cpu::cond). Built with the NDK and run on the device by tools/run_tests.ps1.
#include <cstdio>
#include <random>

#include "cpu/aot_rt.h"

namespace mem {
uint8_t* base = nullptr;
}

int main() {
    std::mt19937 rng(1234);
    const uint32_t edge[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu};
    long checks = 0, fails = 0;
    auto value = [&](int i) { return i < 12 ? edge[i] : (uint32_t)rng(); };
    for (int sz : {1, 2, 4}) {
        const uint32_t m = cpu::size_mask((uint8_t)sz);
        for (int i = 0; i < 12 + 3000; ++i) {
            for (int j = 0; j < 12 + 30; ++j) {
                uint32_t a = value(i) & m, b = value(j) & m;
                for (int op : {0, 1, 4, 5, 6, 7}) {
                    cpu::Cpu c{};
                    uint32_t r = cpu::alu(c, op, a, b, sz);
                    uint32_t rnf = aotrt::alu_nf(op, a, b, sz);
                    ++checks;
                    if (r != rnf) { ++fails; if (fails < 20) printf("alu_nf op%d sz%d %x %x\n", op, sz, a, b); }
                    for (int cc = 0; cc < 16; ++cc) {
                        bool want = cpu::cond(c, cc);
                        bool got = op == 0 ? aotrt::fc_add(cc, a, b, r, sz)
                                 : (op == 5 || op == 7) ? aotrt::fc_sub(cc, a, b, r, sz)
                                 : aotrt::fc_logic(cc, r, sz);
                        ++checks;
                        if (want != got) {
                            ++fails;
                            if (fails < 20) printf("fc op%d cc%d sz%d a=%x b=%x want %d\n", op, cc, sz, a, b, want);
                        }
                    }
                }
                for (bool dec : {false, true}) {
                    cpu::Cpu c{};
                    uint32_t r = cpu::inc_dec(c, a, dec, sz);
                    for (int cc = 0; cc < 16; ++cc) {
                        if (cc == 2 || cc == 3 || cc == 6 || cc == 7) continue;
                        ++checks;
                        if (cpu::cond(c, cc) != aotrt::fc_incdec(cc, dec, a, r, sz)) {
                            ++fails;
                            if (fails < 20) printf("incdec dec%d cc%d sz%d a=%x\n", dec, cc, sz, a);
                        }
                    }
                }
                for (int op : {4, 5, 7}) {
                    uint32_t cnt = (b & 0x1F) ? (b & 0x1F) : 1;
                    cpu::Cpu c{};
                    ++checks;
                    if (cpu::shift(c, op, a, cnt, sz) != aotrt::shift_nf(op, a, cnt, sz)) {
                        ++fails;
                        if (fails < 20) printf("shift op%d sz%d a=%x cnt=%u\n", op, sz, a, cnt);
                    }
                }
            }
        }
    }
    printf("%ld checks, %ld failures\n", checks, fails);
    return fails ? 1 : 0;
}
