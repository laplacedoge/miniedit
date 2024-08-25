cmake_minimum_required(VERSION 3.15)

set(BINARY_NAME miniedit)

project(miniedit LANGUAGES C)

add_compile_options(-g -O0)

add_executable(${BINARY_NAME} main.c)

