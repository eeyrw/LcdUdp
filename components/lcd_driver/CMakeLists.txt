idf_component_register(
    SRCS "lcd_hd44780.c" "lcd_task.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos log
)
