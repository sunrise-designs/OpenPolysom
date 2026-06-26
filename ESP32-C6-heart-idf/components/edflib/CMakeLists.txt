idf_component_register(
    SRCS "edflib.c"
    INCLUDE_DIRS "."
)

# edflib uses POSIX file I/O which goes through ESP-IDF VFS — no extra deps needed
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-result)
