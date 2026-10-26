idf_component_register(SRCS "Photos.cpp" "photo_decoder.cpp" "assets/img_app_gallery.c" INCLUDE_DIRS "." REQUIRES app_support)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
