idf_component_register(SRCS "sentio.cpp" "sentio_sd.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esphome core lvgl
                       PRIV_REQUIRES fatfs sdmmc driver spi_flash vfs)