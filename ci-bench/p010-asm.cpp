#include <array>
#include <utility>
#include <utils/VectorizedDecoders.h>
void p010(uint32_t* y, uint32_t* uv, const uint8_t* lut, uint8_t* d, const uint8_t* e) { VECTOR_P010::process<false>(y, uv, lut, d, e); }
void p010q(uint32_t* y, uint32_t* uv, const uint8_t* lut, uint8_t* d, const uint8_t* e) { VECTOR_P010::process<true>(y, uv, lut, d, e); }
void p010t(uint64_t* y, uint64_t* uv, const uint8_t* lut, uint8_t* d, const uint8_t* e, const std::array<uint8_t,1024>& a, const std::array<uint8_t,1024>& b) { VECTOR_P010::processlWithToneMapping<false>(y, uv, lut, d, e, a, b); }
