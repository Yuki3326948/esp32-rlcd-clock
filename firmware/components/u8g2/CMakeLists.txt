file(GLOB U8G2_SRCS
    "csrc/u8g2_*.c"
    "csrc/u8x8_*.c"
)

# 只保留"算字模/画图元"的那部分,把用不到的成批剪掉。
# 这个工程里 u8g2 只负责 st7305 那层的字节/GPIO 回调,
# 界面文字全走自写的 components/ui_draw(自带字库)。
#
#   u8g2_d_setup.c  根据显示控制器型号展开的初始化表,这里不需要
#   u8x8_d_*.c      几十种屏的驱动,一个都不用
#   u8g2_fonts.c    ★ 38.4MB —— u8g2 自带的全部字体,一个都不用
#   u8x8_fonts.c    ★  1.5MB —— 同上的 8x8 点阵字体
#                   两个 fonts 文件占了整个组件 42MB 里的 40MB,
#                   删掉之后仓库小两个数量级。
#
# 想恢复官方 u8g2 全部字体时,把下面两行 fonts 的排除去掉、
# 并把文件从官方发行包里拷回来即可(同时要把 main/user_config.h 的
# APP_USE_U8G2_DEMO 打开,那里有用到几个字体做演示)。
list(FILTER U8G2_SRCS EXCLUDE REGEX ".*/u8g2_d_setup\\.c$")
list(FILTER U8G2_SRCS EXCLUDE REGEX ".*/u8x8_d_.*\\.c$")
list(FILTER U8G2_SRCS EXCLUDE REGEX ".*/u8g2_fonts\\.c$")
list(FILTER U8G2_SRCS EXCLUDE REGEX ".*/u8x8_fonts\\.c$")

idf_component_register(
    SRCS ${U8G2_SRCS}
    INCLUDE_DIRS "csrc"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC U8X8_WITH_USER_PTR)
