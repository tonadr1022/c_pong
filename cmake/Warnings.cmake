function(add_project_warnings out_target)
  add_library(${out_target} INTERFACE)

  target_compile_options(${out_target} INTERFACE
      # C (GNU/Clang)
      $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,Clang,GNU,AppleClang>>:-Wall;-Wextra;-pedantic>
      # C++ (GNU/Clang)
      $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,Clang,GNU,AppleClang>>:-Wall;-Wextra;-pedantic-errors>
      # MSVC (C)
      $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:C,MSVC>>:/W4;/WX>
      # MSVC (C++)
      $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,MSVC>>:/W4;/WX>
  )
endfunction()
