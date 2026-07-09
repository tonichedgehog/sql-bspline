add_library(dierckx STATIC
  cualde.f
  curev.f
  fpader.f
  fpback.f
  fpbspl.f
  fpchec.f
  fpdisc.f
  fpgivs.f
  fpknot.f
  fppara.f
  fprati.f
  fprota.f
  parcur.f
  )
set_target_properties(dierckx PROPERTIES
  Fortran_VISIBILITY_PRESET hidden
  )
target_include_directories(dierckx INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )
