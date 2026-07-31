cmake_minimum_required(VERSION 3.13)

# Pico SDKのインポート（プロジェクト内に pico_sdk_import.cmake がコピーされている前提です）
include(pico_sdk_import.cmake)

# プロジェクト名（任意に変更可能です）
project(g5n_switcher C CXX ASM)

# Pico SDKの初期化
pico_sdk_init()

# 実行ファイルの追加（ソースコードが main.cpp の場合）
add_executable(g5n_switcher
    main.cpp
)

# tusb_config.h を見つけるために、現在のディレクトリをインクルードパスに追加
target_include_directories(g5n_switcher PRIVATE .)

# 必要なライブラリのリンク
# ★重要：ここで tinyusb_host と tinyusb_board を指定します
target_link_libraries(g5n_switcher
    pico_stdlib
    hardware_gpio
    hardware_timer
    hardware_flash
    hardware_sync
    tinyusb_host
    tinyusb_board
)

# ---------------------------------------------------------
# ★超重要設定：USBホスト機能とUSBシリアルの競合を回避する
# ---------------------------------------------------------
# PicoをUSBホスト（親）にするため、PCと繋ぐためのUSBデバイス機能(stdio)をOFFにします。
# これを 0 にしないとコンパイルエラーになります。
pico_enable_stdio_usb(g5n_switcher 0)

# 代わりにUART（ハードウェアシリアル）での出力を有効にします。
pico_enable_stdio_uart(g5n_switcher 1)

# GPIO0/1はMUTE制御(PIN_MUTE)と衝突するため、UARTピンを未使用のGPIO28(TX)/29(RX)に変更
target_compile_definitions(g5n_switcher PRIVATE
    PICO_DEFAULT_UART_TX_PIN=28
    PICO_DEFAULT_UART_RX_PIN=29
)

# .uf2 ファイルを生成するコマンド
pico_add_extra_outputs(g5n_switcher)