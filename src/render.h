#pragma once

#if defined(_MSC_VER)
  //  Microsoft 
  #define EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
  //  GCC
  #define EXPORT __attribute__((visibility("default")))
#else
  #define EXPORT
  #pragma warning Unknown dynamic link export semantics.
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef void error_callback(const char*);
  
  EXPORT const char *version(error_callback* error);

  EXPORT void init(error_callback* error);

  struct cut
  {
    int64_t start;
    int64_t end;
  };

  struct cut_list
  {
    long num_cuts;
    cut* cuts;
  };

  typedef void progress_callback(const char*, double);

  struct ArgumentResult {
    const char* name;
    const char* value;
  };

  struct ArgumentResultList {
    long num_args;
    ArgumentResult* args;
  };

  EXPORT void render(
    const char *file,
    const char *output,
    cut_list cuts,
    ArgumentResultList args,
    progress_callback *progress,
    error_callback *error
  );

  struct Argument {
    const char short_name;
    const char* long_name;
    const char* description;
    bool required;
    bool is_flag;
  };

  struct ArgumentList {
    long num_args;
    Argument* args;
  };

  EXPORT ArgumentList get_arguments(error_callback* error);

#ifdef __cplusplus
}
#endif