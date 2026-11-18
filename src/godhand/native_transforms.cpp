#include "godhand/native_transforms.h"

#include "godhand/function_hooks.h"
#include "godhand/log.h"
#include "ps2_runtime_macros.h"
#include <ps2_recompiled_functions.h>

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace godhand
{
    namespace
    {
        constexpr uint32_t kVectorBytes = 16;
        constexpr uint32_t kMatrixBytes = 4 * kVectorBytes;

        struct Matrix
        {
            __m128 columns[4];
        };

        // The adapter only handles ordinary RAM. The original functions retain the
        // runtime's treatment of mapped addresses and ranges that wrap around RAM.
        inline bool isRamRange(uint32_t address, uint32_t size)
        {
            return (address & ~0xFu) <= PS2_RAM_SIZE - size;
        }

        inline __m128 loadVector(const uint8_t *ram, uint32_t address)
        {
            __m128 value;
            std::memcpy(&value, ram + (address & ~0xFu), sizeof(value));
            return value;
        }

        inline void storeVector(uint8_t *ram, uint32_t address, __m128 value)
        {
            std::memcpy(ram + (address & ~0xFu), &value, sizeof(value));
        }

        inline Matrix loadMatrix(const uint8_t *ram, uint32_t address)
        {
            Matrix value;
            std::memcpy(value.columns, ram + (address & ~0xFu), kMatrixBytes);
            return value;
        }

        inline void storeMatrix(uint8_t *ram, uint32_t address, const Matrix &matrix)
        {
            std::memcpy(ram + (address & ~0xFu), matrix.columns, kMatrixBytes);
        }

        struct Transform
        {
            __m128 value;
            __m128 partialSum;
        };

        inline Transform transformVector(const Matrix &matrix, __m128 vector)
        {
            const auto &c = matrix.columns;
            // Preserve the game's saturation and arithmetic order per lane. The
            // result and the saved partial sum must both match the original ABI.
            const __m128 x = ps2VuClampVector(_mm_mul_ps(c[0], _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(0, 0, 0, 0))));
            const __m128 y = ps2VuClampVector(_mm_mul_ps(c[1], _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(1, 1, 1, 1))));
            const __m128 z = ps2VuClampVector(_mm_mul_ps(c[2], _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(2, 2, 2, 2))));
            const __m128 w = ps2VuClampVector(_mm_mul_ps(c[3], _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(3, 3, 3, 3))));
            const __m128 xy = ps2VuClampVector(_mm_add_ps(x, y));
            const __m128 xyz = ps2VuClampVector(_mm_add_ps(xy, z));
            return {ps2VuClampVector(_mm_add_ps(xyz, w)), xyz};
        }

        inline void saveMatrixRegisters(R5900Context *ctx, const Matrix &matrix)
        {
            for (int column = 0; column < 4; ++column)
                ctx->vu0_vf[4 + column] = matrix.columns[column];
        }

        inline void saveTransformRegisters(R5900Context *ctx, __m128 input, const Transform &result)
        {
            ctx->vu0_vf[8] = input;
            ctx->vu0_vf[9] = result.value;
            ctx->vu0_acc = result.partialSum;
        }

        void applyMatrix(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = GPR_U32(ctx, 4);
            const uint32_t left = GPR_U32(ctx, 5);
            const uint32_t right = GPR_U32(ctx, 6);
            if (!isRamRange(out, kVectorBytes) || !isRamRange(left, kMatrixBytes) || !isRamRange(right, kVectorBytes))
                return sceVu0ApplyMatrix_0x30a2b0(ram, ctx, runtime);

            const Matrix matrix = loadMatrix(ram, left);
            const __m128 vector = loadVector(ram, right);
            const Transform result = transformVector(matrix, vector);
            storeVector(ram, out, result.value);
            saveMatrixRegisters(ctx, matrix);
            saveTransformRegisters(ctx, vector, result);
            ctx->pc = GPR_U32(ctx, 31);
        }

        void multiplyMatrix(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t out = GPR_U32(ctx, 4);
            const uint32_t left = GPR_U32(ctx, 5);
            uint32_t right = GPR_U32(ctx, 6);
            if (!isRamRange(out, kMatrixBytes) || !isRamRange(left, kMatrixBytes) || !isRamRange(right, kMatrixBytes))
                return sceVu0MulMatrix_0x30a2e0(ram, ctx, runtime);

            const Matrix matrix = loadMatrix(ram, left);
            saveMatrixRegisters(ctx, matrix);
            for (int remaining = 3; remaining >= 0; --remaining)
            {
                // Read each right-hand column before its corresponding write.
                // This preserves partially overlapping input/output buffers.
                const __m128 vector = loadVector(ram, right);
                const Transform result = transformVector(matrix, vector);
                storeVector(ram, out, result.value);
                out += kVectorBytes;
                right += kVectorBytes;
                if (remaining != 0 && !runtime->eeCheckpointDue())
                    continue;

                saveTransformRegisters(ctx, vector, result);
                SET_GPR_S32(ctx, 4, out);
                SET_GPR_S32(ctx, 6, right);
                SET_GPR_S32(ctx, 7, remaining);
                // The dispatcher retains the generated handler at the internal
                // resume address when an interrupt or thread switch is due.
                ctx->pc = remaining != 0 ? 0x30a2f4u : GPR_U32(ctx, 31);
                return;
            }
        }

        void transposeMatrix(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = GPR_U32(ctx, 4);
            const uint32_t in = GPR_U32(ctx, 5);
            if (!isRamRange(out, kMatrixBytes) || !isRamRange(in, kMatrixBytes))
                return sceVu0TransposeMatrix_0x30a3b0(ram, ctx, runtime);

            const Matrix matrix = loadMatrix(ram, in);
            const auto &c = matrix.columns;
            const __m128i low01 = _mm_unpacklo_epi32(_mm_castps_si128(c[0]), _mm_castps_si128(c[1]));
            const __m128i high01 = _mm_unpackhi_epi32(_mm_castps_si128(c[0]), _mm_castps_si128(c[1]));
            const __m128i low23 = _mm_unpacklo_epi32(_mm_castps_si128(c[2]), _mm_castps_si128(c[3]));
            const __m128i high23 = _mm_unpackhi_epi32(_mm_castps_si128(c[2]), _mm_castps_si128(c[3]));
            const Matrix transposed = {{_mm_castsi128_ps(_mm_unpacklo_epi64(low01, low23)),
                                        _mm_castsi128_ps(_mm_unpackhi_epi64(low01, low23)),
                                        _mm_castsi128_ps(_mm_unpacklo_epi64(high01, high23)),
                                        _mm_castsi128_ps(_mm_unpackhi_epi64(high01, high23))}};
            storeMatrix(ram, out, transposed);
            for (int column = 0; column < 4; ++column)
                ctx->r[8 + column] = _mm_castps_si128(transposed.columns[column]);
            ctx->r[12] = low01;
            ctx->r[13] = high01;
            ctx->r[14] = low23;
            ctx->r[15] = high23;
            ctx->pc = GPR_U32(ctx, 31);
        }

        void copyMatrix(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = GPR_U32(ctx, 4);
            const uint32_t in = GPR_U32(ctx, 5);
            if (!isRamRange(out, kMatrixBytes) || !isRamRange(in, kMatrixBytes))
                return sceVu0CopyMatrix_0x30a548(ram, ctx, runtime);

            const Matrix matrix = loadMatrix(ram, in);
            storeMatrix(ram, out, matrix);
            for (int column = 0; column < 4; ++column)
                ctx->r[6 + column] = _mm_castps_si128(matrix.columns[column]);
            ctx->pc = GPR_U32(ctx, 31);
        }

        void identityMatrix(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = GPR_U32(ctx, 4);
            if (!isRamRange(out, kMatrixBytes))
                return sceVu0UnitMatrix_0x30a5a0(ram, ctx, runtime);

            const Matrix identity = {{_mm_setr_ps(1, 0, 0, 0), _mm_setr_ps(0, 1, 0, 0),
                                      _mm_setr_ps(0, 0, 1, 0), _mm_setr_ps(0, 0, 0, 1)}};
            storeMatrix(ram, out, identity);
            for (int column = 0; column < 4; ++column)
                ctx->vu0_vf[7 - column] = identity.columns[column];
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void installNativeTransforms()
    {
        // Keep instruction-level return auditing available in diagnostic builds.
#if !defined(PS2X_STRICT_RETURN_DIAGNOSTICS) || !PS2X_STRICT_RETURN_DIAGNOSTICS
        const char *setting = std::getenv("GODHAND_NATIVE_TRANSFORMS");
        if (setting && std::string_view(setting) == "0")
            return;

        constexpr struct
        {
            uint32_t address;
            const char *name;
            GuestFunction implementation;
        } bindings[] = {
            {0x30a2b0, "sceVu0ApplyMatrix", applyMatrix},
            {0x30a2e0, "sceVu0MulMatrix", multiplyMatrix},
            {0x30a3b0, "sceVu0TransposeMatrix", transposeMatrix},
            {0x30a548, "sceVu0CopyMatrix", copyMatrix},
            {0x30a5a0, "sceVu0UnitMatrix", identityMatrix},
        };

        for (const auto &binding : bindings)
        {
            if (!FunctionHooks::instance().replace(binding.address, binding.name, binding.implementation,
                                                   ReplacementStatus::Native, "x64 SIMD matrix operations"))
                GH_WARN(Hook, "could not install native transform " << binding.name);
        }
#endif
    }
}
