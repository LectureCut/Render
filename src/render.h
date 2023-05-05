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

#ifdef __cplusplus
extern "C" {
#endif

  EXPORT const char* version();

  EXPORT void init();

  struct cut
  {
    double start;
    double end;
  };

  struct cut_list
  {
    long num_cuts;
    cut* cuts;
  };

  typedef void progress_callback(const char*, double);

  EXPORT void render(
    const char *file,
    const char *output,
    cut_list cuts,
    int quality,
    progress_callback *progress
  );

#ifdef __cplusplus
}
#endif