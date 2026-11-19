# adsb-scope: single-application build. The upstream LilyGo tree selected
# among ~30 examples here; this repo builds only the ADS-B Scope app.
#
# The path is set unconditionally on purpose: ESP-IDF evaluates this file
# once before any configuration exists (component requirement expansion),
# so it must not depend on - or fail on - a CONFIG_ variable. The Kconfig
# symbol EXAMPLE_BUILD_ADSB_RECEIVER remains for the sources that test it.
set(project_path "examples/adsb_receiver/")

idf_component_register(SRC_DIRS ${project_path}
                        INCLUDE_DIRS ${project_path}
                        )

# Embed the webapp (served from device + used by the serial console `webapp` cmd)
if(EXISTS "${COMPONENT_DIR}/${project_path}/adsb_scope.htm")
    target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/${project_path}/adsb_scope.htm" TEXT)
endif()

set_source_files_properties(tuner_r82xx.c "class_driver.c"
    PROPERTIES COMPILE_FLAGS
    "-Wno-unused-function -Wno-unused-variable -Wno-incompatible-pointer-types"
)

# The decoder runs on every sample, 2.4 million a second, and is compiled for
# speed whatever the project's level is (it is -Og, for the debugger). At
# -Og the same code takes about 30 % more instructions, which is the
# difference between a decoder that has a core to itself with room to spare
# and one that is close to filling it (docs/DECODER.md section 5.9).
set_source_files_properties(
    "${COMPONENT_DIR}/${project_path}mode-s.c"
    "${COMPONENT_DIR}/${project_path}mode_s_2400.c"
    "${COMPONENT_DIR}/${project_path}mode_s_coh.c"
    PROPERTIES COMPILE_OPTIONS "-O2")

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE -Wno-error=unused-const-variable -Wno-error=delete-non-virtual-dtor
            -Wno-error=implicit-function-declaration -fpermissive)
