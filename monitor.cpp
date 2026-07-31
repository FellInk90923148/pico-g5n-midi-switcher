#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
#include "RtMidi.h"
using namespace std;

// MIDI信号を受信したときに実行されるコールバック関数
void midiCallback(double deltatime, vector<unsigned char> *message, void *userData) {
    unsigned int nBytes = message->size();
    if (nBytes == 0) return;

    // 受信したバイト列を16進数で綺麗にダンプする
    for (unsigned int i = 0; i < nBytes; i++) {
        printf("%02X ", message->at(i));
    }
    printf("\n");
}

int main() {
    RtMidiIn *midiin = nullptr;
    try {
        midiin = new RtMidiIn();
    } catch (RtMidiError &error) {
        error.printMessage();
        return 1;
    }

    // ZOOM G5n のポートを探す
    unsigned int nPorts = midiin->getPortCount();
    int g5nPort = -1;
    for (unsigned int i = 0; i < nPorts; i++) {
        if (midiin->getPortName(i).find("ZOOM") != string::npos) {
            g5nPort = i;
            break;
        }
    }

    if (g5nPort == -1) {
        cerr << "G5nが見つかりません！USB接続を確認してください。" << endl;
        delete midiin;
        return 1;
    }

    // ポートを開いてコールバックを登録
    midiin->openPort(g5nPort);
    midiin->setCallback(&midiCallback);

    // デフォルトでスルーされてしまうSysEx（システムエクスクルーシブ）を「無視しない」設定にする
    midiin->ignoreTypes(false, true, true);

    cout << "G5nのSysEx監視を開始しました。 [Ctrl + C で終了]" << endl;

    // プログラムが終了しないように無限ループ（適当に待機）
    while (true) {
        // 100msスリープ（Macのunistd.hなどでsleepしても良いですが、簡易的に）
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }

    delete midiin;
    return 0;
}