// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <utility>
#include "input_common/keyboard.h"

namespace InputCommon {

class KeyButton final : public Input::ButtonDevice {
public:
    explicit KeyButton(std::atomic<bool>& _status) : status(_status) {}

    ~KeyButton() override = default;

    bool GetStatus() const override {
        return status.load();
    }

    friend class KeyButtonList;

private:
    std::atomic<bool>& status;
};

struct KeyButtonPair {
    explicit KeyButtonPair(int _key_code) : key_code(_key_code) {}
    int key_code;
    std::atomic<bool> status{false};
};

class KeyButtonList {
public:
    KeyButtonPair& AddKeyButton(int key_code) {
        std::lock_guard guard{mutex};
        for (KeyButtonPair& pair : list) {
            if (pair.key_code == key_code)
                return pair;
        }
        return list.emplace_back(key_code);
    }

    void ChangeKeyStatus(int key_code, bool pressed) {
        std::lock_guard guard{mutex};
        for (KeyButtonPair& pair : list) {
            if (pair.key_code == key_code)
                pair.status.store(pressed);
        }
    }

    void ChangeAllKeyStatus(bool pressed) {
        std::lock_guard guard{mutex};
        for (KeyButtonPair& pair : list) {
            pair.status.store(pressed);
        }
    }

private:
    std::mutex mutex;
    std::list<KeyButtonPair> list;
};

Keyboard::Keyboard() : key_button_list{std::make_shared<KeyButtonList>()} {}

std::unique_ptr<Input::ButtonDevice> Keyboard::Create(const Common::ParamPackage& params) {
    int key_code = params.Get("code", 0);
    auto& pair = key_button_list->AddKeyButton(key_code);
    return std::make_unique<KeyButton>(pair.status);
}

void Keyboard::PressKey(int key_code) {
    key_button_list->ChangeKeyStatus(key_code, true);
}

void Keyboard::ReleaseKey(int key_code) {
    key_button_list->ChangeKeyStatus(key_code, false);
}

void Keyboard::ReleaseAllKeys() {
    key_button_list->ChangeAllKeyStatus(false);
}

} // namespace InputCommon
