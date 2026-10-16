// Standalone CPU calibration for Unicorn's arithmetic NaN propagation.
// Build separately with MSVC /Od /fp:strict /FAs; inspect SUBSS/MULSS/ADDSS order.
// This probe does not link the reconstruction library or load the original DLL.
#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <xmmintrin.h>
#include <emmintrin.h>

__declspec(noinline) std::uint32_t native_lerp(
    std::uint32_t first_bits, std::uint32_t last_bits, std::uint32_t amount_bits) {
    const __m128 first = _mm_castsi128_ps(_mm_cvtsi32_si128(std::bit_cast<std::int32_t>(first_bits)));
    const __m128 last = _mm_castsi128_ps(_mm_cvtsi32_si128(std::bit_cast<std::int32_t>(last_bits)));
    const __m128 amount = _mm_castsi128_ps(_mm_cvtsi32_si128(std::bit_cast<std::int32_t>(amount_bits)));
    const __m128 difference = _mm_sub_ss(last, first);
    const __m128 scaled = _mm_mul_ss(difference, amount);
    const __m128 result = _mm_add_ss(scaled, first);
    return std::bit_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(result)));
}

__declspec(noinline) std::uint32_t native_multiply(std::uint32_t left_bits, std::uint32_t right_bits) {
    const __m128 left = _mm_castsi128_ps(_mm_cvtsi32_si128(std::bit_cast<std::int32_t>(left_bits)));
    const __m128 right = _mm_castsi128_ps(_mm_cvtsi32_si128(std::bit_cast<std::int32_t>(right_bits)));
    return std::bit_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(_mm_mul_ss(left, right))));
}

__declspec(noinline) std::uint32_t native_remap(std::uint32_t value_bits,
    std::uint32_t first_bits, std::uint32_t last_bits,
    std::uint32_t out_first_bits, std::uint32_t out_last_bits, bool clamped) {
    const __m128 value = _mm_set_ss(std::bit_cast<float>(value_bits));
    const __m128 first = _mm_set_ss(std::bit_cast<float>(first_bits));
    const __m128 last = _mm_set_ss(std::bit_cast<float>(last_bits));
    const __m128 out_first = _mm_set_ss(std::bit_cast<float>(out_first_bits));
    const __m128 out_last = _mm_set_ss(std::bit_cast<float>(out_last_bits));
    __m128 amount = _mm_setzero_ps();
    if (_mm_movemask_ps(_mm_cmpneq_ss(last, first)) & 1) {
        const __m128 numerator = _mm_sub_ss(value, first);
        const __m128 denominator = _mm_sub_ss(last, first);
        amount = _mm_div_ss(numerator, denominator);
    }
    if (clamped) {
        const __m128 nonnegative = _mm_max_ss(_mm_setzero_ps(), amount);
        const __m128 one = _mm_set_ss(1.0f);
        const __m128 above_one = _mm_cmplt_ss(one, amount);
        amount = _mm_or_ps(_mm_and_ps(above_one, one), _mm_andnot_ps(above_one, nonnegative));
    }
    const __m128 difference = _mm_sub_ss(out_last, out_first);
    const __m128 scaled = _mm_mul_ss(difference, amount);
    const __m128 result = _mm_add_ss(scaled, out_first);
    return std::bit_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(result)));
}

int main(int argc, char**) {
    _mm_setcsr(0x1F80);
    if (argc > 1) {
        std::string operation;
        while (std::cin >> operation) {
            std::uint32_t first{}, second{}, third{}, fourth{}, fifth{};
            if (!(std::cin >> first >> second >> third)) return 2;
            std::uint32_t bits{};
            if (operation == "Lerp") bits = native_lerp(first, second, third);
            else if (operation == "RemapVal" || operation == "RemapValClamped") {
                if (!(std::cin >> fourth >> fifth)) return 2;
                bits = native_remap(first, second, third, fourth, fifth, operation == "RemapValClamped");
            } else return 3;
            std::cout << "{\"bits\":" << bits << "}\n";
        }
        return 0;
    }
    const auto lerp = native_lerp(0x7F800000, 0x7F800000, 0x7FC12345);
    const auto multiply = native_multiply(0xFFC00000, 0x7FC12345);
    const auto reversed = native_multiply(0x7FC12345, 0xFFC00000);
    std::cout << "{\"lerp_bits\":" << lerp << ",\"multiply_bits\":" << multiply
              << ",\"reversed_multiply_bits\":" << reversed << "}\n";
}
