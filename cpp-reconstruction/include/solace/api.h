#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# if defined(SOLACE_CORE_EXPORTS)
#  define SOLACE_API __declspec(dllexport)
# else
#  define SOLACE_API __declspec(dllimport)
# endif
#else
# define SOLACE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Independent recovered-core ABI. This is not the original Solace DLL ABI. */
typedef struct solace_asset_view {
    size_t structure_size;
    const char* path;
    size_t path_size;
    const unsigned char* data;
    size_t data_size;
    const char* sha256;
    size_t sha256_size;
    uint64_t original_rva;
    int is_template;
} solace_asset_view;

SOLACE_API const char* solace_core_version(void);
SOLACE_API size_t solace_asset_count(void);
/* Returns 0 on success, 1 for an invalid argument. Views live until DLL unload. */
SOLACE_API int solace_get_asset(size_t index, solace_asset_view* output);
SOLACE_API int solace_field_key(const char* category, size_t category_size,
                                const char* name, size_t name_size, uint32_t* output);
/* Returns 0 on success, 1 for an invalid argument, 2 for an operation error.
   Error text is copied to caller storage and is always NUL terminated if capacity>0. */
SOLACE_API int solace_validate_config(const char* text, size_t size,
    size_t* field_count, char* error, size_t error_capacity);
SOLACE_API int solace_run_lua(const char* text, size_t size, char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
