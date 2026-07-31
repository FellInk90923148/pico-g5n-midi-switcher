#include <stdio.h>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#if __has_include("bsp/board_api.h")
#include "bsp/board_api.h"
#else
#include "bsp/board.h"
#endif
#include "tusb.h"

// ----------------------------------------------------
// ピン定義
// ----------------------------------------------------
const uint PIN_MUTE   = 0;
const uint PIN_REMOTE = 10;
const uint PIN_L5 = 11, PIN_L4 = 12, PIN_L3 = 13, PIN_L2 = 14, PIN_L1 = 15;
const uint PIN_SW3 = 16, PIN_LED3 = 17;
const uint PIN_SW2 = 18, PIN_LED2 = 19;
const uint PIN_SW1 = 20, PIN_LED1 = 21;

const uint RELAY_PINS[6] = {PIN_L1, PIN_L2, PIN_L3, PIN_L4, PIN_L5, PIN_REMOTE};
const uint SW_PINS[3]    = {PIN_SW1, PIN_SW2, PIN_SW3};
const uint LED_PINS[3]   = {PIN_LED1, PIN_LED2, PIN_LED3};

// フットスイッチは内部プルアップ（押すとLOWになる）
#define IS_PRESSED(pin) (gpio_get((pin)))

// --- 追加：MIDIデバイス管理 ---
uint8_t g5n_dev_addr = 0;
bool is_g5n_mounted = false;
void send_g5n_midi(); // ← 追加(前方宣言)

// --- 追加：tinyusbの自動MIDIクラス判定をバイパスして手動でインターフェースを掴む場合用 ---
uint8_t g5n_midi_itf_num = 0xFF;
uint8_t g5n_midi_ep_out = 0;
uint8_t g5n_midi_ep_in = 0;
bool g5n_midi_manual_ready = false;
bool claim_midi_interface_manually(uint8_t daddr);
void send_g5n_sysex_handshake_manual();
void send_g5n_midi_manual();

// ----------------------------------------------------
// データ構造と状態管理
// ----------------------------------------------------
enum Mode { MODE_PATCH, MODE_BANK, MODE_PROGRAM };
Mode current_mode = MODE_PATCH;

int current_bank = 0;  // 0 〜 26
int current_patch = 1; // 1 〜 5
int edit_loop_idx = 0; // 0:L1, 1:L2, 2:L3, 3:L4, 4:L5, 5:Remote

// [Bank][Patch][Loop/Remote] のON/OFFデータ (RAM保持)
bool patch_data[27][5][6] = {false};

// アニメーション用変数
bool trigger_bank_anim = false;
uint32_t anim_start_time = 0;
int anim_step = 0;

// ----------------------------------------------------
// ハードウェア制御関数
// ----------------------------------------------------
void init_hardware() {
    stdio_init_all();
    board_init();
    tusb_init();

    // MUTE
    gpio_init(PIN_MUTE);
    gpio_set_dir(PIN_MUTE, GPIO_OUT);
    gpio_put(PIN_MUTE, false);

    // Relays
    for (int i = 0; i < 6; i++) {
        gpio_init(RELAY_PINS[i]);
        gpio_set_dir(RELAY_PINS[i], GPIO_OUT);
        gpio_put(RELAY_PINS[i], false);
    }

    // LEDs
    for (int i = 0; i < 3; i++) {
        gpio_init(LED_PINS[i]);
        gpio_set_dir(LED_PINS[i], GPIO_OUT);
        gpio_put(LED_PINS[i], false);
    }

    // Switches
    for (int i = 0; i < 3; i++) {
        gpio_init(SW_PINS[i]);
        gpio_set_dir(SW_PINS[i], GPIO_IN);
        gpio_pull_up(SW_PINS[i]);
    }
}

void set_leds(bool l1, bool l2, bool l3) {
    gpio_put(PIN_LED1, l1);
    gpio_put(PIN_LED2, l2);
    gpio_put(PIN_LED3, l3);
}

// ----------------------------------------------------
// tinyusbの自動MIDIクラス判定をバイパスし、MIDIStreamingインターフェース(class=1,subclass=3)を
// 自分で見つけて低レベルAPI(tuh_edpt_open/tuh_edpt_xfer)で直接掴む
// ----------------------------------------------------
bool claim_midi_interface_manually(uint8_t daddr) {
    static uint8_t desc_buf[1024];
    tusb_xfer_result_t res = tuh_descriptor_get_configuration_sync(daddr, 0, desc_buf, sizeof(desc_buf));
    if (res != XFER_RESULT_SUCCESS) return false;

    uint16_t total_len = (uint16_t)desc_buf[2] | ((uint16_t)desc_buf[3] << 8);
    if (total_len > sizeof(desc_buf)) total_len = sizeof(desc_buf);
    uint16_t offset = desc_buf[0];

    bool in_midi_itf = false;
    g5n_midi_ep_out = 0;
    g5n_midi_ep_in = 0;

    while (offset + 2 <= total_len) {
        uint8_t bLength = desc_buf[offset];
        uint8_t bDescType = desc_buf[offset + 1];
        if (bLength == 0) break;

        if (bDescType == 0x04 && offset + 7 < total_len) { // INTERFACE
            uint8_t itf_num = desc_buf[offset + 2];
            uint8_t itf_class = desc_buf[offset + 5];
            uint8_t itf_subclass = desc_buf[offset + 6];
            in_midi_itf = (itf_class == 0x01 && itf_subclass == 0x03);
            if (in_midi_itf) g5n_midi_itf_num = itf_num;
        }
        else if (bDescType == 0x05 && in_midi_itf && offset + 6 < total_len) { // ENDPOINT
            uint8_t ep_addr = desc_buf[offset + 2];
            uint8_t ep_attr = desc_buf[offset + 3];
            if ((ep_attr & 0x03) == 0x02) { // バルク転送のみ対象
                if (tuh_edpt_open(daddr, (tusb_desc_endpoint_t const*)(desc_buf + offset))) {
                    if (ep_addr & 0x80) g5n_midi_ep_in = ep_addr;
                    else g5n_midi_ep_out = ep_addr;
                }
            }
        }
        offset += bLength;
    }

    return (g5n_midi_ep_out != 0);
}

// バルク転送完了時に呼ばれる(特にすることはない)
void midi_manual_xfer_cb(tuh_xfer_t* xfer) {
    (void)xfer;
}

// USB-MIDIイベントパケット(4バイト単位)にまとめたバイト列をそのままバルクOUTへ送る
bool send_usb_midi_packets(uint8_t daddr, uint8_t ep_out, uint8_t* packets, uint32_t len) {
    static uint8_t xfer_buf[64];
    if (len > sizeof(xfer_buf)) return false;
    memcpy(xfer_buf, packets, len);

    tuh_xfer_t xfer = {0};
    xfer.daddr = daddr;
    xfer.ep_addr = ep_out;
    xfer.buflen = len;
    xfer.buffer = xfer_buf;
    xfer.complete_cb = midi_manual_xfer_cb;
    xfer.user_data = 0;
    return tuh_edpt_xfer(&xfer);
}

// Zoom機器特有の起動リクエスト(SysEx)を手動パスで送信
void send_g5n_sysex_handshake_manual() {
    if (!g5n_midi_manual_ready) return;
    // F0 7E 00 06 01 F7 を2つのUSB-MIDIパケットに分割
    uint8_t packets[8] = {
        0x04, 0xF0, 0x7E, 0x00, // CIN=4: SysEx開始/継続(3バイト)
        0x07, 0x06, 0x01, 0xF7  // CIN=7: SysExがこのパケットの3バイト目で終わる
    };
    send_usb_midi_packets(g5n_dev_addr, g5n_midi_ep_out, packets, sizeof(packets));
}

// パッチ切り替えのMIDI(Bank MSB/LSB + Program Change)を手動パスで送信
void send_g5n_midi_manual() {
    if (current_bank == 0) return;
    if (!g5n_midi_manual_ready) return;

    int target_idx = 5 * (current_bank - 1) + (current_patch - 1);
    uint8_t bank_lsb = (uint8_t)(target_idx / 4);
    uint8_t pc_val = (uint8_t)(target_idx % 4);

    uint8_t packets[12] = {
        0x0B, 0xB0, 0x00, 0x00,      // CC0(Bank MSB)=0
        0x0B, 0xB0, 0x20, bank_lsb,  // CC32(Bank LSB)
        0x0C, 0xC0, pc_val, 0x00     // Program Change
    };
    send_usb_midi_packets(g5n_dev_addr, g5n_midi_ep_out, packets, sizeof(packets));
}

// ----------------------------------------------------
// フラッシュへの保存・復元 (patch_dataを電源断後も保持)
// ----------------------------------------------------
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE) // フラッシュ末尾の1セクタを使用
#define FLASH_DATA_MAGIC 0x47354E31u // "G5N1"。有効なデータかどうかの目印

struct FlashData {
    uint32_t magic;
    bool patch_data[27][5][6];
};

// flash_range_programは256バイト単位でしか書けないので、その倍数に切り上げたバッファを使う
static uint8_t flash_write_buf[((sizeof(FlashData) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE];

void save_patch_data_to_flash() {
    FlashData data;
    data.magic = FLASH_DATA_MAGIC;
    memcpy(data.patch_data, patch_data, sizeof(patch_data));

    memset(flash_write_buf, 0, sizeof(flash_write_buf));
    memcpy(flash_write_buf, &data, sizeof(data));

    uint32_t ints = save_and_disable_interrupts(); // 消去/書き込み中はflash上のコードを実行できないため割り込み禁止
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, flash_write_buf, sizeof(flash_write_buf));
    restore_interrupts(ints);
}

void load_patch_data_from_flash() {
    const FlashData *stored = (const FlashData *)(XIP_BASE + FLASH_TARGET_OFFSET);
    if (stored->magic == FLASH_DATA_MAGIC) {
        memcpy(patch_data, stored->patch_data, sizeof(patch_data));
    }
    // magicが一致しない場合(工場出荷後の初回起動など)はpatch_dataを初期値(全部false)のままにする
}

// ----------------------------------------------------
// リレー切り替え & ミュート処理
// ----------------------------------------------------
void apply_current_patch() {
    // 1. フォトモス導通 (Mute ON)
    gpio_put(PIN_MUTE, true);
    sleep_ms(2);

    // 2. リレー状態適用
    for (int i = 0; i < 6; i++) {
        gpio_put(RELAY_PINS[i], patch_data[current_bank][current_patch - 1][i]);
    }

    // 3. MIDI送信
    send_g5n_midi();

    // 4. フォトモス遮断 (Mute OFF)
    sleep_ms(8); //ポップノイズとのチキンレース、ポップノイズが聞こえたらもっと長めにとる
    gpio_put(PIN_MUTE, false);
}

// ----------------------------------------------------
// LEDアニメーション・ロジック（ノンブロッキング）
// ----------------------------------------------------
void update_leds() {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (current_mode == MODE_PATCH) {
        if (trigger_bank_anim) {
            // バンク表示アニメーション（3進法: 9の位 -> 3の位 -> 1の位）
            int d9 = current_bank / 9;
            int d3 = (current_bank % 9) / 3;
            int d1 = current_bank % 3;

            uint32_t elapsed = now - anim_start_time;
            if (elapsed < 300) set_leds(d9==0, d9==1, d9==2);
            else if (elapsed < 400) set_leds(0,0,0);
            else if (elapsed < 700) set_leds(d3==0, d3==1, d3==2);
            else if (elapsed < 800) set_leds(0,0,0);
            else if (elapsed < 1100) set_leds(d1==0, d1==1, d1==2);
            else {
                trigger_bank_anim = false; // アニメ終了
            }
        } else {
            // 通常パッチ表示
            switch (current_patch) {
                case 1: set_leds(1, 0, 0); break;
                case 2: set_leds(0, 1, 0); break;
                case 3: set_leds(0, 0, 1); break;
                case 4: set_leds(1, 1, 0); break;
                case 5: set_leds(0, 1, 1); break;
            }
        }
    } 
    else if (current_mode == MODE_BANK) {
        // バンクモード時はループ表示 (小休止を挟む)
        uint32_t cycle = now % 1600;
        int d9 = current_bank / 9;
        int d3 = (current_bank % 9) / 3;
        int d1 = current_bank % 3;

        if (cycle < 300) set_leds(d9==0, d9==1, d9==2);
        else if (cycle < 400) set_leds(0,0,0);
        else if (cycle < 700) set_leds(d3==0, d3==1, d3==2);
        else if (cycle < 800) set_leds(0,0,0);
        else if (cycle < 1100) set_leds(d1==0, d1==1, d1==2);
        else set_leds(0,0,0); // 小休止
    }
    else if (current_mode == MODE_PROGRAM) {
        // プログラムモード：選択中のループに対応するLEDを点滅
        bool is_on = patch_data[current_bank][current_patch - 1][edit_loop_idx];
        uint32_t blink_rate = is_on ? 100 : 500; // ONなら早い、OFFなら遅い
        bool blink_state = (now / blink_rate) % 2 == 0;

        if (!blink_state) {
            set_leds(0, 0, 0);
        } else {
            switch (edit_loop_idx) {
                case 0: set_leds(1, 0, 0); break; // L1 (LED1)
                case 1: set_leds(0, 1, 0); break; // L2 (LED2)
                case 2: set_leds(0, 0, 1); break; // L3 (LED3)
                case 3: set_leds(1, 1, 0); break; // L4 (LED12)
                case 4: set_leds(0, 1, 1); break; // L5 (LED23)
                case 5: set_leds(1, 1, 1); break; // Remote (LED123)
            }
        }
    }
}

// ----------------------------------------------------
// スイッチ入力・状態遷移ロジック
// ----------------------------------------------------
void process_switches() {
    static uint32_t detect_start = 0;
    static int pending_sw = 0; // 同時押し判定待ちのスイッチ状態
    static uint32_t press_start = 0;
    static int active_sw = 0;
    static bool hold_processed = false;
    static bool super_hold_processed = false;

    bool s1 = IS_PRESSED(SW_PINS[0]);
    bool s2 = IS_PRESSED(SW_PINS[1]);
    bool s3 = IS_PRESSED(SW_PINS[2]);

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (active_sw == 0) {
        // 現在押されているスイッチをビット演算でまとめる (SW1=1, SW2=2, SW3=4)
        int current_bits = (s1 ? 1 : 0) | (s2 ? 2 : 0) | (s3 ? 4 : 0);
        
        if (current_bits > 0) {
            if (pending_sw == 0) detect_start = now; // 最初に押された瞬間
            pending_sw |= current_bits;              // 押されたスイッチを蓄積
            
            // 100ms経過したら「単押しか同時押しか」を確定させる
            if (now - detect_start > 100) {
                if ((pending_sw & 3) == 3) active_sw = 12;      // SW1(1) + SW2(2)
                else if ((pending_sw & 6) == 6) active_sw = 23; // SW2(2) + SW3(4)
                else if (pending_sw & 1) active_sw = 1;
                else if (pending_sw & 2) active_sw = 2;
                else if (pending_sw & 4) active_sw = 3;
                
                press_start = now;
                hold_processed = false;
                super_hold_processed = false;
                trigger_bank_anim = false;
                pending_sw = 0; // 判定が終わったのでリセット
            }
        } else {
            pending_sw = 0; // 100ms未満で離した場合はリセット
        }
    } else {
        // --- ここから下は既存のコードと同じです ---
        uint32_t duration = now - press_start;
        bool all_released = (!s1 && !s2 && !s3);

        if (!hold_processed && duration > 600) {
            // (長押し判定... 省略せずに残してください)
            if (current_mode == MODE_PATCH) {
                if (active_sw == 1) { current_bank = (current_bank - 1 < 0) ? 26 : current_bank - 1; current_patch = 1; trigger_bank_anim = true; anim_start_time = now; apply_current_patch(); }
                else if (active_sw == 3) { current_bank = (current_bank + 1 > 26) ? 0 : current_bank + 1; current_patch = 1; trigger_bank_anim = true; anim_start_time = now; apply_current_patch(); }
                else if (active_sw == 2) { current_mode = MODE_BANK; }
            } else if (current_mode == MODE_PROGRAM) {
                if (active_sw == 2) {
                    current_mode = MODE_PATCH;
                    apply_current_patch();
                    // MIDI送信が完全に終わるまでUSBホストタスクを回して待つ(この直後にflashで割り込みを止めるため)
                    for (int i = 0; i < 20; i++) { tuh_task(); sleep_ms(2); }
                    save_patch_data_to_flash();
                }
            }
            hold_processed = true;
        }

        if (hold_processed && !super_hold_processed && duration > 1500) {
            if (current_mode == MODE_BANK && active_sw == 2) { 
                current_mode = MODE_PROGRAM; edit_loop_idx = 0; 
            }
            super_hold_processed = true;
        }

        if (all_released) {
            if (!hold_processed && duration > 30) {
                if (current_mode == MODE_PATCH) {
                    if (active_sw == 1) current_patch = 1;
                    if (active_sw == 2) current_patch = 2;
                    if (active_sw == 3) current_patch = 3;
                    if (active_sw == 12) current_patch = 4;
                    if (active_sw == 23) current_patch = 5;
                    apply_current_patch();
                } 
                else if (current_mode == MODE_BANK) {
                    if (active_sw == 1) current_bank = (current_bank - 1 < 0) ? 26 : current_bank - 1;
                    if (active_sw == 3) current_bank = (current_bank + 1 > 26) ? 0 : current_bank + 1;

                    if (active_sw == 2) { 
                        current_mode = MODE_PATCH; 
                        current_patch = 1; 
                        trigger_bank_anim = false;
                        //パッチ適用時のアニメーションは不要なのでtrigger_bank_animをfalseにする
                        //trigger_bank_anim = true; 
                        //anim_start_time = now; 
                        apply_current_patch(); 
                    }
                }
                else if (current_mode == MODE_PROGRAM) {
                    if (active_sw == 1) edit_loop_idx = (edit_loop_idx - 1 < 0) ? 5 : edit_loop_idx - 1;
                    if (active_sw == 3) edit_loop_idx = (edit_loop_idx + 1 > 5) ? 0 : edit_loop_idx + 1;
                    if (active_sw == 2) {
                        bool &state = patch_data[current_bank][current_patch - 1][edit_loop_idx];
                        state = !state;
                        gpio_put(PIN_MUTE, true); sleep_ms(20);
                        gpio_put(RELAY_PINS[edit_loop_idx], state);
                        sleep_ms(20); gpio_put(PIN_MUTE, false);
                    }
                }
            }
            active_sw = 0; // リセット
        }
    }
}
    

// 診断用: USBデバイスが種類を問わず認識された時(MIDIかどうかはまだ関係ない)
void tuh_mount_cb(uint8_t daddr) {
    // ゆっくり2回点滅 = 「USB機器としては認識した」の合図
    for (int i = 0; i < 2; i++) {
        set_leds(true, true, true); sleep_ms(150);
        set_leds(false, false, false); sleep_ms(150);
    }
    sleep_ms(400);

    g5n_dev_addr = daddr;

    // tinyusbの自動MIDIクラス判定をバイパスして自前でMIDIStreamingインターフェースを掴む
    if (claim_midi_interface_manually(daddr)) {
        g5n_midi_manual_ready = true;
        sleep_ms(300);
        send_g5n_sysex_handshake_manual();

        // 掴めた合図: 4灯まとめて素早く3回
        for (int i = 0; i < 3; i++) {
            set_leds(true, true, true); board_led_write(true); sleep_ms(120);
            set_leds(false, false, false); board_led_write(false); sleep_ms(120);
        }
    } else {
        // 掴めなかった合図: 4灯まとめてゆっくり4回
        for (int i = 0; i < 4; i++) {
            set_leds(true, true, true); board_led_write(true); sleep_ms(400);
            set_leds(false, false, false); board_led_write(false); sleep_ms(400);
        }
    }
}

// デバイスが取り外された時(種類を問わない汎用コールバック)
void tuh_umount_cb(uint8_t daddr) {
    if (daddr == g5n_dev_addr) {
        g5n_midi_manual_ready = false;
    }
}

// TinyUSB コールバック: G5n等デバイスがマウントされた時
void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx, uint16_t num_cables_tx) {
    // 接続されたデバイスのアドレスを記憶
    g5n_dev_addr = dev_addr;
    is_g5n_mounted = true;

    // 診断用: 素早く4回点滅 = 「MIDIデバイスとして認識した」の合図
    for (int i = 0; i < 4; i++) {
        set_leds(true, true, true); sleep_ms(80);
        set_leds(false, false, false); sleep_ms(80);
    }

    // Zoom機器特有の起動リクエスト
    uint8_t sysex_init[] = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
    
    // 引数を4つに修正
    tuh_midi_stream_write(dev_addr, 0, sysex_init, sizeof(sysex_init));
    tuh_midi_write_flush(dev_addr);
}

// 追記：デバイスが取り外された時
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (dev_addr == g5n_dev_addr) {
        is_g5n_mounted = false;
    }
}

void send_g5n_midi() {
    if (current_bank == 0) return;

    // 手動で掴めていればそちらを優先(tinyusbの自動MIDIクラス判定が効かないため)
    if (g5n_midi_manual_ready) {
        send_g5n_midi_manual();
        return;
    }

    if (!is_g5n_mounted) return; // 未接続時に送らないガード

    int target_idx = 5 * (current_bank - 1) + (current_patch - 1);
    uint8_t cc_msb[3] = {0xB0, 0x00, 0x00};
    uint8_t cc_lsb[3] = {0xB0, 0x20, (uint8_t)(target_idx / 4)};  // ※3で後述
    uint8_t pc_msg[2] = {0xC0, (uint8_t)(target_idx % 4)};         // ※3で後述

    tuh_midi_stream_write(g5n_dev_addr, 0, cc_msb, 3);
    tuh_midi_stream_write(g5n_dev_addr, 0, cc_lsb, 3);
    tuh_midi_stream_write(g5n_dev_addr, 0, pc_msg, 2);
    tuh_midi_write_flush(g5n_dev_addr);
}

int main() {
    init_hardware();
    load_patch_data_from_flash(); // 電源断前のループ設定を復元

    // 起動時はバンク0、パッチ1に飛ぶ
    current_bank = 0;
    current_patch = 1;
    apply_current_patch();

    while (true) {
        tuh_task();         // USB MIDI ホストタスク
        process_switches(); // スイッチの入力・遷移処理
        update_leds();      // LEDアニメーション更新
        
        sleep_ms(2);        // CPU負荷軽減
    }
    return 0;
}