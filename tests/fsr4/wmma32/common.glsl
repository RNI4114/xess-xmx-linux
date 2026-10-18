#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_8bit_storage : require
#extension GL_EXT_shader_16bit_storage : require
// One subgroup per workgroup (local size = required subgroup size, spec constant 0), as D3D12 WMMA in wave32.
layout(local_size_x_id = 0) in;
#define CM(T, U) coopmat<T, gl_ScopeSubgroup, 16, 16, U>
#define RM gl_CooperativeMatrixLayoutRowMajor
#define CMJ gl_CooperativeMatrixLayoutColumnMajor
