#include <iostream>
#include <vector>
#include <string>
#include <unistd.h>
#include "RtMidi.h"

int main() {
    RtMidiOut *midiout = nullptr;
    try {
        midiout = new RtMidiOut();
    } catch (RtMidiError &error) {
        error.printMessage();
        return 1;
    }

    // ZOOM G5n の出力ポートを探す
    unsigned int nPorts = midiout->getPortCount();
    int g5nPort = -1;
    for (unsigned int i = 0; i < nPorts; i++) {
        if (midiout->getPortName(i).find("ZOOM") != std::string::npos) {
            g5nPort = i;
            break;
        }
    }

    if (g5nPort == -1) {
        std::cerr << "G5nが見つかりません！USB接続を確認してください。" << std::endl;
        delete midiout;
        return 1;
    }

    // ポートを開く
    midiout->openPort(g5nPort);
    std::cout << "G5nへの接続に成功しました。パッチを145番に切り替えます..." << std::endl;

    // 先ほど解析したパッチ145番（バンク36, パッチ0）の3つのメッセージ
    std::vector<unsigned char> msg1 = {0xB0, 0x00, 0x00}; // Bank Select MSB
    std::vector<unsigned char> msg2 = {0xB0, 0x20, 0x24}; // Bank Select LSB (0x24 = 36)
    std::vector<unsigned char> msg3 = {0xC0, 0x00};       // Program Change (0x00 = パッチA)

    // 順番に送信
    midiout->sendMessage(&msg1);
    usleep(10000); // 念のため10ms待機
    midiout->sendMessage(&msg2);
    usleep(10000); // 念のため10ms待機
    midiout->sendMessage(&msg3);

    std::cout << "送信完了！G5nの画面を確認してください。" << std::endl;

    delete midiout;
    return 0;
}