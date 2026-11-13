idf_component_register(
    SRCS "kode_bq25896.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES "driver" "cpp_bus_driver"
)

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})