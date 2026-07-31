#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

// -----------------------------------------------------------------------------
// System Configuration
// -----------------------------------------------------------------------------
// 動作OSの指定（Pico SDK環境）
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_PICO
#endif

// -----------------------------------------------------------------------------
// Host Configuration (PicoをUSBホストとして動かす設定)
// -----------------------------------------------------------------------------
// RP2040のUSBポート0をホストモードかつフルスピードに設定
#define CFG_TUSB_MCU OPT_MCU_RP2040
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)

// ホストスタックの有効化
#define CFG_TUH_ENABLED         1

// デバイス列挙時のバッファサイズ
#define CFG_TUH_ENUMERATION_BUFSIZE 2048

// デバッグ用: 詳細ログが必要な時は2にする(UART GPIO28/29使用)
#define CFG_TUSB_DEBUG 0

#define CFG_TUH_DEVICE_MAX 4
#define CFG_TUH_MIDI CFG_TUH_DEVICE_MAX

// 手動でMIDIStreamingインターフェースを掴んで生転送するためのAPIを有効化
#define CFG_TUH_API_EDPT_XFER 1

// -----------------------------------------------------------------------------
// Host Class Configuration (使用するUSBクラスの設定)
// -----------------------------------------------------------------------------
// MIDI ホストクラスを有効化 (1 = 有効, 0 = 無効)
#define CFG_TUH_MIDI            1

// 今回使わない他のクラスはメモリ節約のためすべて無効(0)にします
#define CFG_TUH_HUB             0
#define CFG_TUH_HID             0
#define CFG_TUH_MSC             0
#define CFG_TUH_CDC             0
#define CFG_TUH_VENDOR          0

// -----------------------------------------------------------------------------
// MIDI Configuration (MIDI通信用のバッファサイズ)
// -----------------------------------------------------------------------------
// G5nからの受信(RX)・送信(TX)バッファ
#define CFG_TUH_MIDI_RX_BUFSIZE 64
#define CFG_TUH_MIDI_TX_BUFSIZE 64
#define CFG_TUH_MIDI_EP_BUFSIZE 64

// -----------------------------------------------------------------------------
// Compiler/Memory Configuration
// -----------------------------------------------------------------------------
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))
#endif

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */