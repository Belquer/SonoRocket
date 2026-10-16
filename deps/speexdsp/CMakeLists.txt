add_library(speexdsp_talk STATIC
    libspeexdsp/mdf.c libspeexdsp/preprocess.c libspeexdsp/filterbank.c
    libspeexdsp/fftwrap.c libspeexdsp/kiss_fft.c libspeexdsp/kiss_fftr.c libspeexdsp/resample.c)
target_include_directories(speexdsp_talk PUBLIC include PRIVATE libspeexdsp)
target_compile_definitions(speexdsp_talk PRIVATE FLOATING_POINT USE_KISS_FFT EXPORT=)
set_target_properties(speexdsp_talk PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(speexdsp_talk PRIVATE /wd4244 /wd4305 /wd4267)
endif()
