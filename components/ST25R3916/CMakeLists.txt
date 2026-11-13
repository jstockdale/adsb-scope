set(src_dirs ./src/)
idf_component_register(
        SRC_DIRS 
        ${src_dirs} 
        INCLUDE_DIRS 
        ${src_dirs}
        REQUIRES
        "arduino_cpp_bus_driver" "NFC-RFAL"
    )

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE -Wno-error=unused-const-variable -Wno-error=delete-non-virtual-dtor
            -Wno-error=implicit-function-declaration -fpermissive)